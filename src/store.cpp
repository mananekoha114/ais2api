// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/store.hpp"
#include "ais2api/region.hpp"
#include "ais2api/usage.hpp"
#include "../browser/rate_policy.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <thread>
namespace ais2api {
namespace {
void migrate_request_usage(sqlite3 *db);
std::string sqlite_path(const fs::path &path) {
    auto u = path.u8string();
    return std::string(reinterpret_cast<const char *>(u.data()), u.size());
}
struct Statement {
    sqlite3_stmt *p = nullptr;
    Statement(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw Error(500, "database", sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(p); }
    void text(int i, std::string_view v) {
        if (sqlite3_bind_text(p, i, v.empty() ? "" : v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK)
            throw Error(500, "database", "Cannot bind database value");
    }
    void integer(int i, std::int64_t v) { sqlite3_bind_int64(p, i, v); }
    bool row() {
        auto r = sqlite3_step(p);
        if (r == SQLITE_ROW)
            return true;
        if (r == SQLITE_DONE)
            return false;
        throw Error(500, "database", sqlite3_errmsg(sqlite3_db_handle(p)));
    }
    std::string str(int i) {
        auto v = sqlite3_column_text(p, i);
        return v ? reinterpret_cast<const char *>(v) : "";
    }
};
std::int64_t next_touch(sqlite3 *db) {
    Statement s(db, "SELECT COALESCE(MAX(touched),0)+1 FROM tools");
    s.row();
    return sqlite3_column_int64(s.p, 0);
}
void exec(sqlite3 *db, const char *sql) {
    char *error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string s = error ? error : "Database error";
        sqlite3_free(error);
        throw Error(500, "database", s);
    }
}
json::array discovered_models(json::array values) {
    auto invalid = [] {
        throw Error(502, "model_discovery_invalid",
                    "Model discovery returned an invalid or empty list");
    };
    if (values.empty() || json::serialize(values).size() > 1024 * 1024)
        invalid();
    std::set<std::string> ids;
    for (auto &value : values) {
        if (!value.is_object())
            invalid();
        auto &item = value.as_object();
        for (auto key : {"id", "name"})
            if (auto field = item.if_contains(key); field && !field->is_string())
                invalid();
        const auto name = string_or(item, "name");
        auto id = string_or(item, "id");
        if (id.empty() && name.starts_with("models/"))
            id = name.substr(7);
        if (id.empty() || id.size() > 512 ||
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                std::string::npos ||
            (!name.empty() && name != "models/" + id) || !ids.insert(id).second)
            invalid();
        if (auto source = item.if_contains("source");
            source && (!source->is_string() || source->as_string() != "live"))
            invalid();
        for (auto key : {"displayName", "description"})
            if (auto field = item.if_contains(key); field && !field->is_string())
                invalid();
        for (auto key : {"inputTokenLimit", "outputTokenLimit"})
            if (auto field = item.if_contains(key);
                field && !field->is_uint64() && !(field->is_int64() && field->as_int64() >= 0))
                invalid();
        if (auto methods = item.if_contains("supportedGenerationMethods")) {
            if (!methods->is_array())
                invalid();
            for (auto &method : methods->as_array())
                if (!method.is_string())
                    invalid();
        }
        item["id"] = id;
        item["name"] = "models/" + id;
        item["source"] = "saved";
    }
    return values;
}
Account decode(Statement &s) {
    Account a;
    a.id = s.str(0);
    a.name = s.str(1);
    a.email = s.str(2);
    a.auth_user = s.str(3);
    a.storage_state = json::parse(s.str(4)).as_object();
    a.disabled = sqlite3_column_int(s.p, 5) != 0;
    return a;
}
json::object summary(const Account &a) {
    return {{"id", a.id},
            {"name", a.name},
            {"email", a.email},
            {"auth_user", a.auth_user},
            {"disabled", a.disabled}};
}
std::string import_text(std::string text) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    auto trim_front = [&] {
        auto first = text.find_first_not_of(whitespace);
        if (first == std::string::npos)
            text.clear();
        else
            text.erase(0, first);
    };
    trim_front();
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
        trim_front();
    }
    // Netscape's last tab can delimit an intentionally empty cookie value.
    // Preserve field whitespace there; remove only surrounding line endings.
    bool netscape = false;
    std::istringstream rows(text);
    std::string row;
    while (std::getline(rows, row))
        if (std::count(row.begin(), row.end(), '\t') >= 6) {
            netscape = true;
            break;
        }
    auto last = text.find_last_not_of(netscape ? std::string_view("\r\n") : whitespace);
    if (last == std::string::npos)
        text.clear();
    else
        text.erase(last + 1);
    return text;
}
void cookie_count(std::size_t count, std::size_t limit) {
    if (count > limit)
        throw Error(413, "cookie_import_limit", "Cookie import exceeds the credential count limit");
}
void cookie_count(const json::value &v, std::size_t limit) {
    const json::value *cookies = &v;
    if (v.is_object()) cookies = v.as_object().if_contains("cookies");
    if (cookies && cookies->is_array()) cookie_count(cookies->as_array().size(), limit);
    if (cookies && cookies->is_object()) cookie_count(cookies->as_object().size(), limit);
}
json::object state(const json::value &input, bool *raw_header = nullptr,
                   std::size_t max_cookies = std::numeric_limits<std::size_t>::max()) {
    cookie_count(input, max_cookies);
    auto v = input;
    if (raw_header) *raw_header = false;
    if (v.is_string()) {
        auto raw = import_text(std::string(v.as_string()));
        try {
            v = json::parse(raw);
        } catch (...) {
            json::array cs;
            if (raw.find('\t') != std::string::npos) {
                std::istringstream lines(raw);
                std::string line;
                while (std::getline(lines, line)) {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    bool http = false;
                    if (line.starts_with("#HttpOnly_")) {
                        http = true;
                        line.erase(0, 10);
                    } else if (line.find_first_not_of(" \t\r") == std::string::npos ||
                               line[0] == '#')
                        continue;
                    std::vector<std::string> columns;
                    std::size_t start = 0;
                    for (int n = 0; n < 6; ++n) {
                        auto end = line.find('\t', start);
                        if (end == std::string::npos)
                            break;
                        columns.push_back(line.substr(start, end - start));
                        start = end + 1;
                    }
                    if (columns.size() != 6)
                        throw Error(400, "invalid_credentials", "Invalid Netscape cookie row");
                    columns.push_back(line.substr(start));
                    json::object cookie{{"domain", columns[0]},
                                        {"path", columns[2]},
                                        {"secure", columns[3] == "TRUE"},
                                        {"httpOnly", http},
                                        {"name", columns[5]},
                                        {"value", columns[6]}};
                    if (columns[4] != "0") {
                        try {
                            std::size_t end = 0;
                            auto expires = std::stoll(columns[4], &end);
                            if (end != columns[4].size())
                                throw std::invalid_argument("expiry");
                            cookie["expires"] = expires;
                        } catch (...) {
                            throw Error(400, "invalid_credentials", "Invalid cookie expiry");
                        }
                    }
                    cookie_count(cs.size() + 1, max_cookies);
                    cs.push_back(cookie);
                }
                return state(json::object{{"cookies", cs}}, nullptr, max_cookies);
            }
            if (raw_header) *raw_header = true;
            if (raw.starts_with("Cookie:") || raw.starts_with("cookie:"))
                raw.erase(0, 7);
            if (raw.find_first_of("\r\n") != std::string::npos)
                throw Error(400, "invalid_credentials",
                            "Cookie header contains an internal line break");
            std::size_t pos = 0;
            while (pos < raw.size()) {
                auto end = raw.find(';', pos);
                auto part = raw.substr(pos, end == std::string::npos ? end : end - pos);
                auto eq = part.find('=');
                if (eq != std::string::npos) {
                    auto k = part.substr(0, eq);
                    k.erase(0, k.find_first_not_of(" \t"));
                    k.erase(k.find_last_not_of(" \t") + 1);
                    if (!k.empty()) {
                        cookie_count(cs.size() + 1, max_cookies);
                        cs.push_back(json::object{{"name", k},
                                                  {"value", part.substr(eq + 1)},
                                                  {"domain", ".google.com"},
                                                  {"path", "/"},
                                                  {"secure", true},
                                                  {"httpOnly", true}});
                    }
                }
                if (end == std::string::npos)
                    break;
                pos = end + 1;
            }
            v = json::object{{"cookies", cs}};
        }
    }
    cookie_count(v, max_cookies);
    if (v.is_array())
        v = json::object{{"cookies", v}};
    if (!v.is_object())
        throw Error(400, "invalid_credentials", "Expected cookie array or storage state");
    auto o = v.as_object();
    auto cookies = o.if_contains("cookies");
    if (!cookies)
        throw Error(400, "invalid_credentials", "Missing cookies");
    if (cookies->is_string()) {
        auto nested = state(*cookies, raw_header, max_cookies);
        o["cookies"] = nested.at("cookies");
        if (auto origins = nested.if_contains("origins")) {
            auto outer = o.if_contains("origins");
            if (!outer) o["origins"] = *origins;
            else if (outer->is_array())
                for (const auto &origin : origins->as_array()) outer->as_array().push_back(origin);
        }
        cookies = o.if_contains("cookies");
    }
    if (cookies->is_object()) {
        json::array mapped;
        for (auto &p : cookies->as_object()) {
            if (!p.value().is_string())
                throw Error(400, "invalid_credentials", "Cookie map values must be strings");
            mapped.push_back(json::object{{"name", p.key()},
                                          {"value", p.value()},
                                          {"domain", ".google.com"},
                                          {"path", "/"},
                                          {"secure", true}});
        }
        o["cookies"] = mapped;
        cookies = o.if_contains("cookies");
    }
    if (!cookies->is_array() || cookies->as_array().empty())
        throw Error(400, "invalid_credentials", "Cookie array is empty");
    for (auto &cv : cookies->as_array()) {
        if (!cv.is_object())
            throw Error(400, "invalid_credentials", "Invalid cookie");
        auto &c = cv.as_object();
        auto domain = string_or(c, "domain");
        if (domain.empty() || domain.find_first_of("/\\ \t\r\n") != std::string::npos ||
            string_or(c, "name").empty() || !c.if_contains("value") || !c.at("value").is_string())
            throw Error(400, "invalid_credentials",
                        "Cookie name, value and valid domain are required");
        bool domain_ok = true;
        for (unsigned char ch : domain)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '-'))
                domain_ok = false;
        if (!domain_ok || domain == "." || domain.find("..") != std::string::npos ||
            domain.back() == '.')
            throw Error(400, "invalid_credentials", "Invalid cookie domain");
        auto cookie_name = string_or(c, "name");
        if (cookie_name.find_first_of("\r\n\t ;=") != std::string::npos ||
            string_or(c, "value").find_first_of("\r\n") != std::string::npos)
            throw Error(400, "invalid_credentials", "Invalid cookie name or value");
        for (auto flag : {"secure", "httpOnly"})
            if (auto p = c.if_contains(flag); p && !p->is_bool())
                throw Error(400, "invalid_credentials", "Cookie flags must be boolean");
        if (auto p = c.if_contains("sameSite");
            p && (!p->is_string() || (p->as_string() != "Strict" && p->as_string() != "Lax" &&
                                      p->as_string() != "None")))
            throw Error(400, "invalid_credentials", "Invalid sameSite metadata");
        if (auto p = c.if_contains("expires"); p && !p->is_number())
            throw Error(400, "invalid_credentials", "Cookie expiry must be numeric");
        if (!c.if_contains("path"))
            c["path"] = "/";
        if (!c.at("path").is_string() || !string_or(c, "path").starts_with('/'))
            throw Error(400, "invalid_credentials", "Invalid cookie path");
    }
    if (auto p = o.if_contains("origins"); p && !p->is_array())
        throw Error(400, "invalid_credentials", "Invalid storage origins");
    return o;
}
} // namespace
struct Store::Impl {
    sqlite3 *db = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
    std::thread worker;
    explicit Impl(const fs::path &directory) {
        secure_directory(directory);
        auto path = directory / "state.sqlite3";
        if (fs::is_symlink(fs::symlink_status(path)))
            throw Error(400, "unsafe_path", "Database cannot be a symbolic link");
        std::promise<void> ready;
        auto f = ready.get_future();
        worker = std::thread([this, path, &ready] {
            try {
                if (sqlite3_open_v2(sqlite_path(path).c_str(), &db,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                        SQLITE_OPEN_NOMUTEX,
                                    nullptr) != SQLITE_OK)
                    throw Error(500, "database", "Cannot open state database");
                sqlite3_busy_timeout(db, 5000);
                int schema_version = 0;
                {
                    Statement version(db, "PRAGMA user_version");
                    if (version.row()) schema_version = sqlite3_column_int(version.p, 0);
                    if (schema_version > 3)
                        throw Error(500, "database_version",
                                    "Database was created by a newer ais2api version");
                }
                exec(db, "PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;CREATE TABLE IF "
                         "NOT EXISTS accounts(id TEXT PRIMARY KEY,name TEXT,email "
                         "TEXT,auth_user TEXT,state TEXT,disabled INTEGER DEFAULT 0,expired "
                         "INTEGER DEFAULT 0);CREATE TABLE IF NOT EXISTS settings(k TEXT "
                         "PRIMARY KEY,v TEXT);CREATE TABLE IF NOT EXISTS failures(account "
                         "TEXT REFERENCES accounts(id) ON DELETE CASCADE,model TEXT,until "
                         "INTEGER,count INTEGER,PRIMARY KEY(account,model));CREATE TABLE IF NOT EXISTS "
                         "quota_counters(account TEXT REFERENCES accounts(id) ON DELETE CASCADE,"
                         "model TEXT,count INTEGER NOT NULL,day INTEGER NOT NULL,cooldown_until INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(account,model));CREATE TABLE IF "
                         "NOT EXISTS sticky(model TEXT PRIMARY KEY,account TEXT REFERENCES "
                         "accounts(id) ON DELETE CASCADE);CREATE TABLE IF NOT EXISTS "
                         "stats(account TEXT,model TEXT,ok INTEGER DEFAULT 0,failed INTEGER "
                         "DEFAULT 0,usage TEXT DEFAULT '{}',PRIMARY "
                         "KEY(account,model));CREATE TABLE IF NOT EXISTS keys(id TEXT "
                         "PRIMARY KEY,name TEXT,hash TEXT UNIQUE,created INTEGER);CREATE TABLE IF NOT EXISTS "
                         "managed_keys(id TEXT PRIMARY KEY,name TEXT,hash TEXT UNIQUE,created INTEGER);CREATE "
                         "TABLE IF NOT EXISTS tools(principal TEXT,call_id TEXT,metadata "
                         "TEXT,created INTEGER,touched INTEGER,PRIMARY "
                         "KEY(principal,call_id));CREATE INDEX IF NOT EXISTS tools_lru ON "
                         "tools(touched);CREATE INDEX IF NOT EXISTS tools_expiry ON "
                         "tools(created);");
                bool quota_deadline = false;
                { Statement columns(db, "PRAGMA table_info(quota_counters)");
                  while (columns.row()) if (columns.str(1) == "cooldown_until") quota_deadline = true; }
                if (!quota_deadline) exec(db, "ALTER TABLE quota_counters ADD COLUMN cooldown_until INTEGER NOT NULL DEFAULT 0");
                // `kind` records the Store::failure kind; `probed` marks an auth
                // cooldown whose single probe attempt was already admitted.
                bool failure_kind = false, failure_probed = false;
                { Statement columns(db, "PRAGMA table_info(failures)");
                  while (columns.row()) { failure_kind |= columns.str(1) == "kind"; failure_probed |= columns.str(1) == "probed"; } }
                if (!failure_kind) exec(db, "ALTER TABLE failures ADD COLUMN kind TEXT NOT NULL DEFAULT ''");
                if (!failure_probed) exec(db, "ALTER TABLE failures ADD COLUMN probed INTEGER NOT NULL DEFAULT 0");
                // Every account owns a permanent browser fingerprint; accounts
                // from older stores receive one the first time the store opens.
                exec(db, "CREATE TABLE IF NOT EXISTS fingerprints(account TEXT PRIMARY KEY REFERENCES "
                         "accounts(id) ON DELETE CASCADE,seed INTEGER NOT NULL,timezone TEXT NOT NULL "
                         "DEFAULT '',locale TEXT NOT NULL DEFAULT '');INSERT OR IGNORE INTO "
                         "fingerprints(account,seed) SELECT id,10000+abs(random()%90000) FROM accounts");
                exec(db, "CREATE TABLE IF NOT EXISTS model_catalog(account TEXT PRIMARY KEY REFERENCES "
                         "accounts(id) ON DELETE CASCADE,models TEXT NOT NULL,saved_at INTEGER NOT NULL)");
                exec(db, "BEGIN IMMEDIATE;"
                         "CREATE TABLE IF NOT EXISTS request_history(seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "id TEXT NOT NULL UNIQUE,created_at INTEGER NOT NULL,model TEXT NOT NULL DEFAULT '',"
                         "account TEXT NOT NULL DEFAULT '',status TEXT NOT NULL,terminal INTEGER NOT NULL DEFAULT 0,"
                         "detail TEXT NOT NULL);"
                         "CREATE INDEX IF NOT EXISTS request_history_time ON request_history(created_at);"
                         "CREATE INDEX IF NOT EXISTS request_history_model ON request_history(model,seq);"
                         "CREATE INDEX IF NOT EXISTS request_history_account ON request_history(account,seq);"
                         "CREATE INDEX IF NOT EXISTS request_history_status ON request_history(status,seq);"
                         "CREATE TABLE IF NOT EXISTS request_totals(account TEXT NOT NULL,model TEXT NOT NULL,"
                         "detail TEXT NOT NULL,PRIMARY KEY(account,model));"
                         "CREATE TABLE IF NOT EXISTS price_sources(source TEXT PRIMARY KEY,catalog TEXT NOT NULL,"
                         "fetched_at INTEGER NOT NULL,attempted_at INTEGER NOT NULL,error TEXT NOT NULL);"
                         "INSERT OR IGNORE INTO settings(k,v) VALUES('usage_since',CAST(unixepoch()*1000 AS TEXT));"
                         "INSERT OR IGNORE INTO settings(k,v) SELECT 'usage_detail_since',v FROM settings WHERE k='usage_since';"
                         "COMMIT;");
                if (schema_version < 3) migrate_request_usage(db);
                ready.set_value();
            } catch (...) {
                ready.set_exception(std::current_exception());
                if (db)
                    sqlite3_close(db);
                db = nullptr;
                return;
            }
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (stopping && queue.empty())
                        break;
                    task = std::move(queue.front());
                    queue.pop_front();
                }
                task();
            }
            sqlite3_close(db);
            db = nullptr;
        });
        try {
            f.get();
        } catch (...) {
            worker.join();
            throw;
        }
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_one();
        if (worker.joinable())
            worker.join();
    }
    template <class F> auto run(F f) -> decltype(f()) {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        auto future = task->get_future();
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw Error(503, "database", "Store is shutting down");
            queue.emplace_back([task] { (*task)(); });
        }
        cv.notify_one();
        return future.get();
    }
    void put(const Account &a) {
        Statement s(db, "INSERT INTO accounts(id,name,email,auth_user,state,disabled) "
                        "VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
                        "name=excluded.name,email=excluded.email,auth_user=excluded."
                        "auth_user,state=excluded.state,disabled=excluded.disabled");
        s.text(1, a.id);
        s.text(2, a.name);
        s.text(3, a.email);
        s.text(4, a.auth_user);
        s.text(5, json::serialize(a.storage_state));
        s.integer(6, a.disabled);
        s.row();
        // New accounts get their permanent fingerprint; updates keep it.
        Statement fingerprint(db, "INSERT OR IGNORE INTO fingerprints(account,seed) VALUES(?,?)");
        fingerprint.text(1, a.id);
        fingerprint.integer(2, default_fingerprint_seed(a.id));
        fingerprint.row();
    }
};
Store::Store(const fs::path &d) : impl_(std::make_unique<Impl>(d)) {}
Store::~Store() = default;
std::vector<Account> Store::accounts() {
    return impl_->run([&] {
        std::vector<Account> out;
        Statement s(impl_->db, "SELECT id,name,email,auth_user,state,disabled FROM "
                               "accounts ORDER BY rowid");
        while (s.row())
            out.push_back(decode(s));
        return out;
    });
}
Account Store::account(std::string_view id) {
    return impl_->run([&] {
        Statement s(impl_->db, "SELECT id,name,email,auth_user,state,disabled FROM "
                               "accounts WHERE id=?");
        s.text(1, id);
        if (!s.row())
            throw Error(404, "account_not_found", "Account not found");
        return decode(s);
    });
}
json::object Store::normalize_storage_state(const json::value &input, bool *raw_header, std::size_t max_cookies) {
    return state(input, raw_header, max_cookies);
}
json::array Store::merge_verified_accounts(const std::vector<Account> &input) {
    auto parsed = input;
    for (auto &a : parsed) {
        if (a.email.empty() || a.email.size() > 320 || a.id.empty() ||
            a.email.find_first_of(" \t\r\n") != std::string::npos ||
            a.auth_user.empty() || a.auth_user.size() > 10 ||
            a.auth_user.find_first_not_of("0123456789") != std::string::npos)
            throw Error(400, "invalid_account", "Verified account identity required");
        a.storage_state = state(a.storage_state);
        a.disabled = false;
    }
    return impl_->run([&] {
        json::array out;
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            for (auto a : parsed) {
                Statement existing(impl_->db, "SELECT id,name FROM accounts WHERE email=? COLLATE NOCASE ORDER BY rowid LIMIT 1");
                existing.text(1, a.email);
                if (existing.row()) {
                    a.id = existing.str(0);
                    a.name = existing.str(1);
                } else {
                    // Candidate IDs must never overwrite an unrelated stored identity.
                    Statement collision(impl_->db, "SELECT 1 FROM accounts WHERE id=?");
                    collision.text(1, a.id);
                    if (collision.row())
                        a.id = "acc_" + random_hex(16);
                }
                impl_->put(a);
                Statement reset(impl_->db, "UPDATE accounts SET expired=0 WHERE id=?");
                reset.text(1, a.id); reset.row();
                // Credential replacement unlocks auth, preserving quota windows and counters.
                Statement failures(impl_->db, "DELETE FROM failures WHERE account=? AND model<>'*'");
                failures.text(1, a.id); failures.row();
                Statement quota(impl_->db, "INSERT INTO failures(account,model,until,count) "
                                          "SELECT account,model,cooldown_until,0 FROM quota_counters "
                                          "WHERE account=? AND cooldown_until>? ON CONFLICT(account,model) "
                                          "DO UPDATE SET until=MAX(failures.until,excluded.until)");
                quota.text(1, a.id); quota.integer(2, unix_seconds()); quota.row();
                out.push_back(summary(a));
            }
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
        return out;
    });
}
void Store::put_account(const Account &a) {
    auto copy = a;
    if (copy.id.empty())
        throw Error(400, "invalid_account", "Account ID required");
    copy.storage_state = state(copy.storage_state);
    impl_->run([&] { impl_->put(copy); });
}
std::optional<json::object> Store::refresh_cookies(std::string_view id, const json::object &expected,
                                                   const json::array &cookies) {
    auto next = expected;
    next["cookies"] = cookies;
    next = state(next);
    return impl_->run([&]() -> std::optional<json::object> {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement current(impl_->db, "SELECT state FROM accounts WHERE id=?");
            current.text(1, id);
            // A concurrent import or deletion wins over a browser write-back.
            if (!current.row() || json::parse(current.str(0)) != json::value(expected) ||
                json::value(next) == json::value(expected)) {
                exec(impl_->db, "ROLLBACK");
                return std::nullopt;
            }
            Statement update(impl_->db, "UPDATE accounts SET state=? WHERE id=?");
            update.text(1, json::serialize(next));
            update.text(2, id);
            update.row();
            exec(impl_->db, "COMMIT");
            return next;
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
void Store::update_email(std::string_view id, std::string_view email) {
    if (email.size() > 320) throw Error(400, "invalid_account", "Email is too long");
    impl_->run([&] {
        Statement s(impl_->db, "UPDATE accounts SET email=? WHERE id=?");
        s.text(1, email); s.text(2, id); s.row();
        if (!sqlite3_changes(impl_->db)) throw Error(404, "account_not_found", "Account not found");
    });
}
void Store::update_account_metadata(std::string_view id, const json::object& patch) {
    for (const auto& item : patch) {
        if (item.key() == "name") {
            if (!item.value().is_string() || item.value().as_string().empty() || item.value().as_string().size() > 200)
                throw Error(400, "invalid_account", "Name must contain 1 to 200 bytes");
        } else if (item.key() == "disabled") {
            if (!item.value().is_bool()) throw Error(400, "invalid_account", "Disabled must be boolean");
        } else throw Error(400, "invalid_account", "Unknown account metadata field");
    }
    impl_->run([&] {
        Statement s(impl_->db, "UPDATE accounts SET name=CASE WHEN ? THEN ? ELSE name END,disabled=CASE WHEN ? THEN ? ELSE disabled END WHERE id=?");
        auto name = patch.if_contains("name"), disabled = patch.if_contains("disabled");
        s.integer(1, name != nullptr); s.text(2, name ? std::string_view(name->as_string()) : std::string_view{});
        s.integer(3, disabled != nullptr); s.integer(4, disabled && disabled->as_bool());
        s.text(5, id); s.row();
        if (!sqlite3_changes(impl_->db)) throw Error(404, "account_not_found", "Account not found");
    });
}
json::array Store::import_accounts(const json::value &input, std::string name) {
    json::value v = input;
    if (v.is_string()) {
        v = json::string(import_text(std::string(v.as_string())));
        try {
            v = json::parse(v.as_string());
        } catch (...) {
        }
    }
    json::array entries;
    if (v.is_object() &&
        (v.as_object().if_contains("accounts") || v.as_object().if_contains("profiles"))) {
        auto &o = v.as_object();
        auto p = o.if_contains("accounts");
        if (!p)
            p = o.if_contains("profiles");
        if (!p->is_array())
            throw Error(400, "invalid_credentials", "Bundle must contain account array");
        entries = p->as_array();
    } else if (v.is_array() && !v.as_array().empty() && v.as_array()[0].is_object() &&
               (v.as_array()[0].as_object().if_contains("cookies") ||
                v.as_array()[0].as_object().if_contains("storage_state")))
        entries = v.as_array();
    else
        entries.push_back(v);
    std::vector<Account> parsed;
    for (auto &entry : entries) {
        Account a;
        a.id = "acc_" + random_hex(8);
        a.name = name.empty() ? "Imported account" : name;
        if (entry.is_object()) {
            auto &o = entry.as_object();
            a.name = string_or(o, "name", a.name);
            a.email = string_or(o, "email");
            a.auth_user = string_or(o, "auth_user", "0");
            if (auto p = o.if_contains("storage_state"))
                a.storage_state = state(*p);
            else
                a.storage_state = state(entry);
        } else
            a.storage_state = state(entry);
        if (a.auth_user.empty() || a.auth_user.size() > 10 ||
            a.auth_user.find_first_not_of("0123456789") != std::string::npos)
            throw Error(400, "invalid_credentials", "auth_user must be a decimal account index");
        parsed.push_back(std::move(a));
    }
    if (parsed.empty())
        throw Error(400, "invalid_credentials", "No accounts supplied");
    return impl_->run([&] {
        json::array out;
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            for (auto &a : parsed) {
                impl_->put(a);
                out.push_back(summary(a));
            }
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
        return out;
    });
}
void Store::remove_account(std::string_view id) {
    impl_->run([&] {
        Statement s(impl_->db, "DELETE FROM accounts WHERE id=?");
        s.text(1, id);
        s.row();
    });
}
void Store::select_account(std::string_view id) {
    impl_->run([&] {
        Statement exists(impl_->db, "SELECT 1 FROM accounts WHERE id=?");
        exists.text(1, id);
        if (!exists.row())
            throw Error(404, "account_not_found", "Account not found");
        // An explicit choice also becomes the current account immediately.
        Statement s(impl_->db, "INSERT INTO settings(k,v) VALUES('selected',?1),('current',?1) ON "
                               "CONFLICT(k) DO UPDATE SET v=excluded.v");
        s.text(1, id);
        s.row();
        exec(impl_->db, "DELETE FROM sticky");
    });
}
void Store::reset_account(std::string_view id) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement a(impl_->db, "UPDATE accounts SET expired=0,disabled=0 WHERE id=?");
            a.text(1, id);
            a.row();
            Statement f(impl_->db, "DELETE FROM failures WHERE account=?");
            f.text(1, id);
            f.row();
            Statement quota(impl_->db, "DELETE FROM quota_counters WHERE account=?");
            quota.text(1, id); quota.row();
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
std::optional<Account> Store::choose(std::string_view model,
                                     const std::vector<std::string> &excluded, bool auth_probe) {
    return impl_->run([&]() -> std::optional<Account> {
        const auto now = unix_seconds();
        // Stay on the account that last completed a generation (the browser's
        // resident session) while it can serve the model; switching rebuilds
        // the page and may restart the browser for another fingerprint. Then
        // the preferred account, then the account that last served the model.
        constexpr std::string_view preference =
            " ORDER BY (a.id=(SELECT v FROM settings WHERE k='current')) DESC,(a.id=(SELECT v FROM "
            "settings WHERE k='selected')) DESC,(a.id=(SELECT account FROM sticky WHERE model=?3)) DESC,a.rowid";
        auto pick = [&](std::string_view filter) -> std::optional<Account> {
            Statement s(impl_->db, (std::string(filter) + std::string(preference)).c_str());
            s.text(1, model);
            s.integer(2, now);
            s.text(3, model);
            while (s.row()) {
                auto a = decode(s);
                if (std::find(excluded.begin(), excluded.end(), a.id) != excluded.end())
                    continue;
                return a;
            }
            return {};
        };
        if (auto a = pick("SELECT a.id,a.name,a.email,a.auth_user,a.state,a.disabled FROM "
                          "accounts a WHERE a.disabled=0 AND a.expired=0 AND NOT EXISTS(SELECT 1 "
                          "FROM failures f WHERE f.account=a.id AND (f.model=?1 OR f.model='*') "
                          "AND f.until>?2)"))
            return a;
        if (!auth_probe) return {};
        // Every account is cooling down. An auth cooldown alone may be stale
        // (the session problem resolved itself), so admit one account blocked
        // only by it for a single attempt per cooldown; quota, permission,
        // expiry and disabled states are never bypassed.
        auto a = pick("SELECT a.id,a.name,a.email,a.auth_user,a.state,a.disabled FROM "
                      "accounts a WHERE a.disabled=0 AND a.expired=0 AND EXISTS(SELECT 1 FROM "
                      "failures f WHERE f.account=a.id AND f.model='*' AND f.kind='auth' AND "
                      "f.probed=0 AND f.until>?2) AND NOT EXISTS(SELECT 1 FROM failures f WHERE "
                      "f.account=a.id AND (f.model=?1 OR f.model='*') AND f.until>?2 AND NOT "
                      "(f.model='*' AND f.kind='auth' AND f.probed=0))");
        if (a) {
            Statement probed(impl_->db, "UPDATE failures SET probed=1 WHERE account=? AND model='*' AND kind='auth'");
            probed.text(1, a->id);
            probed.row();
        }
        return a;
    });
}
ModelCatalog Store::model_catalog() {
    return impl_->run([&] {
        ModelCatalog result;
        Statement row(impl_->db,
            "SELECT a.id,m.models,m.saved_at FROM accounts a LEFT JOIN model_catalog m ON m.account=a.id "
            "ORDER BY (a.id=(SELECT v FROM settings WHERE k='current')) DESC,"
            "(a.id=(SELECT v FROM settings WHERE k='selected')) DESC,a.rowid LIMIT 1");
        if (row.row()) {
            result.account_id = row.str(0);
            auto models = row.str(1);
            if (!models.empty()) {
                result.models = json::parse(models).as_array();
                result.saved_at = sqlite3_column_int64(row.p, 2);
            }
        }
        return result;
    });
}
ModelCatalog Store::save_models(std::string_view account_id, json::array models) {
    auto values = discovered_models(std::move(models));
    return impl_->run([&] {
        const auto saved_at = unix_seconds();
        // One statement replaces the entire snapshot in an implicit transaction.
        Statement save(impl_->db, "INSERT INTO model_catalog(account,models,saved_at) VALUES(?,?,?) "
                                 "ON CONFLICT(account) DO UPDATE SET models=excluded.models,saved_at=excluded.saved_at");
        save.text(1, account_id);
        save.text(2, json::serialize(values));
        save.integer(3, saved_at);
        save.row();
        return ModelCatalog{std::string(account_id), std::move(values), saved_at};
    });
}
json::object Store::rotation_status() {
    auto result = impl_->run([&] {
        json::object out{{"preferred_account_id", nullptr}, {"next_account_id", nullptr}};
        Statement selected(impl_->db, "SELECT a.id FROM accounts a JOIN settings s ON s.v=a.id WHERE s.k='selected'");
        if (selected.row()) out["preferred_account_id"] = selected.str(0);
        Statement next(impl_->db,
            "SELECT a.id FROM accounts a WHERE a.disabled=0 AND a.expired=0 AND NOT EXISTS(SELECT 1 FROM failures f WHERE f.account=a.id AND f.model='*' AND f.until>?) ORDER BY (a.id=(SELECT v FROM settings WHERE k='current')) DESC,(a.id=(SELECT v FROM settings WHERE k='selected')) DESC,a.rowid LIMIT 1");
        next.integer(1, unix_seconds());
        if (next.row()) out["next_account_id"] = next.str(0);
        return out;
    });
    result["accounts"] = account_status();
    return result;
}
std::string Store::rotate_account() {
    return impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement next(impl_->db,
                "SELECT a.id FROM accounts a WHERE a.disabled=0 AND a.expired=0 AND NOT EXISTS(SELECT 1 FROM failures f WHERE f.account=a.id AND f.model='*' AND f.until>?) ORDER BY (a.rowid<=COALESCE((SELECT p.rowid FROM accounts p JOIN settings s ON s.v=p.id WHERE s.k='selected'),0)),a.rowid LIMIT 1");
            next.integer(1, unix_seconds());
            if (!next.row()) throw Error(503, "no_available_account", "No globally available account");
            auto id = next.str(0);
            Statement selected(impl_->db, "INSERT INTO settings(k,v) VALUES('selected',?1),('current',?1) ON CONFLICT(k) DO UPDATE SET v=excluded.v");
            selected.text(1, id);
            selected.row();
            exec(impl_->db, "DELETE FROM sticky");
            exec(impl_->db, "COMMIT");
            return id;
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
void Store::clear_cooldown(std::string_view id, std::string_view model) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement exists(impl_->db, "SELECT 1 FROM accounts WHERE id=?");
            exists.text(1, id);
            if (!exists.row()) throw Error(404, "account_not_found", "Account not found");
            Statement clear(impl_->db, model.empty() ? "DELETE FROM failures WHERE account=?" : "DELETE FROM failures WHERE account=? AND model=?");
            clear.text(1, id);
            if (!model.empty()) clear.text(2, model);
            clear.row();
            Statement quota(impl_->db, model.empty() ? "DELETE FROM quota_counters WHERE account=?" : "DELETE FROM quota_counters WHERE account=? AND model=?");
            quota.text(1, id);
            if (!model.empty()) quota.text(2, model);
            quota.row();
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
void Store::reset_stats() {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement active(impl_->db, "SELECT 1 FROM request_history WHERE terminal=0 LIMIT 1");
            if (active.row()) throw Error(409, "requests_active", "Wait for active requests before resetting statistics");
            exec(impl_->db, "DELETE FROM stats;DELETE FROM request_history;DELETE FROM request_totals;"
                            "UPDATE settings SET v=CAST(unixepoch()*1000 AS TEXT) WHERE k IN ('usage_since','usage_detail_since');COMMIT");
        } catch (...) { exec(impl_->db, "ROLLBACK"); throw; }
    });
}
void Store::failure(std::string_view id, std::string_view model, std::string_view kind,
                    unsigned retry) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            if (kind == "login" || kind == "expired" || kind == "login_expired" ||
                kind == "authentication") {
                Statement s(impl_->db, "UPDATE accounts SET expired=1 WHERE id=?");
                s.text(1, id);
                s.row();
            } else {
                // Authentication trouble affects every model of the account.
                std::string m = (kind == "429" || kind == "rate_limit" || kind == "auth") ? "*" : std::string(model);
                Statement prior(impl_->db,
                                "SELECT count FROM failures WHERE account=? AND model=?");
                prior.text(1, id);
                prior.text(2, m);
                int count = prior.row() ? sqlite3_column_int(prior.p, 0) : 0;
                const bool minute = kind == "rate_limit_minute";
                const bool quota_error = minute || kind == "rate_limit_day" || kind == "rate_limit_model";
                unsigned base_delay = std::min<unsigned>(600, 60u << std::clamp(count, 0, 4));
                const auto current_time = unix_seconds();
                const auto day = browser_detail::pacific_quota_day(current_time);
                int quota_count = 0;
                std::int64_t quota_deadline = 0;
                if (quota_error) {
                    Statement prior_quota(impl_->db, "SELECT count,day,cooldown_until FROM quota_counters WHERE account=? AND model=?");
                    prior_quota.text(1, id); prior_quota.text(2, m);
                    if (prior_quota.row()) {
                        quota_deadline = sqlite3_column_int64(prior_quota.p, 2);
                        if (sqlite3_column_int64(prior_quota.p, 1) == day)
                            quota_count = sqlite3_column_int(prior_quota.p, 0);
                    }
                    if (!minute) quota_count = std::min(quota_count, 4) + 1;
                    base_delay = minute || quota_count <= 2 ? 60u : quota_count <= 4 ? 300u : 600u;
                } else {
                    if (kind == "permission_temporary" || kind == "auth") base_delay = 300;
                    Statement active_quota(impl_->db, "SELECT cooldown_until FROM quota_counters WHERE account=? AND model=?");
                    active_quota.text(1, id); active_quota.text(2, m);
                    if (active_quota.row()) quota_deadline = sqlite3_column_int64(active_quota.p, 0);
                }
                const auto delay = std::max(base_delay, std::min(retry, 86400u));
                auto until =
                    (kind == "permission" || kind == "403") ? INT64_MAX : current_time + delay;
                until = std::max(until, quota_deadline);
                if (quota_error) {
                    Statement save_quota(impl_->db, "INSERT INTO quota_counters(account,model,count,day,cooldown_until) VALUES(?,?,?,?,?) ON CONFLICT(account,model) DO UPDATE SET count=excluded.count,day=excluded.day,cooldown_until=excluded.cooldown_until");
                    save_quota.text(1, id); save_quota.text(2, m);
                    save_quota.integer(3, quota_count); save_quota.integer(4, day);
                    save_quota.integer(5, until); save_quota.row();
                }
                // A failure during an admitted auth probe extends the cooldown
                // without granting another probe; an expired cooldown starts over.
                Statement s(impl_->db,
                            "INSERT INTO failures(account,model,until,count,kind) VALUES(?,?,?,?,?) "
                            "ON CONFLICT(account,model) DO UPDATE SET until=excluded.until,"
                            "count=excluded.count,kind=excluded.kind,probed=CASE WHEN "
                            "failures.until>? THEN failures.probed ELSE 0 END");
                s.text(1, id);
                s.text(2, m);
                s.integer(3, until);
                s.integer(4, quota_error ? count : std::min(count, 1000000) + 1);
                s.text(5, kind);
                s.integer(6, current_time);
                s.row();
            }
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
void Store::success(std::string_view id, std::string_view model) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement sticky(impl_->db, "INSERT INTO sticky VALUES(?,?) ON CONFLICT(model) DO "
                                        "UPDATE SET account=excluded.account");
            sticky.text(1, model);
            sticky.text(2, id);
            sticky.row();
            Statement current(impl_->db, "INSERT INTO settings(k,v) VALUES('current',?) ON CONFLICT(k) "
                                         "DO UPDATE SET v=excluded.v");
            current.text(1, id);
            current.row();
            // A completed generation also proves an auth cooldown stale.
            Statement reset(impl_->db, "DELETE FROM failures WHERE account=? AND "
                                       "((model=? OR model='*') AND until<=? OR model='*' AND kind='auth')");
            reset.text(1, id);
            reset.text(2, model);
            reset.integer(3, unix_seconds());
            reset.row();
            Statement quota(impl_->db, "DELETE FROM quota_counters WHERE account=? AND model=? AND (count<3 OR day<>?)");
            quota.text(1, id); quota.text(2, model);
            quota.integer(3, browser_detail::pacific_quota_day(unix_seconds())); quota.row();
            Statement quota_success(impl_->db, "UPDATE quota_counters SET cooldown_until=0 WHERE account=? AND model=?");
            quota_success.text(1, id); quota_success.text(2, model); quota_success.row();
            exec(impl_->db, "COMMIT");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
namespace {
using UsageGroups = std::map<std::pair<std::string, std::string>, json::object>;
std::int64_t integer(const json::object &o, std::string_view key) {
    auto p = o.if_contains(key);
    if (!p) return 0;
    if (p->is_int64() && p->as_int64() >= 0) return p->as_int64();
    if (p->is_uint64() && p->as_uint64() <= 9007199254740991ULL) return static_cast<std::int64_t>(p->as_uint64());
    throw Error(500, "usage_invalid", "Stored usage counter is invalid");
}
json::object empty_total() {
    return {{"success", 0},          {"failure", 0},           {"priced", 0},
            {"unpriced", 0},         {"amount_nano", 0},       {"known_amount_nano", 0},
            {"partially_priced", 0}, {"usage", json::object{}}};
}
void add_counter(json::object &target, std::string_view key, std::int64_t n) {
    auto prior = integer(target, key);
    if (n < 0 || n > 9007199254740991LL - prior)
        throw Error(500, "usage_overflow", "Usage totals exceed supported precision");
    target[key] = prior + n;
}
void merge_total(json::object &target, const json::object &value) {
    for (auto key : {"success", "failure", "priced", "unpriced", "amount_nano", "known_amount_nano",
                     "partially_priced"})
        add_counter(target, key, integer(value, key));
    auto &usage = target.at("usage").as_object();
    for (auto &p : value.at("usage").as_object())
        add_counter(usage, p.key(), integer(value.at("usage").as_object(), p.key()));
}
UsageGroups request_contributions(const json::object &record) {
    UsageGroups groups;
    auto model = string_or(record, "model");
    auto &main = groups[{string_or(record, "account"), model}];
    main = empty_total();
    main[string_or(record, "status") == "succeeded" ? "success" : "failure"] = 1;
    bool priced = string_or(record.at("cost").as_object(), "status") == "priced";
    main[priced ? "priced" : "unpriced"] = 1;
    const bool known = !record.at("cost").at("known_amount_nano").is_null();
    if (!priced && known)
        main["partially_priced"] = 1;
    for (auto &a : record.at("attempts").as_array()) {
        auto &attempt = a.as_object();
        auto &group = groups[{string_or(attempt, "account"), model}];
        if (group.empty())
            group = empty_total();
        auto &usage = group.at("usage").as_object();
        for (auto &p : attempt.at("usage").as_object())
            if (p.value().is_int64() || p.value().is_uint64())
                add_counter(usage, p.key(), integer(attempt.at("usage").as_object(), p.key()));
        if (priced)
            add_counter(group, "amount_nano",
                        integer(attempt.at("cost").as_object(), "amount_nano"));
        if (known && string_or(attempt.at("cost").as_object(), "status") == "priced")
            add_counter(group, "known_amount_nano",
                        integer(attempt.at("cost").as_object(), "amount_nano"));
    }
    return groups;
}
void migrate_request_usage(sqlite3 *db) {
    exec(db, "BEGIN IMMEDIATE");
    try {
        // Older cumulative rows include history whose details may have expired.
        // Seed the known subtotal with the already complete subtotal, then apply
        // only each retained row's changed contribution; never rebuild all totals.
        Statement totals(db, "SELECT account,model,detail FROM request_totals");
        while (totals.row()) {
            auto total = json::parse(totals.str(2)).as_object();
            total["known_amount_nano"] = integer(total, "amount_nano");
            total["partially_priced"] = 0;
            Statement put(db, "UPDATE request_totals SET detail=? WHERE account=? AND model=?");
            put.text(1, json::serialize(total));
            put.text(2, totals.str(0));
            put.text(3, totals.str(1));
            put.row();
        }
        Statement rows(db, "SELECT detail,terminal FROM request_history ORDER BY seq");
        while (rows.row()) {
            auto original = json::parse(rows.str(0)).as_object();
            auto corrected = original;
            const bool terminal = sqlite3_column_int(rows.p, 1);
            summarize_request(corrected, terminal);
            if (terminal) {
                // Represent the old contract explicitly for contribution removal.
                auto before = original;
                auto old_amount = before.at("cost").at("amount_nano");
                before.at("cost").as_object()["known_amount_nano"] = std::move(old_amount);
                auto previous = request_contributions(before);
                auto next = request_contributions(corrected);
                for (auto &[key, old] : previous) {
                    Statement get(db,
                                  "SELECT detail FROM request_totals WHERE account=? AND model=?");
                    get.text(1, key.first);
                    get.text(2, key.second);
                    if (!get.row())
                        throw Error(500, "usage_migration",
                                    "Missing cumulative request contribution");
                    auto total = json::parse(get.str(0)).as_object();
                    auto subtract = [&](json::object &target, const json::object &source) {
                        for (auto &entry : source) {
                            auto value = integer(source, entry.key()),
                                 prior = integer(target, entry.key());
                            if (value > prior)
                                throw Error(500, "usage_migration",
                                            "Cumulative request contribution is inconsistent");
                            target[entry.key()] = prior - value;
                        }
                    };
                    auto counters = old;
                    counters.erase("usage");
                    subtract(total, counters);
                    subtract(total.at("usage").as_object(), old.at("usage").as_object());
                    merge_total(total, next.at(key));
                    Statement put(db,
                                  "UPDATE request_totals SET detail=? WHERE account=? AND model=?");
                    put.text(1, json::serialize(total));
                    put.text(2, key.first);
                    put.text(3, key.second);
                    put.row();
                }
            }
            Statement put(db, "UPDATE request_history SET status=?,detail=? WHERE id=?");
            put.text(1, string_or(corrected, "status"));
            put.text(2, json::serialize(corrected));
            put.text(3, string_or(corrected, "id"));
            put.row();
        }
        exec(db, "PRAGMA user_version=3;COMMIT");
    } catch (...) {
        exec(db, "ROLLBACK");
        throw;
    }
}
void save_request_row(sqlite3 *db, const json::object &record, bool terminal) {
    Statement update(db, "UPDATE request_history SET model=?,account=?,status=?,terminal=?,detail=? WHERE id=? AND terminal=0");
    update.text(1, string_or(record, "model")); update.text(2, string_or(record, "account"));
    update.text(3, string_or(record, "status")); update.integer(4, terminal);
    update.text(5, json::serialize(record)); update.text(6, string_or(record, "id")); update.row();
    if (!terminal || !sqlite3_changes(db)) return;
    for (auto &[key, value] : request_contributions(record)) {
        json::object total = empty_total();
        Statement previous(db, "SELECT detail FROM request_totals WHERE account=? AND model=?");
        previous.text(1, key.first); previous.text(2, key.second);
        if (previous.row()) total = json::parse(previous.str(0)).as_object();
        merge_total(total, value);
        Statement put(db, "INSERT INTO request_totals VALUES(?,?,?) ON CONFLICT(account,model) DO UPDATE SET detail=excluded.detail");
        put.text(1, key.first); put.text(2, key.second); put.text(3, json::serialize(total)); put.row();
    }
}
constexpr auto request_where = " WHERE (?=0 OR created_at>=?) AND (?=0 OR created_at<?)"
    " AND (?='' OR model=?) AND (?='' OR account=?) AND (?='' OR status=?) AND (?='' OR id=?)";
void validate_query(const RequestQuery &q) {
    if (q.from < 0 || q.to < 0 || (q.to && q.to <= q.from) || q.before < 0 || q.snapshot < 0 || q.limit < 1 || q.limit > 100 ||
        q.model.size() > 256 || q.account.size() > 256 || q.id.size() > 128 ||
        (!q.status.empty() && q.status != "queued" && q.status != "running" && q.status != "finishing" &&
         q.status != "succeeded" && q.status != "failed" && q.status != "cancelled" && q.status != "interrupted"))
        throw Error(400, "invalid_request_query", "Invalid request history filter");
}
void bind_query(Statement &s, const RequestQuery &q) {
    s.integer(1, q.from); s.integer(2, q.from); s.integer(3, q.to); s.integer(4, q.to);
    s.text(5, q.model); s.text(6, q.model); s.text(7, q.account); s.text(8, q.account == ":unassigned" ? "" : q.account);
    s.text(9, q.status); s.text(10, q.status); s.text(11, q.id); s.text(12, q.id);
}
json::object usage_metadata(sqlite3 *db) {
    json::object result;
    Statement s(db, "SELECT k,CAST(v AS INTEGER) FROM settings WHERE k IN ('usage_since','usage_detail_since')");
    while (s.row()) result[s.str(0) == "usage_since" ? "since" : "detail_since"] = sqlite3_column_int64(s.p, 1);
    return result;
}
}
void Store::begin_request(const json::object &record) {
    impl_->run([&] {
        Statement s(impl_->db, "INSERT INTO request_history(id,created_at,status,detail) VALUES(?,?,'queued',?)");
        s.text(1, string_or(record, "id")); s.integer(2, integer(record, "created_at"));
        s.text(3, json::serialize(record)); s.row();
    });
}
void Store::save_request(const json::object &record, bool terminal) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try { save_request_row(impl_->db, record, terminal); exec(impl_->db, "COMMIT"); }
        catch (...) { exec(impl_->db, "ROLLBACK"); throw; }
    });
}
json::object Store::request(std::string_view id) {
    return impl_->run([&] {
        Statement s(impl_->db, "SELECT detail FROM request_history WHERE id=?"); s.text(1, id);
        if (!s.row()) throw Error(404, "request_not_found", "Request record not found or expired");
        return json::parse(s.str(0)).as_object();
    });
}
json::object Store::requests(const RequestQuery &query) {
    return impl_->run([&] {
        auto result = usage_metadata(impl_->db);
        validate_query(query);
        auto snapshot = query.snapshot;
        if (!snapshot) {
            Statement last(impl_->db, "SELECT COALESCE(MAX(seq),0) FROM request_history"); last.row();
            snapshot = sqlite3_column_int64(last.p, 0);
        }
        auto sql = std::string("SELECT seq,detail FROM request_history") + request_where +
            " AND seq<=? AND (?=0 OR seq<?) ORDER BY seq DESC LIMIT ?";
        Statement s(impl_->db, sql.c_str()); bind_query(s, query); s.integer(13, snapshot);
        s.integer(14, query.before); s.integer(15, query.before); s.integer(16, query.limit + 1);
        json::array rows;
        std::int64_t last = 0;
        bool more = false;
        while (s.row()) {
            if (rows.size() == query.limit) { more = true; break; }
            last = sqlite3_column_int64(s.p, 0);
            auto row = json::parse(s.str(1)).as_object();
            row["attempt_count"] = row.at("attempts").as_array().size();
            row.erase("attempts"); row.erase("settings");
            rows.push_back(std::move(row));
        }
        auto count_sql = std::string("SELECT COUNT(*) FROM request_history") + request_where + " AND seq<=?";
        Statement count(impl_->db, count_sql.c_str()); bind_query(count, query); count.integer(13, snapshot); count.row();
        result["total"] = sqlite3_column_int64(count.p, 0); result["snapshot"] = snapshot;
        result["next_cursor"] = more ? json::value(last) : json::value(nullptr);
        result["records"] = std::move(rows);
        return result;
    });
}
json::object Store::stats(const std::optional<RequestQuery> &query) {
    return impl_->run([&] {
        auto result = usage_metadata(impl_->db);
        const bool cumulative = !query || query->cumulative;
        if (query) validate_query(*query);
        if (query && cumulative && (query->from || query->to || !query->id.empty() || !query->status.empty()))
            throw Error(400, "invalid_request_query", "Lifetime statistics only support model and account filters");
        result["scope"] = cumulative ? "lifetime" : "filtered";
        UsageGroups groups;
        if (!cumulative) {
            auto sql = std::string("SELECT detail FROM request_history") + request_where + " AND terminal=1";
            auto selection = *query;
            selection.account.clear();
            Statement s(impl_->db, sql.c_str()); bind_query(s, selection);
            while (s.row()) for (auto &[key, value] : request_contributions(json::parse(s.str(0)).as_object())) {
                if (!query->account.empty() && key.first != (query->account == ":unassigned" ? "" : query->account)) continue;
                auto &total = groups[key]; if (total.empty()) total = empty_total(); merge_total(total, value);
            }
        } else {
            Statement s(impl_->db, "SELECT account,model,detail FROM request_totals WHERE (?='' OR model=?) AND (?='' OR account=?) ORDER BY account,model");
            const auto model = query ? query->model : std::string{};
            const auto account = query ? query->account : std::string{};
            s.text(1, model); s.text(2, model); s.text(3, account); s.text(4, account == ":unassigned" ? "" : account);
            while (s.row()) groups[{s.str(0), s.str(1)}] = json::parse(s.str(2)).as_object();
        }
        json::array rows;
        auto totals = empty_total();
        for (auto &[key, value] : groups) {
            merge_total(totals, value); value["account"] = key.first; value["model"] = key.second; rows.push_back(std::move(value));
        }
        result["records"] = std::move(rows); result["totals"] = std::move(totals);
        json::array legacy;
        Statement s(impl_->db, "SELECT account,model,ok,failed,usage FROM stats "
                               "ORDER BY account,model");
        while (s.row())
            legacy.push_back(json::object{{"account", s.str(0)},
                                        {"model", s.str(1)},
                                        {"success", sqlite3_column_int64(s.p, 2)},
                                        {"failure", sqlite3_column_int64(s.p, 3)},
                                        {"usage", json::parse(s.str(4))}});
        result["legacy_records"] = std::move(legacy);
        return result;
    });
}
void Store::recover_requests() {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement s(impl_->db, "SELECT detail FROM request_history WHERE terminal=0");
            std::vector<json::object> records;
            while (s.row()) records.push_back(json::parse(s.str(0)).as_object());
            for (auto &record : records) {
                record["status"] = "interrupted"; record["error"] = "service_interrupted";
                record["ended_at"] = unix_seconds() * 1000;
                summarize_request(record, true);
                save_request_row(impl_->db, record, true);
            }
            exec(impl_->db, "COMMIT");
        } catch (...) { exec(impl_->db, "ROLLBACK"); throw; }
    });
}
void Store::prune_requests(std::int64_t before) {
    impl_->run([&] {
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement s(impl_->db, "DELETE FROM request_history WHERE seq IN (SELECT seq FROM request_history WHERE terminal=1 AND created_at<? LIMIT 1000)");
            s.integer(1, before); s.row();
            if (sqlite3_changes(impl_->db)) {
                Statement m(impl_->db, "UPDATE settings SET v=CAST(MAX(CAST(v AS INTEGER),?) AS TEXT) WHERE k='usage_detail_since'");
                m.integer(1, before); m.row();
            }
            exec(impl_->db, "COMMIT");
        } catch (...) { exec(impl_->db, "ROLLBACK"); throw; }
    });
}
json::object Store::price_sources() {
    return impl_->run([&] {
        json::object result;
        Statement s(impl_->db, "SELECT source,catalog,fetched_at,attempted_at,error FROM price_sources");
        while (s.row()) result[s.str(0)] = json::object{{"catalog", json::parse(s.str(1))},
            {"fetched_at", sqlite3_column_int64(s.p, 2)}, {"attempted_at", sqlite3_column_int64(s.p, 3)}, {"error", s.str(4)}};
        return result;
    });
}
void Store::save_price_source(std::string_view source, const json::object &catalog, std::int64_t fetched_at, std::string_view error) {
    impl_->run([&] {
        Statement s(impl_->db, "INSERT INTO price_sources VALUES(?,?,?,?,?) ON CONFLICT(source) DO UPDATE SET "
            "catalog=CASE WHEN excluded.error='' THEN excluded.catalog ELSE price_sources.catalog END,"
            "fetched_at=CASE WHEN excluded.error='' THEN excluded.fetched_at ELSE price_sources.fetched_at END,"
            "attempted_at=excluded.attempted_at,error=excluded.error");
        s.text(1, source); s.text(2, json::serialize(catalog)); s.integer(3, fetched_at);
        s.integer(4, unix_seconds()); s.text(5, error); s.row();
    });
}
json::object Store::price_quote(std::string_view model) {
    return impl_->run([&] {
        Statement s(impl_->db, "SELECT catalog FROM price_sources ORDER BY CASE source WHEN 'litellm' THEN 0 ELSE 1 END");
        while (s.row()) {
            auto catalog = json::parse(s.str(0)).as_object();
            if (auto quote = catalog.if_contains(model); quote && quote->is_object()) return quote->as_object();
        }
        return json::object{};
    });
}
unsigned default_fingerprint_seed(std::string_view account_id) {
    return 10000 + static_cast<unsigned>(std::stoul(sha256(account_id).substr(0, 8), nullptr, 16) % 90000);
}
namespace {
BrowserFingerprint read_fingerprint(sqlite3 *db, std::string_view id) {
    Statement exists(db, "SELECT 1 FROM accounts WHERE id=?");
    exists.text(1, id);
    if (!exists.row()) throw Error(404, "account_not_found", "Account not found");
    // Accounts written by an older build in this process may still lack a row.
    Statement create(db, "INSERT OR IGNORE INTO fingerprints(account,seed) VALUES(?,?)");
    create.text(1, id);
    create.integer(2, default_fingerprint_seed(id));
    create.row();
    Statement s(db, "SELECT seed,timezone,locale FROM fingerprints WHERE account=?");
    s.text(1, id);
    s.row();
    return {static_cast<unsigned>(sqlite3_column_int64(s.p, 0)), s.str(1), s.str(2)};
}
} // namespace
BrowserFingerprint Store::fingerprint(std::string_view id) {
    return impl_->run([&] { return read_fingerprint(impl_->db, id); });
}
BrowserFingerprint Store::regenerate_fingerprint(std::string_view id) {
    return impl_->run([&] {
        const auto previous = read_fingerprint(impl_->db, id);
        auto next = previous;
        // Same 10000..99999 range CloakBrowser uses for its own random seeds.
        while (next.seed == previous.seed)
            next.seed = 10000 + static_cast<unsigned>(std::stoul(random_hex(4), nullptr, 16) % 90000);
        Statement s(impl_->db, "UPDATE fingerprints SET seed=? WHERE account=?");
        s.integer(1, next.seed);
        s.text(2, id);
        s.row();
        return next;
    });
}
BrowserFingerprint Store::set_fingerprint_region(std::string_view id, std::optional<std::string> timezone,
                                                 std::optional<std::string> locale) {
    if (timezone && !timezone->empty() && !valid_timezone(*timezone))
        throw Error(400, "invalid_timezone", "Timezone must be an IANA name such as Asia/Shanghai");
    if (locale && !locale->empty() && !valid_locale(*locale))
        throw Error(400, "invalid_locale", "Locale must be a language tag such as zh-CN");
    return impl_->run([&] {
        auto result = read_fingerprint(impl_->db, id);
        if (timezone) result.timezone = *timezone;
        if (locale) result.locale = *locale;
        Statement s(impl_->db, "UPDATE fingerprints SET timezone=?,locale=? WHERE account=?");
        s.text(1, result.timezone);
        s.text(2, result.locale);
        s.text(3, id);
        s.row();
        return result;
    });
}
json::array Store::account_status() {
    return impl_->run([&] {
        json::array rows;
        Statement s(impl_->db, "SELECT id,name,email,auth_user,state,disabled,expired FROM "
                               "accounts ORDER BY rowid");
        while (s.row()) {
            auto o = summary(decode(s));
            o["expired"] = sqlite3_column_int(s.p, 6) != 0;
            json::array failures;
            Statement f(impl_->db, "SELECT model,until,count FROM failures WHERE account=?");
            f.text(1, s.str(0));
            while (f.row())
                failures.push_back(json::object{{"model", f.str(0)},
                                                {"until", sqlite3_column_int64(f.p, 1)},
                                                {"count", sqlite3_column_int(f.p, 2)}});
            o["failures"] = failures;
            Statement fp(impl_->db, "SELECT seed,timezone,locale FROM fingerprints WHERE account=?");
            fp.text(1, s.str(0));
            if (fp.row())
                o["fingerprint"] = json::object{{"seed", sqlite3_column_int64(fp.p, 0)},
                                                {"timezone", fp.str(1)}, {"locale", fp.str(2)}};
            rows.push_back(o);
        }
        return rows;
    });
}
json::object Store::create_key(std::string name) {
    auto secret = "sk-ais2api-" + random_hex(32);
    auto id = "key_" + random_hex(8);
    impl_->run([&] {
        Statement s(impl_->db, "INSERT INTO keys VALUES(?,?,?,?)");
        s.text(1, id);
        s.text(2, name);
        s.text(3, sha256(secret));
        s.integer(4, unix_seconds());
        s.row();
    });
    return {{"id", id}, {"name", name}, {"secret", secret}};
}
json::array Store::keys() {
    return impl_->run([&] {
        json::array rows;
        Statement s(impl_->db, "SELECT id,name,created FROM keys ORDER BY created,id");
        while (s.row())
            rows.push_back(json::object{
                {"id", s.str(0)}, {"name", s.str(1)}, {"created", sqlite3_column_int64(s.p, 2)}});
        return rows;
    });
}
void Store::revoke_key(std::string_view id) {
    impl_->run([&] {
        Statement s(impl_->db, "DELETE FROM keys WHERE id=?");
        s.text(1, id);
        s.row();
    });
}
std::optional<std::string> Store::authenticate(std::string_view secret) {
    return impl_->run([&]() -> std::optional<std::string> {
        Statement s(impl_->db, "SELECT id FROM keys WHERE hash=?");
        s.text(1, sha256(secret));
        if (s.row())
            return s.str(0);
        return {};
    });
}
json::object Store::create_managed_key(std::string name) {
    if (name.empty()) name = "API key";
    if (name.size() > 200) throw Error(400, "invalid_key", "Key name must contain at most 200 bytes");
    auto secret = "sk-ais2api-" + random_hex(32);
    auto id = "managed_" + random_hex(8);
    auto created = unix_seconds();
    impl_->run([&] {
        Statement s(impl_->db, "INSERT INTO managed_keys(id,name,hash,created) VALUES(?,?,?,?)");
        s.text(1, id); s.text(2, name); s.text(3, sha256(secret)); s.integer(4, created); s.row();
    });
    return {{"id", id}, {"name", name}, {"secret", secret}, {"created", created}};
}
json::array Store::managed_keys() {
    return impl_->run([&] {
        json::array out;
        Statement s(impl_->db, "SELECT id,name,created FROM managed_keys ORDER BY rowid");
        while (s.row()) out.push_back(json::object{{"id", s.str(0)}, {"name", s.str(1)}, {"created", sqlite3_column_int64(s.p, 2)}});
        return out;
    });
}
void Store::rename_managed_key(std::string_view id, std::string name) {
    if (name.empty() || name.size() > 200) throw Error(400, "invalid_key", "Key name must contain 1 to 200 bytes");
    impl_->run([&] {
        Statement s(impl_->db, "UPDATE managed_keys SET name=? WHERE id=?");
        s.text(1, name); s.text(2, id); s.row();
        if (!sqlite3_changes(impl_->db)) throw Error(404, "key_not_found", "Key not found");
    });
}
void Store::revoke_managed_key(std::string_view id) {
    impl_->run([&] {
        Statement s(impl_->db, "DELETE FROM managed_keys WHERE id=?");
        s.text(1, id); s.row();
        if (!sqlite3_changes(impl_->db)) throw Error(404, "key_not_found", "Key not found");
    });
}
std::optional<std::string> Store::authenticate_managed(std::string_view secret) {
    if (secret.empty() || secret.size() > 4096) return {};
    return impl_->run([&]() -> std::optional<std::string> {
        Statement s(impl_->db, "SELECT id FROM managed_keys WHERE hash=?");
        s.text(1, sha256(secret));
        if (s.row()) return s.str(0);
        return {};
    });
}
void Store::put_tool(std::string_view principal, std::string_view call,
                     const json::object &metadata) {
    impl_->run([&] {
        if (principal.empty() || call.empty())
            throw Error(400, "invalid_tool", "Principal and call ID required");
        exec(impl_->db, "BEGIN IMMEDIATE");
        try {
            Statement clean(impl_->db, "DELETE FROM tools WHERE created<=?");
            clean.integer(1, unix_seconds() - 86400);
            clean.row();
            Statement s(impl_->db, "INSERT INTO tools VALUES(?,?,?,?,?) ON "
                                   "CONFLICT(principal,call_id) DO UPDATE SET "
                                   "metadata=excluded.metadata,created=excluded."
                                   "created,touched=excluded.touched");
            s.text(1, principal);
            s.text(2, call);
            s.text(3, json::serialize(metadata));
            s.integer(4, unix_seconds());
            s.integer(5, next_touch(impl_->db));
            s.row();
            exec(impl_->db, "DELETE FROM tools WHERE rowid IN(SELECT rowid FROM tools ORDER BY "
                            "touched DESC,rowid DESC LIMIT -1 OFFSET 4096);COMMIT;");
        } catch (...) {
            exec(impl_->db, "ROLLBACK");
            throw;
        }
    });
}
std::optional<json::object> Store::get_tool(std::string_view principal, std::string_view call) {
    return impl_->run([&]() -> std::optional<json::object> {
        Statement s(impl_->db, "SELECT metadata FROM tools WHERE principal=? AND "
                               "call_id=? AND created>?");
        s.text(1, principal);
        s.text(2, call);
        s.integer(3, unix_seconds() - 86400);
        if (!s.row())
            return {};
        auto result = json::parse(s.str(0)).as_object();
        Statement touch(impl_->db, "UPDATE tools SET touched=? WHERE principal=? AND call_id=?");
        touch.integer(1, next_touch(impl_->db));
        touch.text(2, principal);
        touch.text(3, call);
        touch.row();
        return result;
    });
}
void Store::backup(const fs::path &target) {
    impl_->run([&] {
        if (fs::exists(target) || fs::is_symlink(fs::symlink_status(target)))
            throw Error(409, "backup_exists", "Backup target already exists");
        if (!target.parent_path().empty())
            secure_directory(target.parent_path());
        sqlite3 *dest = nullptr;
        if (sqlite3_open(sqlite_path(target).c_str(), &dest) != SQLITE_OK) {
            if (dest)
                sqlite3_close(dest);
            throw Error(500, "backup", "Cannot open backup target");
        }
        auto b = sqlite3_backup_init(dest, "main", impl_->db, "main");
        if (!b) {
            sqlite3_close(dest);
            fs::remove(target);
            throw Error(500, "backup", "Cannot initialize database backup");
        }
        auto rc = sqlite3_backup_step(b, -1);
        auto end = sqlite3_backup_finish(b);
        sqlite3_close(dest);
        if (rc != SQLITE_DONE || end != SQLITE_OK) {
            fs::remove(target);
            throw Error(500, "backup", "Database backup failed");
        }
    });
}
} // namespace ais2api
