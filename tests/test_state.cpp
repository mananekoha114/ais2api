// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/store.hpp"
#include "ais2api/usage.hpp"
#include "../browser/rate_policy.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sqlite3.h>
#include <set>
#include <thread>
using namespace ais2api;
namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-test-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
json::object credential(std::string value = "private-cookie") {
    return {{"cookies", json::array{json::object{{"name", "SID"},
                                                 {"value", value},
                                                 {"domain", ".google.com"},
                                                 {"path", "/"},
                                                 {"secure", true}}}},
            {"origins", json::array{}}};
}
void record_usage(Store &store, const std::string &account, int tokens) {
    RequestUsage usage(store, "test-" + random_hex(8), "gemini");
    usage.configure("model", false); usage.start(); usage.normalized("model", {}); usage.begin_attempt(account);
    usage.event({{"usageMetadata", json::object{{"promptTokenCount", tokens}, {"candidatesTokenCount", 0},
                                                {"totalTokenCount", tokens}, {"cookie", "do-not-persist"}}},
                 {"candidates", json::array{json::object{{"finishReason", "STOP"}}}}}, false);
    usage.end_attempt(); usage.producer_done(); usage.http_done(200, "", true);
}
} // namespace
TEST_CASE("Pacific quota dates follow modern DST boundaries", "[quota-policy]") {
    using namespace std::chrono;
    using browser_detail::pacific_quota_day;
    const auto seconds_at = [](year_month_day date, int hour, int minute = 0) {
        return duration_cast<seconds>((sys_days{date} + hours{hour} + minutes{minute}).time_since_epoch()).count();
    };
    const auto march = sys_days{2026y / March / 8}.time_since_epoch().count();
    CHECK(pacific_quota_day(seconds_at(2026y / March / 8, 7, 59)) == march - 1);
    CHECK(pacific_quota_day(seconds_at(2026y / March / 8, 8)) == march);
    CHECK(pacific_quota_day(seconds_at(2026y / March / 8, 9, 59)) == march);
    CHECK(pacific_quota_day(seconds_at(2026y / March / 8, 10)) == march);
    CHECK(pacific_quota_day(seconds_at(2026y / March / 9, 7)) == march + 1);
    const auto november = sys_days{2026y / November / 1}.time_since_epoch().count();
    CHECK(pacific_quota_day(seconds_at(2026y / November / 1, 6, 59)) == november - 1);
    CHECK(pacific_quota_day(seconds_at(2026y / November / 1, 7)) == november);
    CHECK(pacific_quota_day(seconds_at(2026y / November / 1, 8, 59)) == november);
    CHECK(pacific_quota_day(seconds_at(2026y / November / 1, 9)) == november);
    CHECK(pacific_quota_day(seconds_at(2026y / November / 2, 7, 59)) == november);
    CHECK(pacific_quota_day(seconds_at(2026y / November / 2, 8)) == november + 1);
}

