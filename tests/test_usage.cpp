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
json::value source_data() {
    return json::parse(
        R"({"gemini/example":{"litellm_provider":"gemini","mode":"chat","input_cost_per_token":0.000001,"output_cost_per_token":0.000002,"cache_read_input_token_cost":0.0000001}})");
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
void complete(Store &store, std::string id) {
    RequestUsage usage(store, std::move(id), "chat_completions");
    start(usage);
    usage.event(event(), true);
    usage.end_attempt();
    usage.producer_done();
    usage.http_done(200, "", true);
}
} // namespace
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
        store.save_price_source("litellm", parse_price_catalog("litellm", source_data(), 1), 1);
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
    store.save_price_source("litellm", parse_price_catalog("litellm", source_data(), 1), 1);
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
    store.save_price_source("litellm", parse_price_catalog("litellm", source_data(), 1), 1);
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
    store.save_price_source("litellm", parse_price_catalog("litellm", source_data(), 1), 1);
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
    auto catalog = parse_price_catalog("litellm", source_data(), 1);
    store.save_price_source("litellm", catalog, 1);
    RequestUsage in_flight(store, "old", "gemini");
    start(in_flight);
    auto data = source_data();
    data.at("gemini/example").as_object()["input_cost_per_token"] = 0.00001;
    store.save_price_source("litellm", parse_price_catalog("litellm", data, 2), 2);
    in_flight.event(event(), true);
    in_flight.end_attempt();
    in_flight.producer_done();
    in_flight.http_done(200, "", true);
    REQUIRE(store.request("old").at("cost").at("amount_nano") == 120000);
    complete(store, "new");
    REQUIRE(store.request("new").at("cost").at("amount_nano") == 1020000);
    store.save_price_source("litellm", {}, 0, "price_download_failed");
    REQUIRE(store.price_quote("example").at("fetched_at") == 2);
    REQUIRE(store.price_sources().at("litellm").at("error") == "price_download_failed");
    REQUIRE(store.price_quote("example-other").empty());
}
TEST_CASE("Background price refresh keeps sources independently usable", "[pricing]") {
    Directory d;
    Store store(d.path);
    Config cfg;
    std::atomic<unsigned> calls{0};
    std::atomic<bool> fail{false};
    PriceService service(
        store, [&] { return cfg; }, false,
        [&](std::string_view name, const std::string &, std::stop_token token) {
            check_cancel(token);
            ++calls;
            if (fail || name == "models.dev")
                throw Error(502, "price_download_failed", "Synthetic failure");
            return json::serialize(source_data());
        });
    auto refresh = [&] {
        service.refresh();
        auto deadline = Clock::now() + 3s;
        while (bool_or(service.status(), "refreshing") && Clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        REQUIRE_FALSE(bool_or(service.status(), "refreshing"));
    };
    refresh();
    REQUIRE(calls == 2);
    REQUIRE_FALSE(store.price_quote("example").empty());
    fail = true;
    refresh();
    REQUIRE(calls == 4);
    REQUIRE_FALSE(store.price_quote("example").empty());
    REQUIRE(store.price_sources().at("litellm").at("error") == "price_download_failed");
}
TEST_CASE("Known costs survive an unknown attempt including a genuine zero",
          "[usage][usage-followup]") {
    for (bool free : {false, true}) {
        Directory d;
        Store store(d.path);
        auto data = source_data();
        if (free) {
            data.at("gemini/example").as_object()["input_cost_per_token"] = 0;
            data.at("gemini/example").as_object()["output_cost_per_token"] = 0;
        }
        store.save_price_source("litellm", parse_price_catalog("litellm", data, 1), 1);
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
        store.save_price_source("litellm", parse_price_catalog("litellm", source_data(), 1), 1);
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
            store.save_price_source("litellm", parse_price_catalog("litellm", source_data(), 1), 1);
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
        sqlite3 *db = nullptr;
        REQUIRE(sqlite3_open(path_utf8(d.path / "state.sqlite3").c_str(), &db) == SQLITE_OK);
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
                R"(UPDATE request_totals SET detail='{"success":3,"failure":0,"priced":2,"unpriced":1,"amount_nano":240000,"usage":{"promptTokenCount":300,"candidatesTokenCount":30,"totalTokenCount":330}}';INSERT INTO stats VALUES('legacy','example',2,1,'{}');PRAGMA user_version=2)",
                nullptr, nullptr, nullptr) == SQLITE_OK);
        if (rollback)
            REQUIRE(sqlite3_exec(db,
                                 "CREATE TRIGGER fail_migration BEFORE UPDATE ON request_history "
                                 "BEGIN SELECT RAISE(ABORT,'synthetic migration failure');END",
                                 nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        if (rollback) {
            REQUIRE_THROWS_AS(Store(d.path), Error);
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
            REQUIRE(sqlite3_exec(db, "DROP TRIGGER fail_migration", nullptr, nullptr, nullptr) ==
                    SQLITE_OK);
            sqlite3_close(db);
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
        Store reopened(d.path);
        REQUIRE(reopened.stats() == expected);
    }
}
TEST_CASE("Downloaded public price catalogs match the supported schema", "[.pricing-catalog]") {
    for (auto [source, variable] : {std::pair{"litellm", "AIS2API_TEST_LITELLM_CATALOG"},
                                    {"models.dev", "AIS2API_TEST_MODELSDEV_CATALOG"}}) {
        auto path = std::getenv(variable);
        if (!path)
            SKIP("Set catalog fixture paths for the explicit public catalog compatibility check");
        auto catalog = parse_price_catalog(source, json::parse(read_file(path)), unix_seconds());
        REQUIRE(catalog.contains("gemini-2.5-pro"));
        REQUIRE(catalog.at("gemini-2.5-pro").at("tiers").as_array().size() >= 2);
    }
}
