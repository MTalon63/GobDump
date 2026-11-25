#include "db_handler.h"
#include "core/exception.h"
#include "logger.h"
#include "nlohmann/json.hpp"
#include "utils/string.h"

namespace satdump
{
    DBHandler::DBHandler(std::string path)
    {
        if (!std::filesystem::exists(std::filesystem::path(path).parent_path()) && std::filesystem::path(path).has_parent_path())
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());

        // Open
        if (sqlite3_open(path.c_str(), &db))
            throw satdump_exception("Couldn't open SQLite database! " + std::string(sqlite3_errmsg(db)));
        else
            logger->info("Opened SQLite Database!");
        sqlite3_busy_timeout(db, 1000);

        // The base tables must exist as soon as the file is open, NOT later in init(): init() is
        // only reached after the config is loaded, the plugins are loaded and db->get_user(
        // "language") has already been read. On a FRESH database all of that ran against an empty
        // file, so each use failed with "no such table: user" and startup-time settings were
        // silently dropped. Existing databases hid this because their tables came from a past run.
        std::string sql_create_meta = "CREATE TABLE IF NOT EXISTS meta("
                                      "id TEXT PRIMARY KEY NOT NULL,"
                                      "val TEXT NOT NULL);";
        if (run_sql(sql_create_meta))
            throw satdump_exception("Failed creating meta table!");

        std::string sql_create_user = "CREATE TABLE IF NOT EXISTS user("
                                      "id TEXT PRIMARY KEY NOT NULL,"
                                      "val TEXT NOT NULL);";
        if (run_sql(sql_create_user))
            throw satdump_exception("Failed creating user table!");
    }

    void DBHandler::init()
    {
        // meta and user are already created by the constructor -- they have to exist before the
        // config and plugins are loaded, which all happens before this is called.

        // Let everything else do the same!
        logger->info("Setting up database...");
        for (auto &s : subhandlers)
            s->init();
    }

    DBHandler::~DBHandler()
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        if (db != nullptr && sqlite3_close(db))
            logger->error("Error closing SQLite database!");
    }

    void DBHandler::set_meta(std::string id, std::string val)
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        char *err = NULL;
        replaceAllStr(val, "'", "''");
        std::string sql = "INSERT INTO meta (id, val) VALUES ('" + id + "', '" + val + "') ON CONFLICT(id) DO UPDATE SET val='" + val + "';";
        if (sqlite3_exec(db, sql.c_str(), NULL, 0, &err))
        {
            logger->error("Error setting value (%s = %s) in meta table! %s", id.c_str(), val.c_str(), err);
            sqlite3_free(err);
        }
    }

    std::string DBHandler::get_meta(std::string id, std::string def)
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        if (db == nullptr)
            return def;
        sqlite3_stmt *res = nullptr;

        if (sqlite3_prepare_v2(db, ("SELECT val from meta where id='" + id + "'").c_str(), -1, &res, 0) != SQLITE_OK)
        {
            logger->warn("%s", sqlite3_errmsg(db));
            return def;
        }

        if (sqlite3_step(res) == SQLITE_ROW)
        {
            const unsigned char *val = sqlite3_column_text(res, 0);
            if (val != nullptr)
                def = (const char *)val;
        }

        sqlite3_finalize(res);

        return def;
    }

    void DBHandler::set_user(std::string id, std::string val)
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        char *err = NULL;
        replaceAllStr(val, "'", "''");
        std::string sql = "INSERT INTO user (id, val) VALUES ('" + id + "', '" + val + "') ON CONFLICT(id) DO UPDATE SET val='" + val + "';";
        if (sqlite3_exec(db, sql.c_str(), NULL, 0, &err))
        {
            logger->error("Error setting value (%s = %s) in user table! %s", id.c_str(), val.c_str(), err);
            sqlite3_free(err);
        }
    }

    std::string DBHandler::get_user(std::string id, std::string def)
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        if (db == nullptr)
            return def;
        sqlite3_stmt *res = nullptr;

        if (sqlite3_prepare_v2(db, ("SELECT val from user where id='" + id + "'").c_str(), -1, &res, 0) != SQLITE_OK)
        {
            logger->warn("%s", sqlite3_errmsg(db));
            return def;
        }

        if (sqlite3_step(res) == SQLITE_ROW)
        {
            const unsigned char *val = sqlite3_column_text(res, 0);
            if (val != nullptr)
                def = (const char *)val;
        }

        sqlite3_finalize(res);

        return def;
    }

    void DBHandler::set_user_json(std::string id, nlohmann::json val) { set_user(id, val.dump()); }

    nlohmann::json DBHandler::get_user_json(std::string id, nlohmann::json def)
    {
        try
        {
            return nlohmann::json::parse(get_user(id, def.dump()));
        }
        catch (std::exception &e)
        {
            logger->warn("Error parsing JSON from DB : " + id);
            return def;
        }
    }

    int DBHandler::get_table_size(std::string table)
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        if (db == nullptr)
            return -1;
        sqlite3_stmt *res = nullptr;
        int r = -1;

        if (sqlite3_prepare_v2(db, ("SELECT count(*) from " + table).c_str(), -1, &res, 0) != SQLITE_OK)
        {
            logger->warn("Error getting table size! %s", sqlite3_errmsg(db));
            return r;
        }

        if (sqlite3_step(res) == SQLITE_ROW)
            r = sqlite3_column_int(res, 0);

        sqlite3_finalize(res);

        return r;
    }

    void DBHandler::tr_begin()
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        char *err = NULL;
        std::string sql = "BEGIN;";
        if (sqlite3_exec(db, sql.c_str(), NULL, 0, &err))
        {
            logger->error("Error running BEGIN on DB %s", err);
            sqlite3_free(err);
        }
    }

    void DBHandler::tr_end()
    {
        std::lock_guard<std::recursive_mutex> lock(db_mutex);
        char *err = NULL;
        std::string sql = "END;";
        if (sqlite3_exec(db, sql.c_str(), NULL, 0, &err))
        {
            logger->error("Error running END on DB %s", err);
            sqlite3_free(err);
        }
    }
} // namespace satdump