TEST_CASE("model quota cooldowns isolate counters and preserve drip recovery", "[quota-policy]") {
    Directory d;
    Store store(d.path);
    const auto imported = store.import_accounts(credential());
    const auto id = string_or(imported[0].as_object(), "id");
    auto verified = store.account(id);
    verified.email = "quota-test@example.com";
    store.put_account(verified);
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &database) == SQLITE_OK);
    struct Close { sqlite3* db; ~Close() { sqlite3_close(db); } } close{database};
    auto execute = [&](const char* sql) { REQUIRE(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK); };
    auto scalar = [&](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        REQUIRE(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK);
        struct Finalize { sqlite3_stmt* p; ~Finalize() { sqlite3_finalize(p); } } finalize{statement};
        REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
        return sqlite3_column_int64(statement, 0);
    };
    auto delay_is = [&](int expected) {
        auto delay = scalar("SELECT until FROM failures WHERE model='m'") - unix_seconds();
        CHECK(delay >= expected - 2); CHECK(delay <= expected);
    };
    store.failure(id, "m", "rate_limit_minute"); delay_is(60);
    store.failure(id, "m", "rate_limit_minute"); delay_is(60);
    CHECK(scalar("SELECT count FROM quota_counters WHERE model='m'") == 0);
    CHECK_FALSE(store.choose("m")); REQUIRE(store.choose("other"));
    store.failure(id, "m", "rate_limit_day"); delay_is(60);
    store.failure(id, "m", "rate_limit_model"); delay_is(60);
    store.failure(id, "m", "rate_limit_day"); delay_is(300);
    execute("UPDATE failures SET until=0");
    store.success(id, "m");
    CHECK(scalar("SELECT count FROM quota_counters WHERE model='m'") == 3);
    store.failure(id, "m", "rate_limit_model"); delay_is(300);
    store.failure(id, "m", "rate_limit_model"); delay_is(600);
    store.failure(id, "m", "rate_limit_minute", 3600); delay_is(3600);
    store.failure(id, "m", "rate_limit_minute"); delay_is(3600);
    CHECK(scalar("SELECT count FROM quota_counters WHERE model='m'") == 5);
    store.failure(id, "m", "permission_temporary");
    auto refreshed = store.merge_verified_accounts({store.account(id)});
    REQUIRE(refreshed.size() == 1);
    CHECK(string_or(refreshed[0].as_object(), "id") == id);
    CHECK(store.accounts().size() == 1);
    CHECK_FALSE(store.choose("m"));
    delay_is(3600);
    CHECK(scalar("SELECT count FROM quota_counters WHERE model='m'") == 5);
    execute("UPDATE quota_counters SET day=day-1,cooldown_until=0");
    store.failure(id, "m", "rate_limit_day"); delay_is(60);
    CHECK(scalar("SELECT count FROM quota_counters WHERE model='m'") == 1);
    execute("UPDATE failures SET until=0");
    store.success(id, "m");
    CHECK(scalar("SELECT COUNT(*) FROM quota_counters") == 0);
    store.failure(id, "m", "transient");
    store.failure(id, "m", "transient");
    store.failure(id, "m", "rate_limit_day"); delay_is(60);
    CHECK(scalar("SELECT count FROM quota_counters WHERE model='m'") == 1);
    store.failure(id, "other", "rate_limit_day");
    store.clear_cooldown(id, "m");
    CHECK(scalar("SELECT COUNT(*) FROM quota_counters WHERE model='m'") == 0);
    CHECK(scalar("SELECT COUNT(*) FROM quota_counters WHERE model='other'") == 1);
    store.reset_account(id);
    CHECK(scalar("SELECT COUNT(*) FROM quota_counters") == 0);
    store.failure(id, "m", "permission_temporary"); delay_is(300);
    refreshed = store.merge_verified_accounts({store.account(id)});
    REQUIRE(refreshed.size() == 1);
    CHECK(string_or(refreshed[0].as_object(), "id") == id);
    CHECK(store.accounts().size() == 1);
    REQUIRE(store.choose("m"));
    CHECK(store.choose("m")->id == id);
    store.clear_cooldown(id);
    store.failure(id, "m", "rate_limit_day", 100000); delay_is(86400);
    store.remove_account(id);
    CHECK(scalar("SELECT COUNT(*) FROM quota_counters") == 0);
}

