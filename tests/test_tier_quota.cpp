// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/quota_service.hpp"
#include "ais2api/tier_quota.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>
using namespace ais2api;
using namespace std::chrono_literals;

namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-quota-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
constexpr auto catalog_text = R"({"schema_version":1,"published_at":"2026-11-23T00:00:00Z","note":"test",
  "tier_fallback":{"plus":"free"},
  "models":{"m-pro":{"free":null,"pro":{"requests":50,"tokens":1000},"ultra":{"requests":150,"tokens":3000}},
            "m-all":{"free":{"requests":10,"tokens":100},"pro":{"requests":20,"tokens":200},"ultra":{"requests":30,"tokens":300}}}})";
json::value catalog_data(std::string published = "2026-11-23T00:00:00Z") {
    auto data = json::parse(catalog_text);
    data.as_object()["published_at"] = published;
    return data;
}
std::string error_code(const std::function<void()> &call) {
    try {
        call();
    } catch (const Error &error) {
        return error.code;
    }
    return {};
}
} // namespace

TEST_CASE("Tier quota catalogs validate strictly", "[quota]") {
    const auto parsed = parse_quota_catalog(catalog_data(), 7);
    CHECK(quota_catalog_published(parsed) == "2026-11-23T00:00:00Z");
    CHECK(parsed.at("fetched_at").as_int64() == 7);
    CHECK(parsed.at("models").as_object().size() == 2);
    CHECK(parsed.at("tier_fallback").at("plus") == "free");

    auto broken = [](auto edit) {
        auto data = catalog_data();
        edit(data.as_object());
        return error_code([&] { (void)parse_quota_catalog(data, 0); });
    };
    CHECK(broken([](json::object &o) { o["schema_version"] = 2; }) == "quota_schema_unsupported");
    CHECK(broken([](json::object &o) { o.erase("published_at"); }) == "invalid_quota_catalog");
    CHECK(broken([](json::object &o) { o["published_at"] = "2026-11-23"; }) == "invalid_quota_catalog");
    CHECK(broken([](json::object &o) { o["extra"] = 1; }) == "invalid_quota_catalog");
    CHECK(broken([](json::object &o) { o["models"] = json::object{}; }) == "invalid_quota_catalog");
    CHECK(broken([](json::object &o) { o["models"].as_object()["M-Upper"] = json::object{{"free", nullptr}}; }) ==
          "invalid_quota_catalog");
    CHECK(broken([](json::object &o) { o["models"].as_object()["m-all"].as_object()["gold"] = nullptr; }) ==
          "invalid_quota_catalog");
    CHECK(broken([](json::object &o) {
              o["models"].as_object()["m-all"].as_object()["free"] = json::object{{"requests", 0}, {"tokens", 1}};
          }) == "invalid_quota_catalog");
    CHECK(broken([](json::object &o) {
              o["models"].as_object()["m-all"].as_object()["free"] = json::object{{"requests", 1}};
          }) == "invalid_quota_catalog");
    CHECK(broken([](json::object &o) { o["tier_fallback"] = json::object{{"plus", "free"}, {"free", "pro"}}; }) ==
          "invalid_quota_catalog");
    CHECK(broken([](json::object &o) { o["tier_fallback"] = json::object{{"plus", "plus"}}; }) ==
          "invalid_quota_catalog");
    CHECK(error_code([] { (void)parse_quota_catalog(json::array{}, 0); }) == "invalid_quota_catalog");
}

TEST_CASE("The embedded tier quota catalog parses", "[quota]") {
    const auto parsed = parse_quota_catalog(json::parse(embedded_quota_catalog()), 0);
    const TierPolicy policy(parsed, {});
    CHECK_FALSE(policy.empty());
    CHECK(policy.excludes("gemini-4-argon", "free"));
    CHECK(policy.excludes("gemini-4-argon", "plus"));
    CHECK_FALSE(policy.excludes("gemini-4-argon", "pro"));
    CHECK(policy.excludes("gemini-4-carbon", "pro"));
    CHECK_FALSE(policy.excludes("gemini-4-carbon", "ultra"));
    const auto lite = policy.lookup("gemini-3.1-flash-lite", "plus");
    CHECK(lite.requests == 100);
    CHECK(lite.tokens == 5000000);
}

