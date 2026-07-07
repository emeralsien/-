#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sqlite3.h>
#include "httplib.h"
#include "json.hpp"

sqlite3* db = nullptr;

void init_db() {
    int rc = sqlite3_open("appointment.db", &db);
    if (rc) {
        std::cerr << "Failed to open database" << std::endl;
        exit(1);
    }

    const char* sql_merchant = "CREATE TABLE IF NOT EXISTS t_merchant (MerchantID INTEGER PRIMARY KEY AUTOINCREMENT, ShopName TEXT, ContactInfo TEXT, Address TEXT);";
    const char* sql_user = "CREATE TABLE IF NOT EXISTS t_user (UserID INTEGER PRIMARY KEY AUTOINCREMENT, Username TEXT, PhoneNumber TEXT UNIQUE, Password TEXT);";
    const char* sql_service = "CREATE TABLE IF NOT EXISTS t_service (ServiceID INTEGER PRIMARY KEY AUTOINCREMENT, MerchantID INTEGER, ServiceName TEXT, Category TEXT, Price INTEGER, Duration INTEGER);";
    const char* sql_appointment = "CREATE TABLE IF NOT EXISTS t_appointment (AppointmentID INTEGER PRIMARY KEY AUTOINCREMENT, UserID INTEGER, ServiceID INTEGER, AppointmentTime TEXT, Status INTEGER);";
    const char* sql_review = "CREATE TABLE IF NOT EXISTS t_review (ReviewID INTEGER PRIMARY KEY AUTOINCREMENT, AppointmentID INTEGER UNIQUE, Rating INTEGER, CommentContent TEXT);";

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

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t_service;", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        if (count == 0) {
            char* err_msg = nullptr;
            if (sqlite3_exec(db, "INSERT INTO t_merchant (ShopName, ContactInfo, Address) VALUES ('Shop1', '13800001111', 'Address1');", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting merchant: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
            if (sqlite3_exec(db, "INSERT INTO t_user (Username, PhoneNumber, Password) VALUES ('User1', '13311112222', '123456');", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting user: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
            if (sqlite3_exec(db, "INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration) VALUES (1, 'Service1', 'Category1', 45, 30);", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting service 1: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
            if (sqlite3_exec(db, "INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration) VALUES (1, 'Service2', 'Category1', 120, 60);", nullptr, nullptr, &err_msg) != SQLITE_OK) {
                std::cerr << "Error inserting service 2: " << err_msg << std::endl;
                sqlite3_free(err_msg);
            }
        }
    }
    sqlite3_finalize(stmt);
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
        sqlite3_stmt* stmt;
        const char* sql = "SELECT ServiceID, MerchantID, ServiceName, Category, Price, Duration FROM t_service;";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json item;
                item["ServiceID"] = sqlite3_column_int(stmt, 0);
                item["MerchantID"] = sqlite3_column_int(stmt, 1);
                item["ServiceName"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                item["Category"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                item["Price"] = sqlite3_column_int(stmt, 4);
                item["Duration"] = sqlite3_column_int(stmt, 5);
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

            sqlite3_stmt* stmt;
            const char* sql = "INSERT INTO t_service (MerchantID, ServiceName, Category, Price, Duration) VALUES (?, ?, ?, ?, ?);";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, merchant_id);
                sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, cate.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 4, price);
                sqlite3_bind_int(stmt, 5, duration);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);

            ret["success"] = true;
            ret["message"] = "Service added successfully";
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/appointments", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int user_id = data["UserID"].get<int>();
            int service_id = data["ServiceID"].get<int>();
            std::string app_time = data["AppointmentTime"].get<std::string>();

            sqlite3_stmt* check_stmt;
            const char* check_sql = "SELECT COUNT(*) FROM t_appointment WHERE ServiceID = ? AND AppointmentTime = ? AND Status = 1;";
            int existing_count = 0;
            if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(check_stmt, 1, service_id);
                sqlite3_bind_text(check_stmt, 2, app_time.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(check_stmt) == SQLITE_ROW) {
                    existing_count = sqlite3_column_int(check_stmt, 0);
                }
            }
            sqlite3_finalize(check_stmt);

            if (existing_count >= 1) {
                ret["success"] = false;
                ret["message"] = "Timeslot already booked";
                res.status = 400;
                res.set_content(ret.dump(), "application/json; charset=utf-8");
                return;
            }

            sqlite3_stmt* stmt;
            const char* sql = "INSERT INTO t_appointment (UserID, ServiceID, AppointmentTime, Status) VALUES (?, ?, ?, 1);";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, user_id);
                sqlite3_bind_int(stmt, 2, service_id);
                sqlite3_bind_text(stmt, 3, app_time.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);

            ret["success"] = true;
            ret["message"] = "Appointment booked successfully";
        } catch (const std::exception& e) {
            ret["success"] = false;
            ret["message"] = std::string("Request format error: ") + e.what();
            res.status = 400;
        }
        res.set_content(ret.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/appointments", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json json_response = nlohmann::json::array();
        sqlite3_stmt* stmt;
        const char* sql = "SELECT a.AppointmentID, a.UserID, a.ServiceID, a.AppointmentTime, a.Status, u.Username, s.ServiceName, s.Price FROM t_appointment a JOIN t_user u ON a.UserID = u.UserID JOIN t_service s ON a.ServiceID = s.ServiceID ORDER BY a.AppointmentID DESC;";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                nlohmann::json item;
                item["AppointmentID"] = sqlite3_column_int(stmt, 0);
                item["UserID"] = sqlite3_column_int(stmt, 1);
                item["ServiceID"] = sqlite3_column_int(stmt, 2);
                item["AppointmentTime"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                item["Status"] = sqlite3_column_int(stmt, 4);
                item["Username"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                item["ServiceName"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                item["Price"] = sqlite3_column_int(stmt, 7);
                json_response.push_back(item);
            }
        }
        sqlite3_finalize(stmt);
        res.set_content(json_response.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/appointments/cancel", [](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json ret;
        try {
            auto data = nlohmann::json::parse(req.body);
            int app_id = data["AppointmentID"].get<int>();

            sqlite3_stmt* stmt;
            const char* sql = "UPDATE t_appointment SET Status = 3 WHERE AppointmentID = ? AND Status = 1;";
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

            sqlite3_stmt* stmt;
            const char* sql = "UPDATE t_appointment SET Status = 2 WHERE AppointmentID = ? AND Status = 1;";
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

            sqlite3_stmt* stmt;
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

    std::cout << "Server started at http://127.0.0.1:8080" << std::endl;
    svr.listen("127.0.0.1", 8080);

    sqlite3_close(db);
    return 0;
}