TEST_CASE("existing version one stores add quota counters on reopen", "[quota-policy]") {
    Directory d;
    std::string id;
    { Store store(d.path); id = string_or(store.import_accounts(credential())[0].as_object(), "id"); }
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &database) == SQLITE_OK);
    REQUIRE(sqlite3_exec(database, "DROP TABLE quota_counters; CREATE TABLE quota_counters(account TEXT REFERENCES accounts(id) ON DELETE CASCADE,model TEXT,count INTEGER NOT NULL,day INTEGER NOT NULL,PRIMARY KEY(account,model))", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    Store reopened(d.path);
    CHECK_NOTHROW(reopened.failure(id, "m", "rate_limit_day"));
    CHECK_FALSE(reopened.choose("m"));
    CHECK(reopened.choose("other"));
}

TEST_CASE("configuration merges partial limits and rejects unsafe types") {
    Directory d;
    auto file = d.path / "config.json";
    atomic_write(file, json::serialize(json::object{
                           {"version", 1}, {"port", 1234}, {"limits", {{"queue_size", 2}}}}));
    auto config = Config::load(file, {{"port", 4321}});
    CHECK(config.port == 4321);
    CHECK(config.limits.queue_size == 2);
    CHECK(config.limits.request_bytes == 32 * 1024 * 1024);
    CHECK(config.to_json().at("version") == 1);
    CHECK_THROWS(Config::load(file, {{"port", 65536}}));
    CHECK_THROWS(Config::load(file, {{"headless", "false"}}));
    CHECK_THROWS(Config::load(file, {{"version", 2}}));
    CHECK_THROWS(Config::load(file, {{"unknown", true}}));
    CHECK_THROWS(Config::load(file, {{"limits", {{"max_attempts", 0}}}}));
    CHECK_THROWS(Config::load(file, {{"limits", {{"unknown", 1}}}}));
}
TEST_CASE("account imports are transactional and output excludes credentials") {
    Directory d;
    Store store(d.path);
    auto imported = store.import_accounts(credential());
    REQUIRE(imported.size() == 1);
    auto id = string_or(imported[0].as_object(), "id");
    CHECK(json::serialize(imported).find("private-cookie") == std::string::npos);
    CHECK(store.account(id).storage_state.at("cookies").as_array().size() == 1);
    json::object bundle{
        {"accounts",
         json::array{json::object{{"name", "good"}, {"cookies", credential().at("cookies")}},
                     json::object{{"name", "bad"}, {"cookies", json::array{}}}}}};
    CHECK_THROWS(store.import_accounts(bundle));
    CHECK(store.accounts().size() == 1);
    CHECK(json::serialize(store.account_status()).find("private-cookie") == std::string::npos);
    CHECK_THROWS(store.import_accounts(json::object{
        {"cookies",
         json::array{json::object{{"name", "SID"}, {"value", "secret"}, {"domain", "../bad"}}}}}));
    auto raw = store.import_accounts(json::string("SID=header-value; HSID=second"));
    CHECK(raw.size() == 1);
}
TEST_CASE("routing isolates account expiry and model permissions") {
    Directory d;
    Store store(d.path);
    auto x = store.import_accounts(credential());
    auto y = store.import_accounts(credential("second"));
    auto a = string_or(x[0].as_object(), "id"), b = string_or(y[0].as_object(), "id");
    store.select_account(a);
    REQUIRE(store.choose("m"));
    CHECK(store.choose("m")->id == a);
    store.success(b, "m");
    CHECK(store.choose("m")->id == b);
    store.failure(b, "m", "permission");
    CHECK(store.choose("m")->id == a);
    store.select_account(b);
    CHECK(store.choose("other")->id == b);
    store.failure(b, "other", "login_expired");
    CHECK(store.choose("other")->id == a);
    store.reset_account(b);
    CHECK(store.choose("other")->id == b);
    store.failure(b, "other", "429");
    CHECK(store.choose("another")->id == a);
    CHECK_FALSE(store.choose("m", {a, b}));
    store.remove_account(a);
    CHECK_THROWS(store.account(a));
}
TEST_CASE("hashed API keys persist and cannot be enumerated in plaintext") {
    Directory d;
    std::string secret, id;
    {
        Store store(d.path);
        auto k = store.create_key("test");
        secret = string_or(k, "secret");
        id = string_or(k, "id");
        REQUIRE(secret.size() > 60);
        CHECK(store.authenticate(secret) == id);
        CHECK_FALSE(store.authenticate("incorrect"));
        CHECK(json::serialize(store.keys()).find(secret) == std::string::npos);
    }
    {
        Store store(d.path);
        CHECK(store.authenticate(secret) == id);
        store.revoke_key(id);
        CHECK_FALSE(store.authenticate(secret));
    }
}
TEST_CASE("tool metadata has principal isolation and bounded LRU eviction") {
    Directory d;
    Store store(d.path);
    store.put_tool("a", "shared", {{"signature", "a-value"}});
    store.put_tool("b", "shared", {{"signature", "b-value"}});
    CHECK(string_or(*store.get_tool("a", "shared"), "signature") == "a-value");
    CHECK_FALSE(store.get_tool("c", "shared"));
    for (int i = 0; i < 4095; ++i)
        store.put_tool("a", std::to_string(i), {{"i", i}});
    CHECK_FALSE(store.get_tool("b", "shared"));
    // Reading a refreshed its LRU position; b is the first evicted entry.
    // Insert once more without touching a to prove its eventual eviction too.
    store.put_tool("a", "additional", {{"i", 4095}});
    CHECK_FALSE(store.get_tool("a", "shared"));
    CHECK(store.get_tool("a", "4094"));
}
TEST_CASE("SQLite backup restores account statistics and key authentication") {
    Directory d;
    auto backup = d.path / "snapshot.sqlite3";
    std::string key;
    {
        Store store(d.path / "original");
        auto a = store.import_accounts(credential());
        auto id = string_or(a[0].as_object(), "id");
        record_usage(store, id, 7);
        key = string_or(store.create_key("backup"), "secret");
        store.backup(backup);
        CHECK_THROWS(store.backup(backup));
    }
    fs::create_directories(d.path / "restored");
    fs::copy_file(backup, d.path / "restored" / "state.sqlite3");
    Store restored(d.path / "restored");
    CHECK(restored.accounts().size() == 1);
    CHECK(restored.authenticate(key));
    CHECK(restored.stats().at("records").as_array().size() == 1);
}
TEST_CASE("store serializes simultaneous callers") {
    Directory d;
    Store store(d.path);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (int j = 0; j < 8; ++j)
                store.import_accounts(credential());
        });
    for (auto &t : threads)
        t.join();
    CHECK(store.accounts().size() == 32);
}
TEST_CASE("tool TTL uses original insertion time and statistics aggregate safely") {
    Directory d;
    {
        Store store(d.path);
        store.put_tool("principal", "old", {{"signature", "private"}});
        auto imported = store.import_accounts(credential());
        auto id = string_or(imported[0].as_object(), "id");
        record_usage(store, id, 7);
        record_usage(store, id, 4);
        auto records = store.stats().at("records").as_array();
        CHECK(records[0].as_object().at("usage").as_object().at("totalTokenCount") == 11);
        CHECK(json::serialize(records).find("do-not-persist") == std::string::npos);
    }
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "UPDATE tools SET created=0", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    Store store(d.path);
    CHECK_FALSE(store.get_tool("principal", "old"));
}
TEST_CASE("Netscape cookie metadata and JSON bundle formats import") {
    Directory d;
    Store store(d.path);
    json::string netscape("# Netscape HTTP Cookie "
                          "File\n#HttpOnly_.google.com\tTRUE\t/\tTRUE\t0\tSID\tnetscape-secret\n");
    auto imported = store.import_accounts(netscape);
    auto id = string_or(imported[0].as_object(), "id");
    auto cookie = store.account(id).storage_state.at("cookies").as_array()[0].as_object();
    CHECK(cookie.at("httpOnly").as_bool());
    CHECK(cookie.at("secure").as_bool());
    auto nested = store.import_accounts(json::object{
        {"profiles",
         json::array{json::object{{"name", "profile"},
                                  {"auth_user", "2"},
                                  {"cookies", json::string(json::serialize(credential()))}}}}});
    CHECK(nested[0].as_object().at("auth_user") == "2");
}
TEST_CASE("configuration precedence is defaults then file then environment "
          "then CLI") {
    Directory d;
    auto path = d.path / "config.json";
    atomic_write(path,
                 json::serialize(json::object{{"port", 12000}, {"limits", {{"queue_size", 3}}}}));
    const char *old = std::getenv("AIS2API_PORT");
    std::optional<std::string> previous;
    if (old)
        previous = old;
#ifdef _WIN32
    _putenv_s("AIS2API_PORT", "12001");
#else
    setenv("AIS2API_PORT", "12001", 1);
#endif
    try {
        CHECK(Config::load(path).port == 12001);
        CHECK(Config::load(path, {{"port", 12002}}).port == 12002);
    } catch (...) {
#ifdef _WIN32
        _putenv_s("AIS2API_PORT", previous ? previous->c_str() : "");
#else
        if (previous)
            setenv("AIS2API_PORT", previous->c_str(), 1);
        else
            unsetenv("AIS2API_PORT");
#endif
        throw;
    }
#ifdef _WIN32
    _putenv_s("AIS2API_PORT", previous ? previous->c_str() : "");
#else
    if (previous)
        setenv("AIS2API_PORT", previous->c_str(), 1);
    else
        unsetenv("AIS2API_PORT");
#endif
}
TEST_CASE("newer database versions are refused without altering the version") {
    Directory d;
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "PRAGMA user_version=999", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    CHECK_THROWS_AS(Store(d.path), Error);
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int(statement, 0) == 999);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}
TEST_CASE("Cookie header imports trim only outer pipeline whitespace", "[state][cookie-text]") {
    Directory directory;
    Store store(directory.path);
    for (const auto &text : std::vector<std::string>{
             "SAPISID=synthetic\n", "SAPISID=synthetic\r\n", " \t\r\nSAPISID=synthetic \t\r\n",
             " \tCookie: SAPISID=synthetic\r\n",
             std::string("\xEF\xBB\xBF") + "SAPISID=synthetic\r\n"}) {
        auto imported = store.import_accounts(json::string(text));
        REQUIRE(imported.size() == 1);
        auto account = store.account(string_or(imported[0].as_object(), "id"));
        auto &cookies = account.storage_state.at("cookies").as_array();
        REQUIRE(cookies.size() == 1);
        CHECK(cookies[0].as_object().at("name") == "SAPISID");
        CHECK(cookies[0].as_object().at("value") == "synthetic");
    }
}
TEST_CASE("Cookie header imports reject embedded line breaks without dropping fragments",
          "[state][cookie-text]") {
    Directory directory;
    Store store(directory.path);
    for (const auto &text : std::vector<std::string>{
             "SAPISID=good\nbad\n", "SAPISID=good\r\nInjected=value\r\n",
             "SAPISID=good;\nInjected-Header: evil\n", "SAPISID=good\rIgnored\r\n"}) {
        try {
            store.import_accounts(json::string(text));
            FAIL("Embedded header newline was accepted");
        } catch (const Error &error) {
            CHECK(error.status == 400);
            CHECK(error.code == "invalid_credentials");
        }
    }
    CHECK(store.accounts().empty());
}
TEST_CASE("BOM JSON credentials and Netscape row delimiters remain intact",
          "[state][cookie-text]") {
    Directory directory;
    Store store(directory.path);
    auto json_import = store.import_accounts(json::string(
        std::string("\xEF\xBB\xBF") +
        json::serialize(json::object{
            {"accounts", json::array{json::object{{"name", "BOM bundle"},
                                                  {"storage_state", credential("json-value")}}}}}) +
        "\r\n"));
    REQUIRE(json_import.size() == 1);
    CHECK(store.account(string_or(json_import[0].as_object(), "id"))
              .storage_state.at("cookies")
              .as_array()[0]
              .as_object()
              .at("value") == "json-value");
    auto netscape = store.import_accounts(
        json::string("# Netscape HTTP Cookie "
                     "File\r\n.google.com\tTRUE\t/"
                     "\tTRUE\t0\tSID\tfirst-value\r\n.google.com\tTRUE\t/\tTRUE\t0\tEMPTY\t\r\n"));
    auto saved = store.account(string_or(netscape[0].as_object(), "id"))
                     .storage_state.at("cookies")
                     .as_array();
    REQUIRE(saved.size() == 2);
    CHECK(saved[0].as_object().at("value") == "first-value");
    CHECK(saved[1].as_object().at("value") == "");
}
TEST_CASE("browser cookie write-back only replaces the cookies it was built from", "[cookie-refresh]") {
    Directory dir;
    Store store(dir.path);
    Account account;
    account.id = "acc_refresh";
    account.name = "Refresh";
    account.email = "refresh@example.com";
    account.auth_user = "0";
    account.storage_state = credential("original");
    store.put_account(account);
    store.failure(account.id, "gemini-test", "rate_limit_day");
    const auto built_from = store.account(account.id).storage_state;
    const auto rotated = credential("rotated").at("cookies").as_array();

    auto saved = store.refresh_cookies(account.id, built_from, rotated);
    REQUIRE(saved);
    auto stored = store.account(account.id);
    CHECK(stored.storage_state == *saved);
    CHECK(string_or(stored.storage_state.at("cookies").as_array()[0].as_object(), "value") == "rotated");
    CHECK(stored.email == account.email);
    // Cooldowns, quota state and expiry are not credential replacement.
    CHECK_FALSE(store.choose("gemini-test"));
    auto status = store.account_status();
    CHECK_FALSE(status[0].as_object().at("expired").as_bool());

    // Unchanged cookies and a stale session baseline are both refused.
    CHECK_FALSE(store.refresh_cookies(account.id, *saved, rotated));
    CHECK_FALSE(store.refresh_cookies(account.id, built_from, credential("late").at("cookies").as_array()));
    CHECK(string_or(store.account(account.id).storage_state.at("cookies").as_array()[0].as_object(), "value") == "rotated");
    CHECK_FALSE(store.refresh_cookies("acc_missing", built_from, rotated));
}
TEST_CASE("authentication cooldowns are account-wide and temporary", "[cookie-refresh][recovery]") {
    Directory dir;
    Store store(dir.path);
    Account account;
    account.id = "acc_auth";
    account.name = "Auth";
    account.email = "auth@example.com";
    account.auth_user = "0";
    account.storage_state = credential();
    store.put_account(account);
    store.failure(account.id, "gemini-test", "auth");
    CHECK_FALSE(store.choose("gemini-test"));
    CHECK_FALSE(store.choose("other-model"));
    const auto status = store.account_status()[0].as_object();
    CHECK_FALSE(status.at("expired").as_bool());
    const auto& failure = status.at("failures").as_array()[0].as_object();
    CHECK(string_or(failure, "model") == "*");
    const auto until = failure.at("until").to_number<std::int64_t>();
    CHECK(until > unix_seconds() + 240);
    CHECK(until <= unix_seconds() + 300);
}
TEST_CASE("accounts receive a permanent browser fingerprint when first stored", "[fingerprint]") {
    Directory dir;
    Store store(dir.path);
    const auto imported = store.import_accounts(credential("first-import"));
    const auto id = string_or(imported[0].as_object(), "id");
    const auto first = store.fingerprint(id);
    // Cookie import verifies with the seed the store later assigns to the id.
    CHECK(first.seed == default_fingerprint_seed(id));
    CHECK(default_fingerprint_seed("acc_candidate") == default_fingerprint_seed("acc_candidate"));
    CHECK(first.seed >= 10000);
    CHECK(first.seed <= 99999);
    CHECK(first.timezone.empty());
    CHECK(first.locale.empty());
    // Credential updates, verified re-imports and metadata edits keep it.
    auto account = store.account(id);
    account.email = "permanent@example.com";
    store.put_account(account);
    Account reimport = account;
    reimport.id = "acc_candidate";
    reimport.storage_state = credential("second-import");
    store.merge_verified_accounts({reimport});
    store.update_account_metadata(id, {{"name", "Renamed"}});
    CHECK(store.fingerprint(id).seed == first.seed);
    const auto listed = store.account_status()[0].as_object().at("fingerprint").as_object();
    CHECK(listed.at("seed").to_number<unsigned>() == first.seed);
    CHECK(string_or(listed, "timezone").empty());
    // Distinct accounts get independent seeds (collisions are possible but rare).
    std::set<unsigned> seeds{first.seed};
    for (int i = 0; i < 8; ++i)
        seeds.insert(store.fingerprint(string_or(store.import_accounts(credential("other-" + std::to_string(i)))[0].as_object(), "id")).seed);
    CHECK(seeds.size() > 1);
}
TEST_CASE("fingerprints regenerate and accept validated region overrides", "[fingerprint]") {
    Directory dir;
    Store store(dir.path);
    const auto id = string_or(store.import_accounts(credential())[0].as_object(), "id");
    const auto original = store.fingerprint(id);
    const auto regenerated = store.regenerate_fingerprint(id);
    CHECK(regenerated.seed != original.seed);
    CHECK(store.fingerprint(id).seed == regenerated.seed);

    auto custom = store.set_fingerprint_region(id, std::string("Asia/Tokyo"), std::string("ja-JP"));
    CHECK(custom.timezone == "Asia/Tokyo");
    CHECK(custom.locale == "ja-JP");
    CHECK(custom.seed == regenerated.seed);
    // nullopt keeps a field; "" returns it to the host default.
    auto partial = store.set_fingerprint_region(id, std::nullopt, std::string());
    CHECK(partial.timezone == "Asia/Tokyo");
    CHECK(partial.locale.empty());
    CHECK_THROWS_AS(store.set_fingerprint_region(id, std::string("Asia/Tokyo --flag"), std::nullopt), Error);
    CHECK_THROWS_AS(store.set_fingerprint_region(id, std::nullopt, std::string("en_US")), Error);
    CHECK(store.fingerprint(id).timezone == "Asia/Tokyo");
    CHECK_THROWS_AS(store.fingerprint("acc_missing"), Error);
    CHECK_THROWS_AS(store.regenerate_fingerprint("acc_missing"), Error);
}
TEST_CASE("fingerprints follow account lifetime and back-fill older stores", "[fingerprint]") {
    Directory dir;
    std::string id;
    {
        Store store(dir.path);
        id = string_or(store.import_accounts(credential())[0].as_object(), "id");
        const auto removed = string_or(store.import_accounts(credential("removed"))[0].as_object(), "id");
        store.remove_account(removed);
        CHECK_THROWS_AS(store.fingerprint(removed), Error);
    }
    {
        // Simulate a store written before fingerprints existed.
        sqlite3* database = nullptr;
        REQUIRE(sqlite3_open((dir.path / "state.sqlite3").string().c_str(), &database) == SQLITE_OK);
        REQUIRE(sqlite3_exec(database, "DROP TABLE fingerprints", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(database);
    }
    Store reopened(dir.path);
    const auto status = reopened.account_status()[0].as_object();
    REQUIRE(status.contains("fingerprint"));
    const auto seed = status.at("fingerprint").as_object().at("seed").to_number<unsigned>();
    CHECK(seed >= 10000);
    CHECK(reopened.fingerprint(id).seed == seed);
}

TEST_CASE("An auth cooldown admits one probe attempt only when no account is usable", "[auth-probe]") {
    Directory d;
    Store store(d.path);
    const auto a = string_or(store.import_accounts(credential("first"))[0].as_object(), "id");
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open((d.path / "state.sqlite3").string().c_str(), &database) == SQLITE_OK);
    struct Close { sqlite3* db; ~Close() { sqlite3_close(db); } } close{database};
    auto expire_cooldowns = [&] {
        REQUIRE(sqlite3_exec(database, "UPDATE failures SET until=0", nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    store.failure(a, "m", "auth");
    // Readiness and model discovery never consume the probe.
    CHECK_FALSE(store.choose("m"));
    auto probe = store.choose("m", {}, true);
    REQUIRE(probe); CHECK(probe->id == a);
    // One probe per cooldown, whether it is still running or failed again.
    CHECK_FALSE(store.choose("m", {}, true));
    store.failure(a, "m", "auth");
    CHECK_FALSE(store.choose("m", {}, true));
    CHECK_FALSE(store.choose("other", {}, true));
    // A new cooldown after the old one ran out grants a new probe.
    expire_cooldowns();
    REQUIRE(store.choose("m"));
    store.failure(a, "m", "auth");
    REQUIRE(store.choose("m", {}, true));
    // A completed generation clears the auth cooldown before it expires.
    store.success(a, "m");
    REQUIRE(store.choose("m"));

    SECTION("quota, permission and expiry are never bypassed") {
        store.failure(a, "m", "auth");
        store.failure(a, "m", "rate_limit_minute");
        CHECK_FALSE(store.choose("m", {}, true));
        store.failure(a, "m2", "permission_temporary");
        CHECK_FALSE(store.choose("m2", {}, true));
        // The probe was not spent on the blocked models.
        REQUIRE(store.choose("other", {}, true));
        store.failure(a, "m", "login_expired");
        expire_cooldowns();
        store.failure(a, "m", "auth");
        CHECK_FALSE(store.choose("other", {}, true));
    }
    SECTION("an available account is preferred and keeps the probe unspent") {
        const auto b = string_or(store.import_accounts(credential("second"))[0].as_object(), "id");
        store.failure(a, "m", "auth");
        auto chosen = store.choose("m", {}, true);
        REQUIRE(chosen); CHECK(chosen->id == b);
        auto fallback = store.choose("m", {b}, true);
        REQUIRE(fallback); CHECK(fallback->id == a);
        CHECK_FALSE(store.choose("m", {b}, true));
    }
}

TEST_CASE("Alternating models stay on the current account until it cannot serve one", "[routing]") {
    Directory d;
    Store store(d.path);
    const auto a = string_or(store.import_accounts(credential("first"))[0].as_object(), "id");
    const auto b = string_or(store.import_accounts(credential("second"))[0].as_object(), "id");
    // Each model remembers a different account; the last success is current.
    store.success(a, "x");
    store.success(b, "y");
    for (int i = 0; i < 3; ++i) {
        CHECK(store.choose("x")->id == b);
        CHECK(store.choose("y")->id == b);
    }
    // Only a cooldown on the current account moves that model, preferring the
    // account that last served it; the new account then becomes current.
    store.failure(b, "x", "rate_limit_model");
    CHECK(store.choose("x")->id == a);
    CHECK(store.choose("y")->id == b);
    store.success(a, "x");
    CHECK(store.choose("y")->id == a);
    // An explicit selection takes effect immediately and becomes current.
    store.select_account(b);
    CHECK(store.choose("y")->id == b);
    CHECK(string_or(store.rotation_status(), "next_account_id") == b);
    CHECK(store.rotate_account() == a);
    CHECK(store.choose("y")->id == a);
    CHECK(string_or(store.rotation_status(), "next_account_id") == a);
}
