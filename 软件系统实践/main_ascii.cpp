#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <sqlite3.h>
#include "httplib.h"
#include "json.hpp"

sqlite3* db = nullptr;

std::string normalize_text_value(const std::string& value, const std::string& fallback) {
    if (value.empty()) {
        return fallback;
    }
    std::string normalized = value;
    if (normalized == "??" || normalized == "？？") {
        return fallback;
    }
    if (std::all_of(normalized.begin(), normalized.end(), [](unsigned char c) {
            return c == '?' || c == '？';
        })) {
        return fallback;
    }
    return normalized;
}

bool table_has_column(const std::string& table_name, const std::string& column_name) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "PRAGMA table_info(" + table_name + ");";
    bool exists = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            if (name != nullptr && std::string(reinterpret_cast<const char*>(name)) == column_name) {
                exists = true;
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
    return exists;
}

void ensure_service_capacity_column() {
    if (table_has_column("t_service", "Capacity")) {
        return;
    }
    std::string sql = "ALTER TABLE t_service ADD COLUMN Capacity INTEGER DEFAULT 1;";
    char* err_msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error adding Capacity column: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
}

std::string status_to_string(int status) {
    switch (status) {
        case 1: return "PendingConfirmation";
        case 2: return "Completed";
        case 3: return "Cancelled";
        case 4: return "Cancelled";
        default: return "Unknown";
    }
}

int get_service_merchant_id(int service_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT MerchantID FROM t_service WHERE ServiceID = ?;";
    int merchant_id = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, service_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            merchant_id = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return merchant_id;
}

int get_service_capacity(int service_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COALESCE(Capacity, 1) FROM t_service WHERE ServiceID = ?;";
    int capacity = 1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, service_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            capacity = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return capacity > 0 ? capacity : 1;
}

int get_day_of_week(const std::string& appointment_time) {
    std::tm tm = {};
    std::istringstream ss(appointment_time.substr(0, 10));
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) {
        return -1;
    }
    std::mktime(&tm);
    return tm.tm_wday;
}

bool is_working_day(int merchant_id, const std::string& appointment_time) {
    int day_of_week = get_day_of_week(appointment_time);
    if (day_of_week < 0) {
        return true;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT IsWorkingDay FROM t_schedule WHERE MerchantID = ? AND DayOfWeek = ?;";
    bool working = true;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, merchant_id);
        sqlite3_bind_int(stmt, 2, day_of_week);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            working = sqlite3_column_int(stmt, 0) == 1;
        }
    }
    sqlite3_finalize(stmt);
    return working;
}

int get_occupied_slot_count(int service_id, const std::string& appointment_time) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM t_appointment WHERE ServiceID = ? AND AppointmentTime = ? AND Status IN (1, 2);";
    int count = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, service_id);
        sqlite3_bind_text(stmt, 2, appointment_time.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return count;
}

void add_notification(int merchant_id, int appointment_id, const std::string& message) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO t_notification (MerchantID, AppointmentID, Message, Status) VALUES (?, ?, ?, 0);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, merchant_id);
        sqlite3_bind_int(stmt, 2, appointment_id);
        sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

