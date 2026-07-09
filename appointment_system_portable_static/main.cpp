
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <sqlite3.h>
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

// Direct compile commands for Windows:
// MSVC (Developer Command Prompt):
//   cl /EHsc /std:c++14 main.cpp sqlite3.c /link ws2_32.lib
// MinGW/g++:
//   g++ -std=c++11 main.cpp sqlite3.c -o appointment_server.exe -lws2_32

sqlite3* db = nullptr;
std::mutex db_mutex;

std::string safe_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool is_phone_like(const std::string& phone) {
    if (phone.size() < 6 || phone.size() > 20) return false;
    return std::all_of(phone.begin(), phone.end(), [](unsigned char c){ return std::isdigit(c) || c == '+' || c == '-'; });
}

bool is_valid_datetime(const std::string& v) {
    if (v.size() != 16) return false;
    if (v[4] != '-' || v[7] != '-' || v[10] != ' ' || v[13] != ':') return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13) continue;
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
    }
    int month = std::stoi(v.substr(5, 2));
    int day = std::stoi(v.substr(8, 2));
    int hour = std::stoi(v.substr(11, 2));
    int minute = std::stoi(v.substr(14, 2));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

int get_day_of_week(const std::string& appointment_time) {
    if (appointment_time.size() < 10) return -1;
    std::tm tm = {};
    std::istringstream ss(appointment_time.substr(0, 10));
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) return -1;
    tm.tm_isdst = -1;
    std::mktime(&tm);
    return tm.tm_wday; // 0 Sunday, 1 Monday ... 6 Saturday
}


void open_home_page() {
#ifdef _WIN32
    std::system("start \"\" \"http://127.0.0.1:8080\"");
#elif __APPLE__
    std::system("open \"http://127.0.0.1:8080\"");
#else
    std::system("xdg-open \"http://127.0.0.1:8080\" >/dev/null 2>&1 &");
#endif
}

void set_json(httplib::Response& res, const json& obj, int status = 200) {
    res.status = status;
    res.set_content(obj.dump(), "application/json; charset=utf-8");
}

bool exec_sql(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite error: " << (err ? err : "unknown") << "\nSQL: " << sql << std::endl;
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool table_has_column(const std::string& table_name, const std::string& column_name) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "PRAGMA table_info(" + table_name + ");";
    bool exists = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (safe_text(stmt, 1) == column_name) { exists = true; break; }
        }
    }
    sqlite3_finalize(stmt);
    return exists;
}

void add_column_if_missing(const std::string& table, const std::string& column, const std::string& ddl) {
    if (!table_has_column(table, column)) exec_sql("ALTER TABLE " + table + " ADD COLUMN " + ddl + ";");
}

int scalar_int(const std::string& sql, const std::vector<std::string>& params = {}) {
    sqlite3_stmt* stmt = nullptr;
    int val = 0;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        for (size_t i = 0; i < params.size(); ++i) sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) val = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return val;
}

std::string status_to_string(int status) {
    switch (status) {
        case 1: return "待商家确认";
        case 2: return "已确认";
        case 3: return "已取消";
        case 4: return "已完成";
        default: return "未知状态";
    }
}