TEST_CASE("Tier policies resolve tiers, fallbacks and overrides", "[quota][tier]") {
    const auto catalog = parse_quota_catalog(catalog_data(), 0);
    const TierPolicy policy(catalog, {});
    // Unlisted models and unknown tiers are never filtered or limited.
    const auto unlisted = policy.lookup("other", "free");
    CHECK_FALSE(unlisted.listed);
    CHECK(unlisted.available);
    CHECK_FALSE(unlisted.limited());
    const auto unknown = policy.lookup("m-pro", "");
    CHECK(unknown.listed);
    CHECK(unknown.available);
    CHECK_FALSE(unknown.limited());
    CHECK_FALSE(policy.excludes("m-pro", "unknown"));
    // Null entries exclude; plus falls back to free.
    CHECK(policy.excludes("m-pro", "free"));
    CHECK(policy.excludes("M-PRO", "plus"));
    CHECK_FALSE(policy.excludes("m-pro", "pro"));
    CHECK(policy.lookup("m-all", "plus").requests == 10);
    CHECK(policy.lookup("m-all", "ultra").tokens == 300);
    // Either count reaching the limit exhausts the allowance.
    CHECK_FALSE(policy.exhausted("m-all", "free", 9, 99));
    CHECK(policy.exhausted("m-all", "free", 10, 0));
    CHECK(policy.exhausted("m-all", "free", 0, 100));
    CHECK_FALSE(policy.exhausted("m-all", "", 1000, 1000));
    CHECK(tier_rank("free") < tier_rank("plus"));
    CHECK(tier_rank("plus") < tier_rank("pro"));
    CHECK(tier_rank("pro") < tier_rank("ultra"));
    CHECK(tier_rank("ultra") < tier_rank(""));

    // A configured entry replaces the whole model; tiers it omits have no
    // known limit unless the fallback names one it sets.
    const auto overrides = parse_quota_overrides(json::object{
        {"m-pro", json::object{{"free", json::object{{"requests", 5}, {"tokens", 50}}}}},
        {"m-new", json::object{{"ultra", nullptr}}}});
    const TierPolicy merged(catalog, overrides);
    CHECK_FALSE(merged.excludes("m-pro", "free"));
    CHECK(merged.lookup("m-pro", "plus").requests == 5);
    CHECK_FALSE(merged.lookup("m-pro", "pro").limited());
    CHECK(merged.excludes("m-new", "ultra"));
    const auto effective = merged.effective();
    CHECK(effective.at("m-pro").at("origin") == "config");
    CHECK(effective.at("m-all").at("origin") == "catalog");
    CHECK(effective.at("m-all").at("tiers").at("plus").at("requests") == 10);
    CHECK(effective.at("m-new").at("tiers").at("ultra").is_null());
    CHECK(error_code([] { (void)parse_quota_overrides(json::object{{"m", json::object{{"pro", 5}}}}); }) ==
          "invalid_config");
}

TEST_CASE("Configured tier quotas are validated and hot-reloadable", "[quota][config]") {
    Directory d;
    auto cfg = Config::load(d.path / "config.json");
    CHECK(cfg.tier_quotas.empty());
    auto patched = cfg.patched({{"tier_quotas", json::object{{"gemini-x", json::object{{"free", nullptr}}}}}});
    CHECK(patched.tier_quotas.at("gemini-x").at("free").is_null());
    CHECK_FALSE(restart_required(cfg, patched));
    CHECK(error_code([&] { (void)cfg.patched({{"tier_quotas", json::object{{"Gemini-X", json::object{{"free", nullptr}}}}}}); }) ==
          "invalid_config");
    CHECK(error_code([&] { (void)cfg.patched({{"tier_quotas", json::array{}}}); }) == "invalid_config");
}

