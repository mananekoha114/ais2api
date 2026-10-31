// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/usage.hpp"
#include "ais2api/pricing.hpp"
#include "ais2api/price_service.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstdlib>
#include <sqlite3.h>
using namespace ais2api;
using namespace std::chrono_literals;
namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-usage-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
json::value source_data(std::string published = "2026-01-01T00:00:00Z") {
    auto data = json::parse(
        R"({"schema_version":1,"source":"https://ai.google.dev/gemini-api/docs/pricing","currency":"USD","models":{"example":{"name":"Example","periods":[{"from":null,"tiers":[{"above_tokens":0,"input":1,"output":2,"cache_read":0.1}]}]}}})");
    data.as_object()["published_at"] = published;
    return data;
}
json::object &base_tier(json::value &data) {
    return data.at("models").at("example").at("periods").at(0).at("tiers").at(0).as_object();
}
json::object event(int input = 100, int output = 10) {
    return {{"usageMetadata", json::object{{"promptTokenCount", input},
                                           {"candidatesTokenCount", output},
                                           {"totalTokenCount", input + output},
                                           {"cookie", "private-metadata"}}},
            {"candidates", json::array{json::object{{"finishReason", "STOP"}}}}};
}
void start(RequestUsage &usage) {
    usage.configure("alias", false);
    REQUIRE(usage.start());
    usage.normalized("example", {});
    usage.begin_attempt("a");
}
void sql(const fs::path &file, const std::string &text) {
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(path_utf8(file).c_str(), &db) == SQLITE_OK);
    const auto status = sqlite3_exec(db, text.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    REQUIRE(status == SQLITE_OK);
}
std::int64_t query_number(const fs::path &file, const char *text) {
    sqlite3 *db = nullptr;
    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_open(path_utf8(file).c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_prepare_v2(db, text, -1, &statement, nullptr) == SQLITE_OK);
    const auto value = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}
// Moves the closed store's usage tables into state.sqlite3 with the interim
// schema and leaves usage.sqlite3 with empty tables.
void make_interim_layout(const fs::path &directory, int version) {
    sql(directory / "state.sqlite3",
        "ATTACH DATABASE '" + path_utf8(directory / "usage.sqlite3") + "' AS u;"
        "CREATE TABLE model_catalog(account TEXT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,"
        "models TEXT NOT NULL,saved_at INTEGER NOT NULL);"
        "CREATE TABLE request_history(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT NOT NULL UNIQUE,"
        "created_at INTEGER NOT NULL,model TEXT NOT NULL DEFAULT '',account TEXT NOT NULL DEFAULT '',"
        "status TEXT NOT NULL,terminal INTEGER NOT NULL DEFAULT 0,detail TEXT NOT NULL);"
        "CREATE INDEX request_history_time ON request_history(created_at);"
        "CREATE TABLE request_totals(account TEXT NOT NULL,model TEXT NOT NULL,detail TEXT NOT NULL,"
        "PRIMARY KEY(account,model));"
        "CREATE TABLE price_sources(source TEXT PRIMARY KEY,catalog TEXT NOT NULL,fetched_at INTEGER NOT NULL,"
        "attempted_at INTEGER NOT NULL,error TEXT NOT NULL);"
        "INSERT INTO request_history SELECT * FROM u.request_history;"
        "INSERT INTO request_totals SELECT * FROM u.request_totals;"
        "INSERT INTO price_sources SELECT * FROM u.price_sources;"
        "INSERT INTO model_catalog SELECT * FROM u.model_catalog;"
        "INSERT INTO settings(k,v) SELECT k,v FROM u.meta WHERE k IN ('usage_since','usage_detail_since');"
        "DELETE FROM u.request_history;DELETE FROM u.request_totals;DELETE FROM u.price_sources;"
        "DELETE FROM u.model_catalog;UPDATE u.meta SET v=CAST(unixepoch()*1000 AS TEXT);"
        "PRAGMA main.user_version=" + std::to_string(version) + ";DETACH DATABASE u;");
}
Account test_account() {
    Account value;
    value.id = "a";
    value.email = "a@example.com";
    value.storage_state = {{"cookies", json::array{json::object{{"name", "SID"}, {"value", "synthetic"},
                                                                {"domain", ".google.com"}, {"path", "/"},
                                                                {"secure", true}}}}};
    return value;
}
void complete(Store &store, std::string id) {
    RequestUsage usage(store, std::move(id), "chat_completions");
    start(usage);
    usage.event(event(), true);
    usage.end_attempt();
    usage.producer_done();
    usage.http_done(200, "", true);
}
} // namespace
TEST_CASE("Queued request writes keep their order and report failures", "[usage]") {
    Directory d;
    Store store(d.path);
    std::atomic<int> failures{0};
    {
        RequestUsage usage(store, "queued", "gemini", [&] { ++failures; });
        start(usage);
        usage.event(event(), true);
        usage.end_attempt();
        usage.producer_done();
        usage.http_done(200, "", true);
    }
    // Reads run behind every queued write.
    CHECK(store.request("queued").at("status") == "succeeded");
    CHECK(store.stats().at("totals").at("success") == 1);
    CHECK(failures == 0);
    sql(d.path / "usage.sqlite3", "CREATE TRIGGER fail_terminal BEFORE UPDATE ON request_history WHEN NEW.terminal=1 "
                                  "BEGIN SELECT RAISE(ABORT,'synthetic history failure'); END");
    {
        RequestUsage usage(store, "unsaved", "gemini", [&] { ++failures; });
        start(usage);
        usage.event(event(), true);
        usage.end_attempt();
        usage.producer_done();
        REQUIRE_NOTHROW(usage.http_done(200, "", true));
    }
    store.flush_requests();
    CHECK(failures == 1);
    CHECK(store.request("unsaved").at("status") == "finishing");
    CHECK(store.stats().at("totals").at("success") == 1);
}
TEST_CASE("Recorded thinking levels ignore request case", "[usage]") {
    Directory d;
    Store store(d.path);
    RequestUsage usage(store, "levels", "gemini");
    usage.configure("example", false);
    REQUIRE(usage.start());
    usage.normalized("example", json::parse(R"({"generationConfig":{"thinkingConfig":{"thinkingLevel":"low"}}})").as_object());
    usage.producer_done();
    usage.http_done(200, "", true);
    REQUIRE(store.request("levels").at("settings").at("thinkingConfig").at("thinkingLevel") == "LOW");
}
TEST_CASE("Request history excludes arbitrary settings text and payloads", "[usage]") {
    Directory d;
    Store store(d.path);
    RequestUsage usage(store, "settings", "gemini");
    REQUIRE(usage.start());
    usage.normalized(
        "example",
        json::parse(
            R"({"contents":[{"text":"secret"}],"generationConfig":{"temperature":0.5,"stopSequences":["secret"],"responseSchema":{"description":"secret"},"responseMimeType":"text/plain","thinkingConfig":{"includeThoughts":true,"thinkingLevel":"HIGH","text":"secret"}}})")
            .as_object());
    auto record = store.request("settings");
    REQUIRE(record.at("settings") ==
            json::parse(R"({"temperature":0.5,"responseMimeType":"text/plain","thinkingConfig":{"includeThoughts":true,"thinkingLevel":"HIGH"}})"));
    REQUIRE(json::serialize(record).find("secret") == std::string::npos);
}
TEST_CASE("Completed attempts survive interruption before producer completion", "[usage]") {
    Directory d;
    {
        Store store(d.path);
        store.save_price_source("google", parse_price_catalog(source_data(), 1), 1);
        RequestUsage usage(store, "interrupted", "gemini");
        start(usage);
        usage.event(event(), true);
        usage.end_attempt();
    }
    Store reopened(d.path);
    reopened.recover_requests();
    auto record = reopened.request("interrupted");
    REQUIRE(record.at("status") == "interrupted");
    REQUIRE(record.at("finish_reason") == "STOP");
    REQUIRE(record.at("usage").at("totalTokenCount") == 110);
    REQUIRE(record.at("cost").at("amount_nano").is_null());
    REQUIRE(record.at("cost").at("known_amount_nano") == 120000);
    REQUIRE(record.at("attempts").as_array().front().at("cost").at("amount_nano") == 120000);
    auto totals = reopened.stats().at("totals");
    REQUIRE(totals.at("failure") == 1);
    REQUIRE(totals.at("known_amount_nano") == 120000);
    reopened.recover_requests();
    REQUIRE(reopened.stats().at("totals") == totals);
}
TEST_CASE("Request finalization joins delivery and generation exactly once", "[usage]") {
    Directory d;
    Store store(d.path);
    store.save_price_source("google", parse_price_catalog(source_data(), 1), 1);
    RequestUsage usage(store, "r1", "chat_completions");
    start(usage);
    usage.event(event(50, 5), true);
    usage.event(event(), true);
    usage.end_attempt();
    usage.producer_done();
    REQUIRE(store.request("r1").at("status") == "finishing");
    REQUIRE(store.stats().at("totals").at("success") == 0);
    REQUIRE_THROWS_AS(store.reset_stats(), Error);
    usage.http_done(200, "", true);
    usage.http_done(200, "", true);
    usage.producer_done();
    auto record = store.request("r1");
    REQUIRE(record.at("status") == "succeeded");
    REQUIRE(record.at("requested_model") == "alias");
    REQUIRE(record.at("model") == "example");
    REQUIRE(record.at("cost").at("amount_nano") == 120000);
    REQUIRE(record.at("usage").at("totalTokenCount") == 110);
    REQUIRE(json::serialize(record).find("private-metadata") == std::string::npos);
    store.save_request(record, true);
    auto totals = store.stats().at("totals");
    REQUIRE(totals.at("success") == 1);
    REQUIRE(totals.at("amount_nano") == 120000);
    REQUIRE(totals.at("usage").at("totalTokenCount") == 110);
}
TEST_CASE("Disconnect retains known usage and queued rejection is a known zero", "[usage]") {
    Directory d;
    Store store(d.path);
    store.save_price_source("google", parse_price_catalog(source_data(), 1), 1);
    RequestUsage usage(store, "disconnected", "gemini");
    start(usage);
    usage.http_done(499, "client_disconnected", false);
    REQUIRE(store.stats().at("totals").at("failure") == 0);
    usage.event(event(), true);
    usage.end_attempt("cancelled");
    usage.producer_done("cancelled");
    REQUIRE(store.request("disconnected").at("status") == "cancelled");
    REQUIRE(store.request("disconnected").at("cost").at("amount_nano") == 120000);
    RequestUsage queued(store, "rejected", "responses");
    queued.http_done(429, "queue_full", true);
    REQUIRE_FALSE(queued.start());
    REQUIRE(store.request("rejected").at("status") == "failed");
    REQUIRE(store.request("rejected").at("cost").at("amount_nano") == 0);
}
TEST_CASE("Retries remain one request and unknown attempts do not become free", "[usage]") {
    Directory d;
    Store store(d.path);
    store.save_price_source("google", parse_price_catalog(source_data(), 1), 1);
    RequestUsage usage(store, "retry", "chat_completions");
    start(usage);
    usage.event(event(20, 0), false);
    usage.end_attempt("upstream_error");
    usage.begin_attempt("b");
    usage.event(event(), true);
    usage.end_attempt();
    usage.http_done(200, "", true);
    usage.producer_done();
    REQUIRE(store.stats().at("totals").at("success") == 1);
    REQUIRE(store.stats().at("totals").at("amount_nano") == 140000);
    auto groups = store.stats().at("records").as_array();
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].at("success") == 0);
    REQUIRE(groups[0].at("amount_nano") == 20000);
    REQUIRE(groups[1].at("success") == 1);
    RequestQuery by_account;
    by_account.account = "a";
    auto filtered = store.stats(by_account).at("totals");
    REQUIRE(filtered.at("success") == 0);
    REQUIRE(filtered.at("amount_nano") == 20000);
    by_account.cumulative = true;
    REQUIRE(store.stats(by_account).at("totals") == filtered);
    RequestUsage unknown(store, "unknown", "gemini");
    start(unknown);
    unknown.end_attempt("upstream_error");
    unknown.begin_attempt("b");
    unknown.event(event(), true);
    unknown.end_attempt();
    unknown.producer_done();
    unknown.http_done(200, "", true);
    REQUIRE(store.request("unknown").at("cost").at("amount_nano").is_null());
    REQUIRE(store.stats().at("totals").at("unpriced") == 1);
    REQUIRE(store.stats().at("totals").at("amount_nano") == 140000);
}
TEST_CASE("History cursors filters retention and reset share one store", "[usage]") {
    Directory d;
    Store store(d.path);
    complete(store, "first");
    complete(store, "second");
    RequestQuery q;
    q.limit = 1;
    auto page = store.requests(q);
    REQUIRE(page.at("records").as_array()[0].at("id") == "second");
    REQUIRE(page.at("total") == 2);
    complete(store, "third");
    q.before = page.at("next_cursor").as_int64();
    q.snapshot = page.at("snapshot").as_int64();
    auto next = store.requests(q);
    REQUIRE(next.at("records").as_array()[0].at("id") == "first");
    REQUIRE(next.at("total") == 2);
    RequestQuery filter;
    filter.id = "second";
    REQUIRE(store.stats(filter).at("totals").at("success") == 1);
    auto created = store.request("second").at("created_at").as_int64();
    filter.from = created;
    filter.to = created + 1;
    REQUIRE(store.requests(filter).at("total") == 1);
    filter.from = created + 1;
    filter.to = created + 2;
    REQUIRE(store.requests(filter).at("total") == 0);
    RequestUsage active(store, "active", "gemini");
    store.prune_requests((unix_seconds() + 1) * 1000);
    REQUIRE(store.requests({}).at("total") == 1);
    REQUIRE(store.stats().at("totals").at("success") == 3);
    RequestQuery lifetime;
    lifetime.cumulative = true;
    lifetime.model = "example";
    lifetime.account = "a";
    REQUIRE(store.stats(lifetime).at("totals").at("success") == 3);
    lifetime.cumulative = false;
    REQUIRE(store.stats(lifetime).at("totals").at("success") == 0);
    store.recover_requests();
    REQUIRE(store.request("active").at("status") == "interrupted");
    REQUIRE(store.stats().at("totals").at("failure") == 1);
    store.recover_requests();
    REQUIRE(store.stats().at("totals").at("failure") == 1);
    store.reset_stats();
    REQUIRE(store.requests({}).at("total") == 0);
    REQUIRE(store.stats().at("totals").at("success") == 0);
    filter.to = filter.from;
    REQUIRE_THROWS_AS(store.requests(filter), Error);
}
TEST_CASE("Legacy counters remain explicitly separate through migration", "[usage]") {
    Directory d;
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(path_utf8(d.path / "state.sqlite3").c_str(), &db) == SQLITE_OK);
    REQUIRE(
        sqlite3_exec(db,
                     "CREATE TABLE stats(account TEXT,model TEXT,ok INTEGER,failed INTEGER,usage "
                     "TEXT,PRIMARY KEY(account,model));INSERT INTO stats "
                     "VALUES('old','example',2,3,'{\"totalTokenCount\":50}');PRAGMA user_version=1",
                     nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    Store store(d.path);
    REQUIRE(store.stats().at("legacy_records").as_array()[0].at("failure") == 3);
    REQUIRE(store.stats().at("records").as_array().empty());
    complete(store, "new");
    REQUIRE(store.stats().at("totals").at("success") == 1);
    REQUIRE(store.stats().at("legacy_records").as_array()[0].at("success") == 2);
    store.reset_stats();
    REQUIRE(store.stats().at("legacy_records").as_array().empty());
}
TEST_CASE("Price snapshots survive catalog updates and failed refreshes", "[usage][pricing]") {
    Directory d;
    Store store(d.path);
    auto catalog = parse_price_catalog(source_data(), 1);
    store.save_price_source("google", catalog, 1);
    RequestUsage in_flight(store, "old", "gemini");
    start(in_flight);
    auto data = source_data();
    base_tier(data)["input"] = 10;
    store.save_price_source("google", parse_price_catalog(data, 2), 2);
    in_flight.event(event(), true);
    in_flight.end_attempt();
    in_flight.producer_done();
    in_flight.http_done(200, "", true);
    REQUIRE(store.request("old").at("cost").at("amount_nano") == 120000);
    complete(store, "new");
    REQUIRE(store.request("new").at("cost").at("amount_nano") == 1020000);
    store.save_price_source("google", {}, 0, "price_download_failed");
    REQUIRE(store.price_quote("example").at("fetched_at") == 2);
    REQUIRE(store.price_sources().at("google").at("error") == "price_download_failed");
    REQUIRE(store.price_quote("example-other").empty());
}
TEST_CASE("Background price refresh keeps the newest catalog usable", "[pricing]") {
    Directory d;
    Store store(d.path);
    Config cfg;
    std::atomic<unsigned> calls{0};
    std::atomic<bool> fail{false};
    std::string published = "2099-01-01T00:00:00Z";
    std::mutex published_mutex;
    std::string requested;
    PriceService service(
        store, [&] { return cfg; }, false,
        [&](std::string_view url, const std::string &, std::stop_token token) {
            check_cancel(token);
            ++calls;
            std::lock_guard lock(published_mutex);
            requested = url;
            if (fail)
                throw Error(502, "price_download_failed", "Synthetic failure");
            return json::serialize(source_data(published));
        });
    auto refresh = [&] {
        service.refresh();
        auto deadline = Clock::now() + 3s;
        while (bool_or(service.status(), "refreshing") && Clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        REQUIRE_FALSE(bool_or(service.status(), "refreshing"));
    };
    // The embedded snapshot is installed before any download.
    for (auto deadline = Clock::now() + 3s; store.price_sources().empty() && Clock::now() < deadline;)
        std::this_thread::sleep_for(5ms);
    auto status = service.status().at("sources").at(0).as_object();
    CHECK(bool_or(status, "embedded"));
    CHECK(status.at("models").to_number<std::int64_t>() > 0);
    CHECK_FALSE(store.price_quote("gemini-2.5-pro").empty());
    refresh();
    REQUIRE(calls == 1);
    CHECK(requested == price_catalog_url);
    CHECK_FALSE(store.price_quote("example").empty());
    CHECK(store.price_quote("gemini-2.5-pro").empty());
    status = service.status().at("sources").at(0).as_object();
    CHECK_FALSE(bool_or(status, "embedded"));
    CHECK(string_or(status, "published_at") == "2099-01-01T00:00:00Z");
    fail = true;
    refresh();
    REQUIRE(calls == 2);
    CHECK_FALSE(store.price_quote("example").empty());
    CHECK(store.price_sources().at("google").at("error") == "price_download_failed");
    // An older published catalog does not replace newer stored prices.
    fail = false;
    {
        std::lock_guard lock(published_mutex);
        published = "2000-01-01T00:00:00Z";
    }
    refresh();
    REQUIRE(calls == 3);
    CHECK(store.price_quote("example").at("published_at") == "2099-01-01T00:00:00Z");
    CHECK(store.price_sources().at("google").at("error") == "");
    CHECK(store.price_sources().at("google").at("fetched_at").as_int64() > 0);
}
TEST_CASE("Known costs survive an unknown attempt including a genuine zero",
          "[usage][usage-followup]") {
    for (bool free : {false, true}) {
        Directory d;
        Store store(d.path);
        auto data = source_data();
        if (free) {
            base_tier(data)["input"] = 0;
            base_tier(data)["output"] = 0;
        }
        store.save_price_source("google", parse_price_catalog(data, 1), 1);
        RequestUsage usage(store, "partial", "gemini");
        start(usage);
        usage.end_attempt("permission_denied");
        usage.begin_attempt("b");
        usage.event(event(), true);
        usage.end_attempt();
        usage.producer_done();
        usage.http_done(200, "", true);
        auto record = store.request("partial");
        REQUIRE(record.at("cost").at("amount_nano").is_null());
        REQUIRE(record.at("cost").at("known_amount_nano") == (free ? 0 : 120000));
        REQUIRE(record.at("cost").at("unpriced_attempts") == 1);
        auto totals = store.stats().at("totals");
        REQUIRE(totals.at("amount_nano") == 0);
        REQUIRE(totals.at("known_amount_nano") == (free ? 0 : 120000));
        REQUIRE(totals.at("partially_priced") == 1);
        RequestQuery filter;
        filter.account = "b";
        REQUIRE(store.stats(filter).at("totals").at("known_amount_nano") ==
                (free ? 0 : 120000));
    }
}
TEST_CASE("Content filtering fails the request while preserving priced terminal usage",
          "[usage][usage-followup]") {
    for (auto reason : {"PROHIBITED_CONTENT", "SAFETY", "MAX_TOKENS"}) {
        Directory d;
        Store store(d.path);
        store.save_price_source("google", parse_price_catalog(source_data(), 1), 1);
        RequestUsage usage(store, "filtered", "chat_completions");
        start(usage);
        auto final = event();
        final.at("candidates").as_array()[0].as_object()["finishReason"] = reason;
        usage.event(final, true);
        usage.end_attempt();
        usage.producer_done();
        usage.http_done(200, "", true);
        auto record = store.request("filtered");
        bool blocked = std::string_view(reason) != "MAX_TOKENS";
        REQUIRE(record.at("status") == (blocked ? "failed" : "succeeded"));
        REQUIRE(record.at("finish_reason") == reason);
        REQUIRE(record.at("generation_completed") == !blocked);
        REQUIRE(record.at("delivery_completed") == true);
        REQUIRE(record.at("cost").at("amount_nano") == 120000);
        REQUIRE(store.stats().at("totals").at("failure") == (blocked ? 1 : 0));
    }
}
TEST_CASE("Version three corrects retained contributions and preserves expired totals atomically",
          "[usage][usage-followup]") {
    for (bool rollback : {false, true}) {
        Directory d;
        {
            Store store(d.path);
            store.save_price_source("google", parse_price_catalog(source_data(), 1), 1);
            complete(store, "expired");
            store.prune_requests((unix_seconds() + 1) * 1000);
            RequestUsage partial(store, "partial", "gemini");
            start(partial);
            partial.end_attempt("permission_denied");
            partial.begin_attempt("a");
            partial.event(event(), true);
            partial.end_attempt();
            partial.producer_done();
            partial.http_done(200, "", true);
            RequestUsage blocked(store, "blocked", "gemini");
            start(blocked);
            auto last = event();
            last.at("candidates").as_array()[0].as_object()["finishReason"] = "PROHIBITED_CONTENT";
            blocked.event(last, true);
            blocked.end_attempt();
            blocked.producer_done();
            blocked.http_done(200, "", true);
        }
        // Rewrite the records and totals in the version 2 format, then move
        // them into state.sqlite3 as that release stored them.
        sqlite3 *db = nullptr;
        REQUIRE(sqlite3_open(path_utf8(d.path / "usage.sqlite3").c_str(), &db) == SQLITE_OK);
        std::vector<json::object> old_records;
        sqlite3_stmt *read = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, "SELECT detail FROM request_history", -1, &read, nullptr) ==
                SQLITE_OK);
        while (sqlite3_step(read) == SQLITE_ROW)
            old_records.push_back(
                json::parse(reinterpret_cast<const char *>(sqlite3_column_text(read, 0)))
                    .as_object());
        sqlite3_finalize(read);
        for (auto &record : old_records) {
            record.erase("finish_reason");
            record["status"] = "succeeded";
            record["error"] = "";
            record["generation_completed"] = true;
            record.at("cost").as_object().erase("known_amount_nano");
            record.at("cost").as_object().erase("unpriced_attempts");
            sqlite3_stmt *put = nullptr;
            REQUIRE(sqlite3_prepare_v2(
                        db, "UPDATE request_history SET status='succeeded',detail=? WHERE id=?", -1,
                        &put, nullptr) == SQLITE_OK);
            auto data = json::serialize(record);
            auto id = string_or(record, "id");
            sqlite3_bind_text(put, 1, data.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(put, 2, id.c_str(), -1, SQLITE_TRANSIENT);
            REQUIRE(sqlite3_step(put) == SQLITE_DONE);
            sqlite3_finalize(put);
        }
        REQUIRE(
            sqlite3_exec(
                db,
                R"(UPDATE request_totals SET detail='{"success":3,"failure":0,"priced":2,"unpriced":1,"amount_nano":240000,"usage":{"promptTokenCount":300,"candidatesTokenCount":30,"totalTokenCount":330}}')",
                nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        make_interim_layout(d.path, 2);
        sql(d.path / "state.sqlite3", "INSERT INTO stats VALUES('legacy','example',2,1,'{}')");
        // The correction runs in usage.sqlite3 after the copy; a failure there
        // must leave state.sqlite3 untouched so the next start can finish.
        if (rollback)
            sql(d.path / "usage.sqlite3", "CREATE TRIGGER fail_migration BEFORE UPDATE ON request_history "
                                          "BEGIN SELECT RAISE(ABORT,'synthetic migration failure');END");
        if (rollback) {
            REQUIRE_THROWS_AS(Store(d.path), Error);
            REQUIRE(query_number(d.path / "usage.sqlite3", "SELECT COUNT(*) FROM meta WHERE k='legacy_correction'") == 1);
            REQUIRE(sqlite3_open(path_utf8(d.path / "state.sqlite3").c_str(), &db) == SQLITE_OK);
            REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &read, nullptr) == SQLITE_OK);
            REQUIRE(sqlite3_step(read) == SQLITE_ROW);
            REQUIRE(sqlite3_column_int(read, 0) == 2);
            sqlite3_finalize(read);
            REQUIRE(sqlite3_prepare_v2(db, "SELECT detail FROM request_totals", -1, &read,
                                       nullptr) == SQLITE_OK);
            REQUIRE(sqlite3_step(read) == SQLITE_ROW);
            auto total = json::parse(reinterpret_cast<const char *>(sqlite3_column_text(read, 0)))
                             .as_object();
            REQUIRE_FALSE(total.contains("known_amount_nano"));
            REQUIRE(total.at("success") == 3);
            sqlite3_finalize(read);
            sqlite3_close(db);
            sql(d.path / "usage.sqlite3", "DROP TRIGGER fail_migration");
        }
        json::object expected;
        {
            Store migrated(d.path);
            expected = migrated.stats();
            REQUIRE(migrated.requests({}).at("total") == 2);
            REQUIRE(migrated.request("blocked").at("status") == "failed");
            REQUIRE(migrated.request("partial").at("cost").at("known_amount_nano") == 120000);
            REQUIRE(expected.at("totals").at("success") == 2);
            REQUIRE(expected.at("totals").at("failure") == 1);
            REQUIRE(expected.at("totals").at("amount_nano") == 240000);
            REQUIRE(expected.at("totals").at("known_amount_nano") == 360000);
            REQUIRE(expected.at("totals").at("partially_priced") == 1);
            REQUIRE(expected.at("totals").at("usage").at("totalTokenCount") == 330);
            REQUIRE(expected.at("legacy_records").as_array()[0].at("success") == 2);
        }
        REQUIRE(query_number(d.path / "state.sqlite3", "PRAGMA user_version") == 1);
        REQUIRE(query_number(d.path / "state.sqlite3",
                             "SELECT COUNT(*) FROM sqlite_master WHERE name='request_history'") == 0);
        REQUIRE(query_number(d.path / "usage.sqlite3", "SELECT COUNT(*) FROM meta WHERE k LIKE 'legacy%'") == 0);
        Store reopened(d.path);
        REQUIRE(reopened.stats() == expected);
    }
}
TEST_CASE("Interim state databases move usage data out and return to version 1", "[usage][usage-database]") {
    Directory d;
    std::int64_t since = 0;
    {
        Store store(d.path);
        store.put_account(test_account());
        store.save_price_source("google", parse_price_catalog(source_data(), 1), 1);
        store.save_models("a", json::array{json::object{{"name", "models/gemini-kept"}}});
        complete(store, "kept");
        since = store.requests({}).at("since").to_number<std::int64_t>();
    }
    make_interim_layout(d.path, 3);
    sql(d.path / "state.sqlite3",
        "PRAGMA foreign_keys=OFF;INSERT INTO model_catalog VALUES('ghost','[]',1);"
        "UPDATE settings SET v='1000' WHERE k='usage_since'");
    {
        Store migrated(d.path);
        REQUIRE(migrated.request("kept").at("status") == "succeeded");
        REQUIRE(migrated.stats().at("totals").at("success") == 1);
        REQUIRE(migrated.stats().at("since") == 1000);
        REQUIRE(migrated.price_quote("example").contains("tiers"));
        REQUIRE(string_or(migrated.model_catalog().models[0].as_object(), "id") == "gemini-kept");
        complete(migrated, "after");
        REQUIRE(migrated.stats().at("totals").at("success") == 2);
    }
    REQUIRE(since > 1000);
    REQUIRE(query_number(d.path / "state.sqlite3", "PRAGMA user_version") == 1);
    for (auto table : {"request_history", "request_totals", "price_sources", "model_catalog"})
        REQUIRE(query_number(d.path / "state.sqlite3",
                             ("SELECT COUNT(*) FROM sqlite_master WHERE name='" + std::string(table) + "'").c_str()) == 0);
    REQUIRE(query_number(d.path / "state.sqlite3", "SELECT COUNT(*) FROM settings WHERE k LIKE 'usage%'") == 0);
    REQUIRE(query_number(d.path / "usage.sqlite3", "SELECT COUNT(*) FROM model_catalog WHERE account='ghost'") == 0);
    REQUIRE(query_number(d.path / "usage.sqlite3", "SELECT COUNT(*) FROM request_history") == 2);
}
TEST_CASE("Usage data stays out of the state database and backups include both", "[usage][usage-database]") {
    Directory d;
    {
        Store store(d.path);
        store.put_account(test_account());
        store.save_models("a", json::array{json::object{{"name", "models/gemini-kept"}}});
        complete(store, "kept");
        const auto target = d.path / "backup" / "snapshot.sqlite3";
        store.backup(target);
        REQUIRE(fs::exists(usage_backup_path(target)));
        REQUIRE(usage_backup_path(target).filename() == "snapshot-usage.sqlite3");
        REQUIRE_THROWS_AS(store.backup(target), Error);
        store.remove_account("a");
        REQUIRE(query_number(d.path / "usage.sqlite3", "SELECT COUNT(*) FROM model_catalog") == 0);
    }
    REQUIRE(query_number(d.path / "state.sqlite3", "PRAGMA user_version") == 1);
    REQUIRE(query_number(d.path / "state.sqlite3",
                         "SELECT COUNT(*) FROM sqlite_master WHERE name IN "
                         "('request_history','request_totals','price_sources','model_catalog')") == 0);
    REQUIRE(query_number(d.path / "usage.sqlite3", "PRAGMA user_version") == 1);
    fs::create_directories(d.path / "restored");
    fs::copy_file(d.path / "backup" / "snapshot.sqlite3", d.path / "restored" / "state.sqlite3");
    fs::copy_file(d.path / "backup" / "snapshot-usage.sqlite3", d.path / "restored" / "usage.sqlite3");
    Store restored(d.path / "restored");
    REQUIRE(restored.request("kept").at("status") == "succeeded");
    REQUIRE(string_or(restored.model_catalog().models[0].as_object(), "id") == "gemini-kept");
}
TEST_CASE("The embedded official price snapshot matches the supported schema", "[pricing]") {
    auto catalog = parse_price_catalog(json::parse(embedded_price_catalog()), 0);
    REQUIRE(catalog.contains("gemini-2.5-pro"));
    auto quote = effective_quote(catalog.at("gemini-2.5-pro").as_object(), utc_date());
    REQUIRE(quote.at("tiers").as_array().size() >= 2);
    REQUIRE_FALSE(price_catalog_published(catalog).empty());
}