void add_notification(const std::string& receiver_type, int receiver_id, int appointment_id, const std::string& message) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO t_notification (ReceiverType, ReceiverID, AppointmentID, Message, Status, CreatedAt) VALUES (?, ?, ?, ?, 0, datetime('now','localtime'));";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, receiver_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, receiver_id);
        sqlite3_bind_int(stmt, 3, appointment_id);
        sqlite3_bind_text(stmt, 4, message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

int get_service_merchant_id(int service_id) {
    sqlite3_stmt* stmt = nullptr;
    int id = 0;
    const char* sql = "SELECT MerchantID FROM t_service WHERE ServiceID=? AND IsActive=1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, service_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

int get_service_capacity(int service_id) {
    sqlite3_stmt* stmt = nullptr;
    int cap = 1;
    const char* sql = "SELECT COALESCE(Capacity,1) FROM t_service WHERE ServiceID=? AND IsActive=1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, service_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) cap = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return cap > 0 ? cap : 1;
}

bool is_working_day(int merchant_id, const std::string& appointment_time) {
    int dow = get_day_of_week(appointment_time);
    if (dow < 0) return false;
    sqlite3_stmt* stmt = nullptr;
    bool working = true;
    const char* sql = "SELECT IsWorkingDay FROM t_schedule WHERE MerchantID=? AND DayOfWeek=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, merchant_id);
        sqlite3_bind_int(stmt, 2, dow);
        if (sqlite3_step(stmt) == SQLITE_ROW) working = sqlite3_column_int(stmt, 0) == 1;
    }
    sqlite3_finalize(stmt);
    return working;
}

json appointment_json_from_stmt(sqlite3_stmt* stmt) {
    json item;
    int status = sqlite3_column_int(stmt, 4);
    item["AppointmentID"] = sqlite3_column_int(stmt, 0);
    item["UserID"] = sqlite3_column_int(stmt, 1);
    item["ServiceID"] = sqlite3_column_int(stmt, 2);
    item["AppointmentTime"] = safe_text(stmt, 3);
    item["Status"] = status;
    item["StatusText"] = status_to_string(status);
    item["Username"] = safe_text(stmt, 5);
    item["PhoneNumber"] = safe_text(stmt, 6);
    item["ServiceName"] = safe_text(stmt, 7);
    item["Category"] = safe_text(stmt, 8);
    item["Price"] = sqlite3_column_int(stmt, 9);
    item["Duration"] = sqlite3_column_int(stmt, 10);
    item["MerchantID"] = sqlite3_column_int(stmt, 11);
    item["ShopName"] = safe_text(stmt, 12);
    item["HasReview"] = sqlite3_column_int(stmt, 13) > 0;
    return item;
}

std::string read_html_file(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.is_open()) return "<h1>index.html not found</h1>";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void init_db() {
    if (sqlite3_open("appointment.db", &db) != SQLITE_OK) {
        std::cerr << "Failed to open database" << std::endl;
        std::exit(1);
    }
    exec_sql("PRAGMA foreign_keys = ON;");
    exec_sql("CREATE TABLE IF NOT EXISTS t_merchant (MerchantID INTEGER PRIMARY KEY AUTOINCREMENT, ShopName TEXT NOT NULL, ContactInfo TEXT, Address TEXT, MerchantPhone TEXT UNIQUE, Password TEXT, CreatedAt TEXT DEFAULT (datetime('now','localtime')));");
    exec_sql("CREATE TABLE IF NOT EXISTS t_user (UserID INTEGER PRIMARY KEY AUTOINCREMENT, Username TEXT NOT NULL, PhoneNumber TEXT UNIQUE NOT NULL, Password TEXT NOT NULL, CreatedAt TEXT DEFAULT (datetime('now','localtime')));");
    exec_sql("CREATE TABLE IF NOT EXISTS t_service (ServiceID INTEGER PRIMARY KEY AUTOINCREMENT, MerchantID INTEGER NOT NULL, ServiceName TEXT NOT NULL, Category TEXT, Price INTEGER NOT NULL, Duration INTEGER NOT NULL, Capacity INTEGER DEFAULT 1, IsActive INTEGER DEFAULT 1, CreatedAt TEXT DEFAULT (datetime('now','localtime')));");
    exec_sql("CREATE TABLE IF NOT EXISTS t_appointment (AppointmentID INTEGER PRIMARY KEY AUTOINCREMENT, UserID INTEGER NOT NULL, ServiceID INTEGER NOT NULL, AppointmentTime TEXT NOT NULL, Status INTEGER DEFAULT 1, CreatedAt TEXT DEFAULT (datetime('now','localtime')));");
    exec_sql("CREATE TABLE IF NOT EXISTS t_review (ReviewID INTEGER PRIMARY KEY AUTOINCREMENT, AppointmentID INTEGER UNIQUE NOT NULL, Rating INTEGER NOT NULL, CommentContent TEXT, CreatedAt TEXT DEFAULT (datetime('now','localtime')));");
    exec_sql("CREATE TABLE IF NOT EXISTS t_schedule (ScheduleID INTEGER PRIMARY KEY AUTOINCREMENT, MerchantID INTEGER NOT NULL, DayOfWeek INTEGER NOT NULL, IsWorkingDay INTEGER DEFAULT 1, UNIQUE(MerchantID, DayOfWeek));");
    exec_sql("CREATE TABLE IF NOT EXISTS t_notification (NotificationID INTEGER PRIMARY KEY AUTOINCREMENT, ReceiverType TEXT DEFAULT 'merchant', ReceiverID INTEGER DEFAULT 1, AppointmentID INTEGER, Message TEXT NOT NULL, Status INTEGER DEFAULT 0, CreatedAt TEXT DEFAULT (datetime('now','localtime')));");

    add_column_if_missing("t_merchant", "MerchantPhone", "MerchantPhone TEXT");
    add_column_if_missing("t_merchant", "Password", "Password TEXT");
    add_column_if_missing("t_merchant", "CreatedAt", "CreatedAt TEXT DEFAULT ''");
    add_column_if_missing("t_service", "Capacity", "Capacity INTEGER DEFAULT 1");
    add_column_if_missing("t_service", "IsActive", "IsActive INTEGER DEFAULT 1");
    add_column_if_missing("t_service", "CreatedAt", "CreatedAt TEXT DEFAULT ''");
    add_column_if_missing("t_appointment", "CreatedAt", "CreatedAt TEXT DEFAULT ''");
    add_column_if_missing("t_review", "CreatedAt", "CreatedAt TEXT DEFAULT ''");
    add_column_if_missing("t_notification", "ReceiverType", "ReceiverType TEXT DEFAULT 'merchant'");
    add_column_if_missing("t_notification", "ReceiverID", "ReceiverID INTEGER DEFAULT 1");
    add_column_if_missing("t_notification", "CreatedAt", "CreatedAt TEXT DEFAULT ''");

    exec_sql("CREATE INDEX IF NOT EXISTS idx_app_slot ON t_appointment(ServiceID, AppointmentTime, Status);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_app_user ON t_appointment(UserID, Status);");
    exec_sql("CREATE INDEX IF NOT EXISTS idx_service_merchant ON t_service(MerchantID, IsActive);");

    int merchants = scalar_int("SELECT COUNT(*) FROM t_merchant;");
    if (merchants == 0) {
        exec_sql("INSERT INTO t_merchant (ShopName, ContactInfo, Address, MerchantPhone, Password) VALUES ('示例医美健康中心','13800001111','学校东门商业街 2 楼','13800001111','123456');");
    } else {
        exec_sql("UPDATE t_merchant SET MerchantPhone=COALESCE(NULLIF(MerchantPhone,''), ContactInfo), Password=COALESCE(NULLIF(Password,''),'123456') WHERE MerchantID=1;");
    }

    int users = scalar_int("SELECT COUNT(*) FROM t_user;");
    if (users == 0) {
        exec_sql("INSERT INTO t_user (Username, PhoneNumber, Password) VALUES ('测试用户','13311112222','123456');");
    }
    int active_services = scalar_int("SELECT COUNT(*) FROM t_service WHERE IsActive=1;");
    if (active_services == 0) {
        exec_sql("INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration, Capacity, IsActive) VALUES (1,'体检预约','医疗健康',99,30,3,1);");
        exec_sql("INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration, Capacity, IsActive) VALUES (1,'皮肤护理','美容护理',128,60,2,1);");
        exec_sql("INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration, Capacity, IsActive) VALUES (1,'私教体验课','健身运动',80,45,4,1);");
    }
    for (int d = 0; d <= 6; ++d) {
        int working = (d == 0 || d == 6) ? 0 : 1;
        std::ostringstream os;
        os << "INSERT OR IGNORE INTO t_schedule (MerchantID, DayOfWeek, IsWorkingDay) VALUES (1," << d << "," << working << ");";
        exec_sql(os.str());
    }
}

int main() {
    init_db();
    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(read_html_file("index.html"), "text/html; charset=utf-8");
    });

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res){ set_json(res, {{"success", true}, {"message", "server ok"}}); });

    svr.Post("/api/auth/register", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto data = json::parse(req.body);
            std::string username = trim(data.value("Username", ""));
            std::string phone = trim(data.value("PhoneNumber", ""));
            std::string password = data.value("Password", "");
            if (username.empty() || phone.empty() || password.empty()) return set_json(res, {{"success", false}, {"message", "请完整填写用户名、手机号和密码"}}, 400);
            if (!is_phone_like(phone)) return set_json(res, {{"success", false}, {"message", "手机号格式不正确"}}, 400);
            if (password.size() < 6) return set_json(res, {{"success", false}, {"message", "密码至少 6 位"}}, 400);
            if (scalar_int("SELECT COUNT(*) FROM t_user WHERE PhoneNumber=?;", {phone}) > 0) return set_json(res, {{"success", false}, {"message", "该手机号已注册"}}, 400);
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO t_user (Username, PhoneNumber, Password, CreatedAt) VALUES (?, ?, ?, datetime('now','localtime'));";
            bool ok = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, phone.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, password.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
            if (!ok) return set_json(res, {{"success", false}, {"message", "注册失败，请检查数据库"}}, 500);
            int uid = static_cast<int>(sqlite3_last_insert_rowid(db));
            set_json(res, {{"success", true}, {"message", "注册成功"}, {"user", {{"UserID", uid}, {"Username", username}, {"PhoneNumber", phone}}}});
        } catch (const std::exception& e) { set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Post("/api/auth/login", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto data = json::parse(req.body);
            std::string phone = trim(data.value("PhoneNumber", ""));
            std::string password = data.value("Password", "");
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT UserID, Username, PhoneNumber FROM t_user WHERE PhoneNumber=? AND Password=?;";
            json ret = {{"success", false}, {"message", "手机号或密码错误"}};
            int status = 401;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, phone.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    status = 200;
                    ret = {{"success", true}, {"message", "登录成功"}, {"user", {{"UserID", sqlite3_column_int(stmt,0)}, {"Username", safe_text(stmt,1)}, {"PhoneNumber", safe_text(stmt,2)}}}};
                }
            }
            sqlite3_finalize(stmt);
            set_json(res, ret, status);
        } catch (const std::exception& e) { set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Post("/api/merchant/register", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d = json::parse(req.body);
            std::string shop = trim(d.value("ShopName", ""));
            std::string phone = trim(d.value("MerchantPhone", ""));
            std::string password = d.value("Password", "");
            std::string address = trim(d.value("Address", ""));
            if (shop.empty() || phone.empty() || password.empty()) return set_json(res, {{"success", false}, {"message", "请填写店铺名、手机号和密码"}}, 400);
            if (!is_phone_like(phone) || password.size() < 6) return set_json(res, {{"success", false}, {"message", "手机号或密码格式不正确"}}, 400);
            if (scalar_int("SELECT COUNT(*) FROM t_merchant WHERE MerchantPhone=?;", {phone}) > 0) return set_json(res, {{"success", false}, {"message", "该商家手机号已注册"}}, 400);
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO t_merchant (ShopName, ContactInfo, Address, MerchantPhone, Password, CreatedAt) VALUES (?, ?, ?, ?, ?, datetime('now','localtime'));";
            bool ok = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, shop.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, phone.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, address.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, phone.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, password.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
            if (!ok) return set_json(res, {{"success", false}, {"message", "商家注册失败"}}, 500);
            int mid = static_cast<int>(sqlite3_last_insert_rowid(db));
            for (int day = 0; day <= 6; ++day) {
                sqlite3_stmt* s = nullptr;
                const char* isql = "INSERT INTO t_schedule (MerchantID, DayOfWeek, IsWorkingDay) VALUES (?, ?, ?);";
                if (sqlite3_prepare_v2(db, isql, -1, &s, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(s, 1, mid); sqlite3_bind_int(s, 2, day); sqlite3_bind_int(s, 3, (day==0||day==6)?0:1); sqlite3_step(s);
                }
                sqlite3_finalize(s);
            }
            set_json(res, {{"success", true}, {"message", "商家注册成功"}, {"merchant", {{"MerchantID", mid}, {"ShopName", shop}, {"MerchantPhone", phone}, {"Address", address}}}});
        } catch (const std::exception& e) { set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Post("/api/merchant/login", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d = json::parse(req.body);
            std::string phone = trim(d.value("MerchantPhone", ""));
            std::string password = d.value("Password", "");
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT MerchantID, ShopName, ContactInfo, Address, MerchantPhone FROM t_merchant WHERE MerchantPhone=? AND Password=?;";
            json ret = {{"success", false}, {"message", "商家手机号或密码错误"}};
            int status = 401;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, phone.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    status = 200;
                    ret = {{"success", true}, {"message", "商家登录成功"}, {"merchant", {{"MerchantID", sqlite3_column_int(stmt,0)}, {"ShopName", safe_text(stmt,1)}, {"ContactInfo", safe_text(stmt,2)}, {"Address", safe_text(stmt,3)}, {"MerchantPhone", safe_text(stmt,4)}}}};
                }
            }
            sqlite3_finalize(stmt);
            set_json(res, ret, status);
        } catch (const std::exception& e) { set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Get("/api/services", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string keyword = req.has_param("keyword") ? trim(req.get_param_value("keyword")) : "";
        std::string category = req.has_param("category") ? trim(req.get_param_value("category")) : "";
        int merchant_id = req.has_param("MerchantID") ? std::stoi(req.get_param_value("MerchantID")) : 0;
        json arr = json::array();
        std::string sql = "SELECT s.ServiceID,s.MerchantID,s.ServiceName,s.Category,s.Price,s.Duration,s.Capacity,s.IsActive,s.CreatedAt,m.ShopName,m.Address,"
                          "(SELECT COUNT(*) FROM t_review r JOIN t_appointment a ON r.AppointmentID=a.AppointmentID WHERE a.ServiceID=s.ServiceID) AS ReviewCount,"
                          "COALESCE((SELECT AVG(r.Rating) FROM t_review r JOIN t_appointment a ON r.AppointmentID=a.AppointmentID WHERE a.ServiceID=s.ServiceID),0) AS AvgRating "
                          "FROM t_service s JOIN t_merchant m ON s.MerchantID=m.MerchantID WHERE 1=1";
        std::vector<std::string> params;
        if (merchant_id > 0) {
            sql += " AND s.MerchantID=?";
            params.push_back(std::to_string(merchant_id));
        }
        if (!keyword.empty()) { sql += " AND (s.ServiceName LIKE ? OR s.Category LIKE ? OR m.ShopName LIKE ?)"; params.push_back("%"+keyword+"%"); params.push_back("%"+keyword+"%"); params.push_back("%"+keyword+"%"); }
        if (!category.empty()) { sql += " AND s.Category LIKE ?"; params.push_back("%"+category+"%"); }
        sql += " ORDER BY s.ServiceID DESC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            for (size_t i = 0; i < params.size(); ++i) sqlite3_bind_text(stmt, static_cast<int>(i+1), params[i].c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                json it;
                it["ServiceID"] = sqlite3_column_int(stmt,0);
                it["MerchantID"] = sqlite3_column_int(stmt,1);
                it["ServiceName"] = safe_text(stmt,2);
                it["Category"] = safe_text(stmt,3);
                it["Price"] = sqlite3_column_int(stmt,4);
                it["Duration"] = sqlite3_column_int(stmt,5);
                it["Capacity"] = sqlite3_column_int(stmt,6);
                it["IsActive"] = sqlite3_column_int(stmt,7) == 1;
                it["CreatedAt"] = safe_text(stmt,8);
                it["ShopName"] = safe_text(stmt,9);
                it["Address"] = safe_text(stmt,10);
                it["ReviewCount"] = sqlite3_column_int(stmt,11);
                it["AvgRating"] = sqlite3_column_double(stmt,12);
                arr.push_back(it);
            }
        }
        sqlite3_finalize(stmt);
        set_json(res, arr);
    });

    svr.Post("/api/merchant/service", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d = json::parse(req.body);
            int mid = d.value("MerchantID", 0);
            std::string name = trim(d.value("ServiceName", ""));
            std::string cate = trim(d.value("Category", ""));
            int price = d.value("Price", 0), duration = d.value("Duration", 0), cap = d.value("Capacity", 1);
            if (mid <= 0 || name.empty() || cate.empty() || price < 0 || duration <= 0 || cap <= 0) return set_json(res, {{"success", false}, {"message", "服务信息不完整或数值不合法"}}, 400);
            if (scalar_int("SELECT COUNT(*) FROM t_merchant WHERE MerchantID=?;", {std::to_string(mid)}) == 0) return set_json(res, {{"success", false}, {"message", "商家不存在，请先注册/登录商家账号"}}, 400);
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration, Capacity, IsActive, CreatedAt) VALUES (?, ?, ?, ?, ?, ?, 1, datetime('now','localtime'));";
            bool ok = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, mid); sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(stmt, 3, cate.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 4, price); sqlite3_bind_int(stmt, 5, duration); sqlite3_bind_int(stmt, 6, cap);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
            set_json(res, {{"success", ok}, {"message", ok ? "服务发布成功" : "服务发布失败"}}, ok ? 200 : 500);
        } catch (const std::exception& e) { set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Post("/api/merchant/service/delete", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d = json::parse(req.body);
            int mid = d.value("MerchantID", 0), sid = d.value("ServiceID", 0);
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE t_service SET IsActive=0 WHERE ServiceID=? AND MerchantID=?;";
            bool ok = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, sid); sqlite3_bind_int(stmt, 2, mid); sqlite3_step(stmt); ok = sqlite3_changes(db) > 0;
            }
            sqlite3_finalize(stmt);
            set_json(res, {{"success", ok}, {"message", ok ? "服务已下架，历史预约保留" : "服务不存在或无权限"}}, ok ? 200 : 404);
        } catch (const std::exception& e) { set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Post("/api/merchant/service/restore", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d = json::parse(req.body);
            int mid = d.value("MerchantID", 0), sid = d.value("ServiceID", 0);
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE t_service SET IsActive=1 WHERE ServiceID=? AND MerchantID=?;";
            bool ok = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, sid); sqlite3_bind_int(stmt, 2, mid); sqlite3_step(stmt); ok = sqlite3_changes(db) > 0;
            }
            sqlite3_finalize(stmt);
            set_json(res, {{"success", ok}, {"message", ok ? "服务已恢复上架" : "服务不存在或无权限"}}, ok ? 200 : 404);
        } catch (const std::exception& e) { set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Get("/api/merchant/schedule", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        int mid = req.has_param("MerchantID") ? std::stoi(req.get_param_value("MerchantID")) : 1;
        json arr = json::array(); sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT DayOfWeek, IsWorkingDay FROM t_schedule WHERE MerchantID=? ORDER BY DayOfWeek;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) { sqlite3_bind_int(stmt,1,mid); while(sqlite3_step(stmt)==SQLITE_ROW) arr.push_back({{"DayOfWeek",sqlite3_column_int(stmt,0)},{"IsWorkingDay",sqlite3_column_int(stmt,1)}}); }
        sqlite3_finalize(stmt); set_json(res, arr);
    });

    svr.Post("/api/merchant/schedule", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d = json::parse(req.body); int mid = d.value("MerchantID", 0); auto schedule = d.at("Schedule");
            if (mid <= 0 || !schedule.is_array()) return set_json(res, {{"success", false}, {"message", "排班参数错误"}}, 400);
            exec_sql("BEGIN TRANSACTION;");
            sqlite3_stmt* del = nullptr;
            if (sqlite3_prepare_v2(db, "DELETE FROM t_schedule WHERE MerchantID=?;", -1, &del, nullptr) == SQLITE_OK) { sqlite3_bind_int(del,1,mid); sqlite3_step(del); }
            sqlite3_finalize(del);
            for (auto& item : schedule) {
                int day = item.value("DayOfWeek", -1), working = item.value("IsWorkingDay", 1);
                if (day < 0 || day > 6) continue;
                sqlite3_stmt* ins = nullptr;
                if (sqlite3_prepare_v2(db, "INSERT INTO t_schedule (MerchantID,DayOfWeek,IsWorkingDay) VALUES (?,?,?);", -1, &ins, nullptr)==SQLITE_OK) { sqlite3_bind_int(ins,1,mid); sqlite3_bind_int(ins,2,day); sqlite3_bind_int(ins,3,working?1:0); sqlite3_step(ins); }
                sqlite3_finalize(ins);
            }
            exec_sql("COMMIT;"); set_json(res, {{"success", true}, {"message", "排班设置已保存"}});
        } catch (const std::exception& e) { exec_sql("ROLLBACK;"); set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Post("/api/appointments", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d = json::parse(req.body);
            int uid = d.value("UserID", 0), sid = d.value("ServiceID", 0);
            std::string at = trim(d.value("AppointmentTime", ""));
            if (uid <= 0 || sid <= 0 || !is_valid_datetime(at)) return set_json(res, {{"success", false}, {"message", "预约参数不合法，请选择有效时间"}}, 400);
            if (scalar_int("SELECT COUNT(*) FROM t_user WHERE UserID=?;", {std::to_string(uid)}) == 0) return set_json(res, {{"success", false}, {"message", "用户不存在，请重新登录"}}, 400);
            int mid = get_service_merchant_id(sid);
            if (mid == 0) return set_json(res, {{"success", false}, {"message", "服务不存在或已下架"}}, 400);
            if (!is_working_day(mid, at)) return set_json(res, {{"success", false}, {"message", "该日期为商家休息日，请重新选择"}}, 400);
            if (scalar_int("SELECT COUNT(*) FROM t_appointment WHERE UserID=? AND AppointmentTime=? AND Status IN (1,2);", {std::to_string(uid), at}) > 0) return set_json(res, {{"success", false}, {"message", "你在该时段已有未完成预约，不能重复预约"}}, 400);
            int capacity = get_service_capacity(sid);
            int occupied = scalar_int("SELECT COUNT(*) FROM t_appointment WHERE ServiceID=? AND AppointmentTime=? AND Status IN (1,2);", {std::to_string(sid), at});
            if (occupied >= capacity) return set_json(res, {{"success", false}, {"message", "该时段名额已满，请选择其他时间"}}, 400);
            exec_sql("BEGIN IMMEDIATE TRANSACTION;");
            occupied = scalar_int("SELECT COUNT(*) FROM t_appointment WHERE ServiceID=? AND AppointmentTime=? AND Status IN (1,2);", {std::to_string(sid), at});
            if (occupied >= capacity) { exec_sql("ROLLBACK;"); return set_json(res, {{"success", false}, {"message", "该时段刚被约满，请刷新后重试"}}, 409); }
            sqlite3_stmt* stmt = nullptr; bool ok = false;
            if (sqlite3_prepare_v2(db, "INSERT INTO t_appointment (UserID,ServiceID,AppointmentTime,Status,CreatedAt) VALUES (?,?,?,1,datetime('now','localtime'));", -1, &stmt, nullptr)==SQLITE_OK) {
                sqlite3_bind_int(stmt,1,uid); sqlite3_bind_int(stmt,2,sid); sqlite3_bind_text(stmt,3,at.c_str(),-1,SQLITE_TRANSIENT); ok = sqlite3_step(stmt)==SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
            int aid = static_cast<int>(sqlite3_last_insert_rowid(db));
            if (ok) { add_notification("merchant", mid, aid, "有新的预约申请待确认"); add_notification("user", uid, aid, "预约已提交，等待商家确认"); exec_sql("COMMIT;"); set_json(res, {{"success", true}, {"message", "预约已提交，等待商家确认"}, {"AppointmentID", aid}, {"Status", 1}, {"StatusText", status_to_string(1)}}); }
            else { exec_sql("ROLLBACK;"); set_json(res, {{"success", false}, {"message", "预约创建失败"}}, 500); }
        } catch (const std::exception& e) { exec_sql("ROLLBACK;"); set_json(res, {{"success", false}, {"message", std::string("请求格式错误：") + e.what()}}, 400); }
    });

    svr.Get("/api/appointments", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string sql = "SELECT a.AppointmentID,a.UserID,a.ServiceID,a.AppointmentTime,a.Status,u.Username,u.PhoneNumber,s.ServiceName,s.Category,s.Price,s.Duration,s.MerchantID,m.ShopName,"
                          "(SELECT COUNT(*) FROM t_review r WHERE r.AppointmentID=a.AppointmentID) AS HasReview "
                          "FROM t_appointment a JOIN t_user u ON a.UserID=u.UserID JOIN t_service s ON a.ServiceID=s.ServiceID JOIN t_merchant m ON s.MerchantID=m.MerchantID WHERE 1=1";
        std::vector<std::string> params;
        if (req.has_param("UserID")) { sql += " AND a.UserID=?"; params.push_back(req.get_param_value("UserID")); }
        if (req.has_param("MerchantID")) { sql += " AND s.MerchantID=?"; params.push_back(req.get_param_value("MerchantID")); }
        sql += " ORDER BY a.AppointmentID DESC;";
        json arr = json::array(); sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr)==SQLITE_OK) {
            for(size_t i=0;i<params.size();++i) sqlite3_bind_text(stmt,(int)i+1,params[i].c_str(),-1,SQLITE_TRANSIENT);
            while(sqlite3_step(stmt)==SQLITE_ROW) arr.push_back(appointment_json_from_stmt(stmt));
        }
        sqlite3_finalize(stmt); set_json(res, arr);
    });

    svr.Post("/api/merchant/appointments/confirm", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try {
            auto d=json::parse(req.body); int mid=d.value("MerchantID",0), aid=d.value("AppointmentID",0);
            sqlite3_stmt* check=nullptr; int uid=0;
            const char* csql="SELECT a.UserID FROM t_appointment a JOIN t_service s ON a.ServiceID=s.ServiceID WHERE a.AppointmentID=? AND s.MerchantID=? AND a.Status=1;";
            if(sqlite3_prepare_v2(db,csql,-1,&check,nullptr)==SQLITE_OK){sqlite3_bind_int(check,1,aid);sqlite3_bind_int(check,2,mid); if(sqlite3_step(check)==SQLITE_ROW) uid=sqlite3_column_int(check,0);} sqlite3_finalize(check);
            if(uid==0) return set_json(res, {{"success",false},{"message","预约不存在、无权限或已处理"}},400);
            sqlite3_stmt* stmt=nullptr; bool ok=false;
            if(sqlite3_prepare_v2(db,"UPDATE t_appointment SET Status=2 WHERE AppointmentID=? AND Status=1;",-1,&stmt,nullptr)==SQLITE_OK){sqlite3_bind_int(stmt,1,aid); sqlite3_step(stmt); ok=sqlite3_changes(db)>0;} sqlite3_finalize(stmt);
            if(ok){ add_notification("user",uid,aid,"商家已确认你的预约，请按时到店"); set_json(res,{{"success",true},{"message","已确认预约"},{"Status",2},{"StatusText",status_to_string(2)}});} else set_json(res,{{"success",false},{"message","确认失败"}},500);
        } catch(const std::exception& e){ set_json(res,{{"success",false},{"message",std::string("请求格式错误：")+e.what()}},400); }
    });

    svr.Post("/api/appointments/complete", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try { auto d=json::parse(req.body); int mid=d.value("MerchantID",0), aid=d.value("AppointmentID",0); sqlite3_stmt* chk=nullptr; int uid=0;
            const char* csql="SELECT a.UserID FROM t_appointment a JOIN t_service s ON a.ServiceID=s.ServiceID WHERE a.AppointmentID=? AND s.MerchantID=? AND a.Status=2;";
            if(sqlite3_prepare_v2(db,csql,-1,&chk,nullptr)==SQLITE_OK){sqlite3_bind_int(chk,1,aid);sqlite3_bind_int(chk,2,mid);if(sqlite3_step(chk)==SQLITE_ROW) uid=sqlite3_column_int(chk,0);} sqlite3_finalize(chk);
            if(uid==0) return set_json(res,{{"success",false},{"message","只有已确认且属于本商家的预约才能核销"}},400);
            sqlite3_stmt* st=nullptr; bool ok=false; if(sqlite3_prepare_v2(db,"UPDATE t_appointment SET Status=4 WHERE AppointmentID=? AND Status=2;",-1,&st,nullptr)==SQLITE_OK){sqlite3_bind_int(st,1,aid);sqlite3_step(st);ok=sqlite3_changes(db)>0;} sqlite3_finalize(st);
            if(ok){ add_notification("user",uid,aid,"服务已完成，欢迎评价反馈"); set_json(res,{{"success",true},{"message","已完成核销，可以邀请用户评价"},{"Status",4},{"StatusText",status_to_string(4)}});} else set_json(res,{{"success",false},{"message","核销失败"}},500);
        } catch(const std::exception& e){ set_json(res,{{"success",false},{"message",std::string("请求格式错误：")+e.what()}},400); }
    });

    svr.Post("/api/appointments/cancel", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try { auto d=json::parse(req.body); int aid=d.value("AppointmentID",0), uid=d.value("UserID",0), mid=d.value("MerchantID",0);
            std::string sql="SELECT a.UserID,s.MerchantID,a.Status FROM t_appointment a JOIN t_service s ON a.ServiceID=s.ServiceID WHERE a.AppointmentID=?;"; sqlite3_stmt* chk=nullptr; int real_uid=0, real_mid=0, stt=0;
            if(sqlite3_prepare_v2(db,sql.c_str(),-1,&chk,nullptr)==SQLITE_OK){sqlite3_bind_int(chk,1,aid);if(sqlite3_step(chk)==SQLITE_ROW){real_uid=sqlite3_column_int(chk,0); real_mid=sqlite3_column_int(chk,1); stt=sqlite3_column_int(chk,2);}} sqlite3_finalize(chk);
            if(real_uid==0) return set_json(res,{{"success",false},{"message","预约不存在"}},404);
            bool allowed=(uid>0 && uid==real_uid)||(mid>0 && mid==real_mid); if(!allowed) return set_json(res,{{"success",false},{"message","无权限取消该预约"}},403);
            if(!(stt==1||stt==2)) return set_json(res,{{"success",false},{"message","该预约当前状态不能取消"}},400);
            sqlite3_stmt* up=nullptr; bool ok=false; if(sqlite3_prepare_v2(db,"UPDATE t_appointment SET Status=3 WHERE AppointmentID=? AND Status IN (1,2);",-1,&up,nullptr)==SQLITE_OK){sqlite3_bind_int(up,1,aid);sqlite3_step(up);ok=sqlite3_changes(db)>0;} sqlite3_finalize(up);
            if(ok){ if(uid>0) add_notification("merchant",real_mid,aid,"用户取消了预约"); if(mid>0) add_notification("user",real_uid,aid,"商家取消了预约，请重新选择时间"); set_json(res,{{"success",true},{"message","预约已取消"},{"Status",3},{"StatusText",status_to_string(3)}});} else set_json(res,{{"success",false},{"message","取消失败"}},500);
        } catch(const std::exception& e){ set_json(res,{{"success",false},{"message",std::string("请求格式错误：")+e.what()}},400); }
    });

    svr.Post("/api/reviews", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        try { auto d=json::parse(req.body); int aid=d.value("AppointmentID",0), uid=d.value("UserID",0), rating=d.value("Rating",0); std::string comment=trim(d.value("CommentContent", ""));
            if(rating<1||rating>5) return set_json(res,{{"success",false},{"message","评分必须为 1-5"}},400);
            if(scalar_int("SELECT COUNT(*) FROM t_appointment WHERE AppointmentID=? AND UserID=? AND Status=4;",{std::to_string(aid),std::to_string(uid)})==0) return set_json(res,{{"success",false},{"message","只能评价自己已完成的预约"}},400);
            sqlite3_stmt* st=nullptr; bool ok=false; if(sqlite3_prepare_v2(db,"INSERT INTO t_review (AppointmentID,Rating,CommentContent,CreatedAt) VALUES (?,?,?,datetime('now','localtime'));",-1,&st,nullptr)==SQLITE_OK){sqlite3_bind_int(st,1,aid);sqlite3_bind_int(st,2,rating);sqlite3_bind_text(st,3,comment.c_str(),-1,SQLITE_TRANSIENT); ok=sqlite3_step(st)==SQLITE_DONE;} sqlite3_finalize(st);
            set_json(res,{{"success",ok},{"message",ok?"评价提交成功":"该预约已评价，不能重复提交"}},ok?200:400);
        } catch(const std::exception& e){ set_json(res,{{"success",false},{"message",std::string("请求格式错误：")+e.what()}},400); }
    });

    svr.Get("/api/reviews", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string sql="SELECT r.ReviewID,r.AppointmentID,r.Rating,r.CommentContent,r.CreatedAt,u.Username,s.ServiceName FROM t_review r JOIN t_appointment a ON r.AppointmentID=a.AppointmentID JOIN t_user u ON a.UserID=u.UserID JOIN t_service s ON a.ServiceID=s.ServiceID WHERE 1=1";
        std::vector<std::string> params; if(req.has_param("ServiceID")){sql+=" AND s.ServiceID=?"; params.push_back(req.get_param_value("ServiceID"));} if(req.has_param("MerchantID")){sql+=" AND s.MerchantID=?"; params.push_back(req.get_param_value("MerchantID"));} sql += " ORDER BY r.ReviewID DESC LIMIT 50;";
        json arr=json::array(); sqlite3_stmt* st=nullptr; if(sqlite3_prepare_v2(db,sql.c_str(),-1,&st,nullptr)==SQLITE_OK){for(size_t i=0;i<params.size();++i)sqlite3_bind_text(st,(int)i+1,params[i].c_str(),-1,SQLITE_TRANSIENT); while(sqlite3_step(st)==SQLITE_ROW){arr.push_back({{"ReviewID",sqlite3_column_int(st,0)},{"AppointmentID",sqlite3_column_int(st,1)},{"Rating",sqlite3_column_int(st,2)},{"CommentContent",safe_text(st,3)},{"CreatedAt",safe_text(st,4)},{"Username",safe_text(st,5)},{"ServiceName",safe_text(st,6)}});}} sqlite3_finalize(st); set_json(res,arr);
    });

    auto notifications_handler = [](const httplib::Request& req, httplib::Response& res, const std::string& type) {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string key = type == "user" ? "UserID" : "MerchantID";
        int rid = req.has_param(key.c_str()) ? std::stoi(req.get_param_value(key.c_str())) : 0;
        json arr=json::array(); sqlite3_stmt* st=nullptr;
        const char* sql="SELECT NotificationID,AppointmentID,Message,Status,CreatedAt FROM t_notification WHERE ReceiverType=? AND ReceiverID=? ORDER BY NotificationID DESC LIMIT 20;";
        if(sqlite3_prepare_v2(db,sql,-1,&st,nullptr)==SQLITE_OK){sqlite3_bind_text(st,1,type.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int(st,2,rid);while(sqlite3_step(st)==SQLITE_ROW){arr.push_back({{"NotificationID",sqlite3_column_int(st,0)},{"AppointmentID",sqlite3_column_int(st,1)},{"Message",safe_text(st,2)},{"Status",sqlite3_column_int(st,3)},{"CreatedAt",safe_text(st,4)}});}} sqlite3_finalize(st); set_json(res,arr);
    };
    svr.Get("/api/user/notifications", [notifications_handler](const httplib::Request& req, httplib::Response& res){ notifications_handler(req,res,"user"); });
    svr.Get("/api/merchant/notifications", [notifications_handler](const httplib::Request& req, httplib::Response& res){ notifications_handler(req,res,"merchant"); });

    const char* url = "http://127.0.0.1:8080";
    std::cout << "==============================================" << std::endl;
    std::cout << " 在线预约系统正在启动" << std::endl;
    std::cout << " 浏览器地址：" << url << std::endl;
    std::cout << " 测试用户：13311112222 / 123456" << std::endl;
    std::cout << " 测试商家：13800001111 / 123456" << std::endl;
    std::cout << " 使用完毕后，关闭本窗口即可停止服务器" << std::endl;
    std::cout << "==============================================" << std::endl;
    std::thread([](){
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        open_home_page();
    }).detach();
    svr.listen("0.0.0.0", 8080);
    sqlite3_close(db);
    return 0;
}