TEST_CASE("Background quota refresh keeps the newest catalog", "[quota]") {
    Directory d;
    Store store(d.path);
    Config cfg;
    std::mutex mutex;
    std::atomic<unsigned> calls{0};
    std::string published = "2099-01-01T00:00:00Z", requested, failure;
    QuotaService service(
        store, [&] { return cfg; }, false,
        [&](std::string_view url, const std::string &, std::stop_token token) {
            check_cancel(token);
            ++calls;
            std::lock_guard lock(mutex);
            requested = url;
            if (failure == "download")
                throw Error(502, "quota_download_failed", "Synthetic failure");
            auto data = catalog_data(published);
            if (failure == "schema")
                data.as_object()["schema_version"] = 2;
            return json::serialize(data);
        });
    auto refresh = [&] {
        service.refresh();
        for (auto deadline = Clock::now() + 3s; bool_or(service.status(), "refreshing") && Clock::now() < deadline;)
            std::this_thread::sleep_for(5ms);
        REQUIRE_FALSE(bool_or(service.status(), "refreshing"));
    };
    auto source = [&] { return service.status().at("source").as_object(); };
    // The embedded snapshot is installed before the first policy.
    CHECK(bool_or(source(), "embedded"));
    CHECK(service.policy(cfg)->excludes("gemini-4-argon", "free"));
    CHECK_FALSE(service.policy(cfg)->lookup("m-all", "free").listed);

    refresh();
    REQUIRE(calls == 1);
    CHECK(requested == quota_catalog_url);
    CHECK_FALSE(bool_or(source(), "embedded"));
    CHECK(string_or(source(), "published_at") == "2099-01-01T00:00:00Z");
    CHECK(service.policy(cfg)->lookup("m-all", "free").requests == 10);
    CHECK_FALSE(service.policy(cfg)->lookup("gemini-4-argon", "free").listed);

    {
        std::lock_guard lock(mutex);
        failure = "download";
    }
    refresh();
    CHECK(string_or(source(), "error") == "quota_download_failed");
    CHECK(service.policy(cfg)->lookup("m-all", "free").requests == 10);

    {
        std::lock_guard lock(mutex);
        failure = "schema";
    }
    refresh();
    CHECK(string_or(source(), "error") == "quota_schema_unsupported");
    CHECK(service.policy(cfg)->lookup("m-all", "free").listed);

    // An older published catalog does not replace the stored one.
    {
        std::lock_guard lock(mutex);
        failure.clear();
        published = "2000-01-01T00:00:00Z";
    }
    refresh();
    CHECK(string_or(source(), "error").empty());
    CHECK(string_or(source(), "published_at") == "2099-01-01T00:00:00Z");
    CHECK(source().at("fetched_at").as_int64() > 0);

    // The configuration's entries apply on the next policy.
    cfg.tier_quotas = json::object{{"m-all", json::object{{"free", nullptr}}}};
    CHECK(service.policy(cfg)->excludes("m-all", "free"));
    CHECK(service.status().at("models").at("m-all").at("origin") == "config");
}

TEST_CASE("Account quota views list the tier's limits and today's use", "[quota][tier]") {
    const TierPolicy policy(parse_quota_catalog(catalog_data(), 0), {});
    const auto raw = [](std::string model, std::int64_t requests, std::int64_t tokens, bool exhausted = false) {
        return json::object{{"model", std::move(model)}, {"requests", requests}, {"tokens", tokens}, {"exhausted", exhausted}};
    };
    json::array accounts{
        json::object{{"id", "free"}, {"benefit_tier", "free"}, {"daily_usage", json::array{raw("m-all", 4, 100), raw("other", 3, 9)}}},
        json::object{{"id", "pro"}, {"benefit_tier", "pro"}, {"daily_usage", json::array{raw("m-pro", 1, 10, true)}}},
        json::object{{"id", "plus"}, {"benefit_tier", "plus"}, {"daily_usage", json::array{}}},
        json::object{{"id", "unknown"}, {"benefit_tier", ""}, {"daily_usage", json::array{raw("m-all", 50, 5000)}}}};
    annotate_quota(accounts, policy);
    const auto view = [&](std::size_t index) { return accounts[index].at("daily_usage").as_array(); };

    // Free: m-pro is refused, m-all has limits (token limit reached), then
    // other models used today without limits.
    auto rows = view(0);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].at("model") == "m-all");
    CHECK(rows[0].at("requests").as_int64() == 4);
    CHECK(rows[0].at("request_limit").as_int64() == 10);
    CHECK(rows[0].at("token_limit").as_int64() == 100);
    CHECK(rows[0].at("exhausted") == true);
    CHECK(rows[1].at("model") == "other");
    CHECK(rows[1].at("request_limit").is_null());
    CHECK(rows[1].at("token_limit").is_null());
    CHECK(rows[1].at("exhausted") == false);

    // Pro: both models in table order; a daily 429 mark is exhausted below the limits.
    rows = view(1);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].at("model") == "m-pro");
    CHECK(rows[0].at("request_limit").as_int64() == 50);
    CHECK(rows[0].at("exhausted") == true);
    CHECK(rows[1].at("model") == "m-all");
    CHECK(rows[1].at("requests").as_int64() == 0);
    CHECK(rows[1].at("exhausted") == false);

    // Plus uses the free limits.
    rows = view(2);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("model") == "m-all");
    CHECK(rows[0].at("request_limit").as_int64() == 10);

    // An unknown tier has no limits and is never estimated as spent.
    rows = view(3);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("request_limit").is_null());
    CHECK(rows[0].at("exhausted") == false);

    CHECK(next_quota_reset(0) == 8 * 3600);
}