void init_db() {
    int rc = sqlite3_open("appointment.db", &db);
    if (rc) {
        std::cerr << "Failed to open database" << std::endl;
        exit(1);
    }

    const char* sql_merchant = "CREATE TABLE IF NOT EXISTS t_merchant (MerchantID INTEGER PRIMARY KEY AUTOINCREMENT, ShopName TEXT, ContactInfo TEXT, Address TEXT);";
    const char* sql_user = "CREATE TABLE IF NOT EXISTS t_user (UserID INTEGER PRIMARY KEY AUTOINCREMENT, Username TEXT, PhoneNumber TEXT UNIQUE, Password TEXT);";
    const char* sql_service = "CREATE TABLE IF NOT EXISTS t_service (ServiceID INTEGER PRIMARY KEY AUTOINCREMENT, MerchantID INTEGER, ServiceName TEXT, Category TEXT, Price INTEGER, Duration INTEGER, Capacity INTEGER DEFAULT 1);";
    const char* sql_appointment = "CREATE TABLE IF NOT EXISTS t_appointment (AppointmentID INTEGER PRIMARY KEY AUTOINCREMENT, UserID INTEGER, ServiceID INTEGER, AppointmentTime TEXT, Status INTEGER);";
    const char* sql_review = "CREATE TABLE IF NOT EXISTS t_review (ReviewID INTEGER PRIMARY KEY AUTOINCREMENT, AppointmentID INTEGER UNIQUE, Rating INTEGER, CommentContent TEXT);";
    const char* sql_schedule = "CREATE TABLE IF NOT EXISTS t_schedule (ScheduleID INTEGER PRIMARY KEY AUTOINCREMENT, MerchantID INTEGER, DayOfWeek INTEGER, IsWorkingDay INTEGER DEFAULT 1);";
    const char* sql_notification = "CREATE TABLE IF NOT EXISTS t_notification (NotificationID INTEGER PRIMARY KEY AUTOINCREMENT, MerchantID INTEGER, AppointmentID INTEGER, Message TEXT, Status INTEGER DEFAULT 0);";

    char* err_msg = nullptr;
    if (sqlite3_exec(db, sql_merchant, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error creating merchant table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(db, sql_user, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error creating user table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(db, sql_service, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error creating service table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(db, sql_appointment, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error creating appointment table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(db, sql_review, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error creating review table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(db, sql_schedule, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error creating schedule table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(db, sql_notification, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error creating notification table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    ensure_service_capacity_column();

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t_service;", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        if (count == 0) {
            if (sqlite3_exec(db, "INSERT INTO t_merchant (ShopName, ContactInfo, Address) VALUES ('Shop1', '13800001111', 'Address1');", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting merchant: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
            if (sqlite3_exec(db, "INSERT INTO t_user (Username, PhoneNumber, Password) VALUES ('User1', '13311112222', '123456');", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting user: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
            if (sqlite3_exec(db, "INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration, Capacity) VALUES (1, 'Service1', 'Category1', 45, 30, 3);", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting service 1: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
            if (sqlite3_exec(db, "INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration, Capacity) VALUES (1, 'Service2', 'Category1', 120, 60, 2);", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting service 2: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
        }
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(db, "INSERT OR IGNORE INTO t_service (ServiceID, MerchantID, ServiceName, Category, Price, Duration, Capacity) VALUES (3, 1, 'Service3', 'Category2', 99, 90, 2);", nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error ensuring service 3: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
    if (sqlite3_exec(db, "UPDATE t_service SET ServiceName = 'Service3', Category = 'Category2', Price = 99, Duration = 90, Capacity = 2 WHERE ServiceID = 3;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error updating service 3: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    if (sqlite3_exec(db, "INSERT OR IGNORE INTO t_schedule (MerchantID, DayOfWeek, IsWorkingDay) VALUES (1, 0, 0), (1, 1, 1), (1, 2, 1), (1, 3, 1), (1, 4, 1), (1, 5, 1), (1, 6, 0);", nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Error inserting default schedule: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }
}

std::string read_html_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "<h1>404 HTML File Not Found</h1>";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main() {
    init_db();
    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(read_html_file("index.html"), "text/html; charset=utf-8");
    });

    svr.Post("/api/auth/register", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            std::string username = data.value("Username", "");
            std::string phone = data.value("PhoneNumber", "");
            std::string password = data.value("Password", "");

            if (username.empty() || phone.empty() || password.empty()) {
                ret["success"] = false;
                ret["message"] = "Username, phone number and password cannot be empty";
                res.status = 400;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            sqlite3_stmt* check_stmt = nullptr;
            const char* check_sql = "SELECT COUNT(*) FROM t_user WHERE PhoneNumber = ?;";
            int existing_count = 0;
            if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(check_stmt, 1, phone.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(check_stmt) == SQLITE_ROW) {
                    existing_count = sqlite3_column_int(check_stmt, 0);
                }
            }
            sqlite3_finalize(check_stmt);

            if (existing_count > 0) {
                ret["success"] = false;
                ret["message"] = "Phone number already registered";
                res.status = 400;
            } else {
                sqlite3_stmt* stmt = nullptr;
                const char* sql = "INSERT INTO t_user (Username, PhoneNumber, Password) VALUES (?, ?, ?);";
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, phone.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, password.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                }
                sqlite3_finalize(stmt);

                int user_id = static_cast<int>(sqlite3_last_insert_rowid(db));
                ret["success"] = true;
                ret["message"] = "Registration successful";
                ret["user"]["UserID"] = user_id;
                ret["user"]["Username"] = username;
                ret["user"]["PhoneNumber"] = phone;
            }
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/auth/login", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            std::string phone = data.value("PhoneNumber", "");
            std::string password = data.value("Password", "");

            if (phone.empty() || password.empty()) {
                ret["success"] = false;
                ret["message"] = "Phone number and password cannot be empty";
                res.status = 400;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT UserID, Username, PhoneNumber FROM t_user WHERE PhoneNumber = ? AND Password = ?;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, phone.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    ret["success"] = true;
                    ret["message"] = "Login successful";
                    ret["user"]["UserID"] = sqlite3_column_int(stmt, 0);
                    ret["user"]["Username"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    ret["user"]["PhoneNumber"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                } else {
                    ret["success"] = false;
                    ret["message"] = "Incorrect phone number or password";
                    res.status = 401;
                }
            }
            sqlite3_finalize(stmt);
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/services", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json json_response = nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT ServiceID, MerchantID, ServiceName, Category, Price, Duration, Capacity FROM t_service;";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json item;
                item["ServiceID"] = sqlite3_column_int(stmt, 0);
                item["MerchantID"] = sqlite3_column_int(stmt, 1);
                item["ServiceName"] = normalize_text_value(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)), "未命名服务");
                item["Category"] = normalize_text_value(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)), "未分类");
                item["Price"] = sqlite3_column_int(stmt, 4);
                item["Duration"] = sqlite3_column_int(stmt, 5);
                item["Capacity"] = sqlite3_column_int(stmt, 6);
                json_response.push_back(item);
            }
        }
        sqlite3_finalize(stmt);
        res.set_content(json_response.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/merchant/service", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int merchant_id = data["MerchantID"].get<int>();
            std::string name = data["ServiceName"].get<std::string>();
            std::string cate = data["Category"].get<std::string>();
            int price = data["Price"].get<int>();
            int duration = data["Duration"].get<int>();
            int capacity = data.value("Capacity", 1);

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration, Capacity) VALUES (?, ?, ?, ?, ?, ?);";
            bool insert_ok = false;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, merchant_id);
                sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, cate.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 4, price);
                sqlite3_bind_int(stmt, 5, duration);
                sqlite3_bind_int(stmt, 6, capacity);
                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    insert_ok = true;
                }
            }
            sqlite3_finalize(stmt);

            if (!insert_ok) {
                ret["success"] = false;
                ret["message"] = "Failed to publish service";
                res.status = 500;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            ret["success"] = true;
            ret["message"] = "Service added successfully";
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/merchant/service/delete", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int merchant_id = data["MerchantID"].get<int>();
            int service_id = data["ServiceID"].get<int>();

            sqlite3_stmt* notif_stmt = nullptr;
            const char* notif_sql = "DELETE FROM t_notification WHERE AppointmentID IN (SELECT AppointmentID FROM t_appointment WHERE ServiceID = ?);";
            if (sqlite3_prepare_v2(db, notif_sql, -1, &notif_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(notif_stmt, 1, service_id);
                sqlite3_step(notif_stmt);
            }
            sqlite3_finalize(notif_stmt);

            sqlite3_stmt* appointment_stmt = nullptr;
            const char* appointment_sql = "DELETE FROM t_appointment WHERE ServiceID = ?;";
            if (sqlite3_prepare_v2(db, appointment_sql, -1, &appointment_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(appointment_stmt, 1, service_id);
                sqlite3_step(appointment_stmt);
            }
            sqlite3_finalize(appointment_stmt);

            sqlite3_stmt* service_stmt = nullptr;
            const char* service_sql = "DELETE FROM t_service WHERE ServiceID = ? AND MerchantID = ?;";
            bool deleted = false;
            if (sqlite3_prepare_v2(db, service_sql, -1, &service_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(service_stmt, 1, service_id);
                sqlite3_bind_int(service_stmt, 2, merchant_id);
                if (sqlite3_step(service_stmt) == SQLITE_DONE) {
                    deleted = sqlite3_changes(db) > 0;
                }
            }
            sqlite3_finalize(service_stmt);

            if (!deleted) {
                ret["success"] = false;
                ret["message"] = "Service not found or you do not have permission to delete it";
                res.status = 404;
            } else {
                ret["success"] = true;
                ret["message"] = "Service deleted successfully";
            }
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/merchant/schedule", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int merchant_id = data["MerchantID"].get<int>();
            auto schedule = data["Schedule"];

            sqlite3_stmt* delete_stmt = nullptr;
            const char* delete_sql = "DELETE FROM t_schedule WHERE MerchantID = ?;";
            if (sqlite3_prepare_v2(db, delete_sql, -1, &delete_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(delete_stmt, 1, merchant_id);
                sqlite3_step(delete_stmt);
            }
            sqlite3_finalize(delete_stmt);

            for (const auto& item : schedule) {
                int day = item["DayOfWeek"].get<int>();
                int working = item["IsWorkingDay"].get<int>();
                sqlite3_stmt* insert_stmt = nullptr;
                const char* insert_sql = "INSERT INTO t_schedule (MerchantID, DayOfWeek, IsWorkingDay) VALUES (?, ?, ?);";
                if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(insert_stmt, 1, merchant_id);
                    sqlite3_bind_int(insert_stmt, 2, day);
                    sqlite3_bind_int(insert_stmt, 3, working);
                    sqlite3_step(insert_stmt);
                }
                sqlite3_finalize(insert_stmt);
            }

            ret["success"] = true;
            ret["message"] = "Schedule updated successfully";
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/merchant/schedule", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json json_response = nlohmann::json::array();
        int merchant_id = std::stoi(req.get_param_value("MerchantID"));
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT DayOfWeek, IsWorkingDay FROM t_schedule WHERE MerchantID = ? ORDER BY DayOfWeek;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, merchant_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json item;
                item["DayOfWeek"] = sqlite3_column_int(stmt, 0);
                item["IsWorkingDay"] = sqlite3_column_int(stmt, 1);
                json_response.push_back(item);
            }
        }
        sqlite3_finalize(stmt);
        res.set_content(json_response.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/appointments", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int user_id = data["UserID"].get<int>();
            int service_id = data["ServiceID"].get<int>();
            std::string app_time = data["AppointmentTime"].get<std::string>();

            int merchant_id = get_service_merchant_id(service_id);
            if (merchant_id == 0) {
                ret["success"] = false;
                ret["message"] = "Service not found";
                res.status = 400;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            if (!is_working_day(merchant_id, app_time)) {
                ret["success"] = false;
                ret["message"] = "Service provider is off duty on that day";
                res.status = 400;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            int capacity = get_service_capacity(service_id);
            int occupied = get_occupied_slot_count(service_id, app_time);
            if (occupied >= capacity) {
                ret["success"] = false;
                ret["message"] = "Insufficient inventory for the selected timeslot";
                res.status = 400;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO t_appointment (UserID, ServiceID, AppointmentTime, Status) VALUES (?, ?, ?, 1);";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, user_id);
                sqlite3_bind_int(stmt, 2, service_id);
                sqlite3_bind_text(stmt, 3, app_time.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);

            int appointment_id = static_cast<int>(sqlite3_last_insert_rowid(db));
            add_notification(merchant_id, appointment_id, "New appointment is waiting for your confirmation.");

            ret["success"] = true;
            ret["message"] = "Appointment submitted and waiting for provider confirmation";
            ret["AppointmentID"] = appointment_id;
            ret["Status"] = 1;
            ret["StatusText"] = status_to_string(1);
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/appointments", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json json_response = nlohmann::json::array();
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT a.AppointmentID, a.UserID, a.ServiceID, a.AppointmentTime, a.Status, u.Username, s.ServiceName, s.Price FROM t_appointment a JOIN t_user u ON a.UserID = u.UserID JOIN t_service s ON a.ServiceID = s.ServiceID ORDER BY a.AppointmentID DESC;";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json item;
                item["AppointmentID"] = sqlite3_column_int(stmt, 0);
                item["UserID"] = sqlite3_column_int(stmt, 1);
                item["ServiceID"] = sqlite3_column_int(stmt, 2);
                item["AppointmentTime"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                item["Status"] = sqlite3_column_int(stmt, 4);
                item["StatusText"] = status_to_string(sqlite3_column_int(stmt, 4));
                item["Username"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                item["ServiceName"] = normalize_text_value(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)), "未命名服务");
                item["Price"] = sqlite3_column_int(stmt, 7);
                json_response.push_back(item);
            }
        }
        sqlite3_finalize(stmt);
        res.set_content(json_response.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/merchant/notifications", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json json_response = nlohmann::json::array();
        int merchant_id = std::stoi(req.get_param_value("MerchantID"));
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT n.NotificationID, n.AppointmentID, n.Message, a.UserID, a.ServiceID, a.AppointmentTime, a.Status FROM t_notification n JOIN t_appointment a ON n.AppointmentID = a.AppointmentID WHERE n.MerchantID = ? AND n.Status = 0 ORDER BY n.NotificationID DESC;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, merchant_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json item;
                item["NotificationID"] = sqlite3_column_int(stmt, 0);
                item["AppointmentID"] = sqlite3_column_int(stmt, 1);
                item["Message"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                item["UserID"] = sqlite3_column_int(stmt, 3);
                item["ServiceID"] = sqlite3_column_int(stmt, 4);
                item["AppointmentTime"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                item["Status"] = sqlite3_column_int(stmt, 6);
                json_response.push_back(item);
            }
        }
        sqlite3_finalize(stmt);
        res.set_content(json_response.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/merchant/appointments", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json json_response = nlohmann::json::array();
        int merchant_id = std::stoi(req.get_param_value("MerchantID"));
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT a.AppointmentID, a.UserID, a.ServiceID, a.AppointmentTime, a.Status, u.Username, s.ServiceName FROM t_appointment a JOIN t_user u ON a.UserID = u.UserID JOIN t_service s ON a.ServiceID = s.ServiceID WHERE s.MerchantID = ? AND a.Status = 1 ORDER BY a.AppointmentID DESC;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, merchant_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json item;
                item["AppointmentID"] = sqlite3_column_int(stmt, 0);
                item["UserID"] = sqlite3_column_int(stmt, 1);
                item["ServiceID"] = sqlite3_column_int(stmt, 2);
                item["AppointmentTime"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                item["Status"] = sqlite3_column_int(stmt, 4);
                item["StatusText"] = status_to_string(sqlite3_column_int(stmt, 4));
                item["Username"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                item["ServiceName"] = normalize_text_value(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)), "未命名服务");
                json_response.push_back(item);
            }
        }
        sqlite3_finalize(stmt);
        res.set_content(json_response.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/merchant/appointments/confirm", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int merchant_id = data["MerchantID"].get<int>();
            int appointment_id = data["AppointmentID"].get<int>();

            sqlite3_stmt* check_stmt = nullptr;
            const char* check_sql = "SELECT a.AppointmentID FROM t_appointment a JOIN t_service s ON a.ServiceID = s.ServiceID WHERE a.AppointmentID = ? AND s.MerchantID = ? AND a.Status = 1;";
            bool valid = false;
            if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(check_stmt, 1, appointment_id);
                sqlite3_bind_int(check_stmt, 2, merchant_id);
                if (sqlite3_step(check_stmt) == SQLITE_ROW) {
                    valid = true;
                }
            }
            sqlite3_finalize(check_stmt);

            if (!valid) {
                ret["success"] = false;
                ret["message"] = "Appointment not found or already handled";
                res.status = 400;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE t_appointment SET Status = 2 WHERE AppointmentID = ? AND Status = 1;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, appointment_id);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);

            sqlite3_stmt* notif_stmt = nullptr;
            const char* notif_sql = "UPDATE t_notification SET Status = 1 WHERE AppointmentID = ?;";
            if (sqlite3_prepare_v2(db, notif_sql, -1, &notif_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(notif_stmt, 1, appointment_id);
                sqlite3_step(notif_stmt);
            }
            sqlite3_finalize(notif_stmt);

            ret["success"] = true;
            ret["message"] = "Appointment confirmed and inventory locked";
            ret["AppointmentID"] = appointment_id;
            ret["Status"] = 2;
            ret["StatusText"] = status_to_string(2);
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/appointments/cancel", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int app_id = data["AppointmentID"].get<int>();

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE t_appointment SET Status = 3 WHERE AppointmentID = ? AND Status IN (1, 2);";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, app_id);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);

            ret["success"] = true;
            ret["message"] = "Appointment cancelled";
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/appointments/complete", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int app_id = data["AppointmentID"].get<int>();

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE t_appointment SET Status = 2 WHERE AppointmentID = ? AND Status IN (1, 2);";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, app_id);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);

            ret["success"] = true;
            ret["message"] = "Appointment completed";
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/reviews", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int app_id = data["AppointmentID"].get<int>();
            int rating = data["Rating"].get<int>();
            std::string comment = data["CommentContent"].get<std::string>();

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO t_review (AppointmentID, Rating, CommentContent) VALUES (?, ?, ?);";
            bool success = true;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, app_id);
                sqlite3_bind_int(stmt, 2, rating);
                sqlite3_bind_text(stmt, 3, comment.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) != SQLITE_DONE) {
                    success = false;
                }
            }
            sqlite3_finalize(stmt);

            if (success) {
                ret["success"] = true;
                ret["message"] = "Review submitted successfully";
            } else {
                ret["success"] = false;
                ret["message"] = "Appointment already reviewed";
                res.status = 400;
            }
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    std::cout << "Server started at http://0.0.0.0:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);

    sqlite3_close(db);
    return 0;
}
