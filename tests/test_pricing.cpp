// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/pricing.hpp"
#include "ais2api/protocol.hpp"
#include "ais2api/usage.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
json::value catalog_data() {
    return json::parse(R"({"schema_version":1,"source":"https://ai.google.dev/gemini-api/docs/pricing",
        "published_at":"2026-10-31T00:00:00Z","currency":"USD","unit":"USD per 1M tokens","service_tier":"standard",
        "models":{"example":{"name":"Example","periods":[
          {"from":null,"tiers":[{"above_tokens":0,"input":1.25,"output":10,"cache_read":0.125},
                                {"above_tokens":200000,"input":2.5,"output":15,"cache_read":0.25}]},
          {"from":"2027-01-01","tiers":[{"above_tokens":0,"input":2.5,"output":20,"cache_read":null}]}]}}})");
}
json::object prices() {
    auto catalog = parse_price_catalog(catalog_data(), 123);
    return effective_quote(catalog.at("example").as_object(), "2026-12-31");
}
} // namespace
TEST_CASE("Gemini pricing separates cached input and reasoning", "[pricing]") {
    auto quote = prices();
    json::object usage{{"promptTokenCount", 1000},
                       {"cachedContentTokenCount", 400},
                       {"candidatesTokenCount", 300},
                       {"thoughtsTokenCount", 200},
                       {"totalTokenCount", 1300}};
    auto result = estimate_tokens(usage, quote);
    REQUIRE(result.at("status") == "priced");
    REQUIRE(result.at("amount_nano") == 3800000);
    REQUIRE(result.at("tokens").at("input") == 600);
    REQUIRE(result.at("tokens").at("output") == 100);
    REQUIRE(result.at("lines").as_array().size() == 4);
    usage["promptTokenCount"] = 200000;
    usage.erase("totalTokenCount");
    REQUIRE(estimate_tokens(usage, quote).at("above_tokens") == 0);
    usage["promptTokenCount"] = 200001;
    REQUIRE(estimate_tokens(usage, quote).at("above_tokens") == 200000);
}
TEST_CASE("Official catalog quotes follow announced price periods", "[pricing]") {
    auto catalog = parse_price_catalog(catalog_data(), 123);
    REQUIRE(catalog.size() == 1);
    auto &entry = catalog.at("example").as_object();
    REQUIRE(entry.at("source") == "google");
    REQUIRE(entry.at("published_at") == "2026-10-31T00:00:00Z");
    REQUIRE(price_catalog_published(catalog) == "2026-10-31T00:00:00Z");
    auto before = effective_quote(entry, "2026-12-31");
    REQUIRE(before.at("effective_from").is_null());
    REQUIRE(before.at("fetched_at") == 123);
    REQUIRE_FALSE(before.contains("periods"));
    REQUIRE(before.at("tiers").as_array().front().at("input_pico") == 1250000);
    REQUIRE(before.at("tiers").as_array().front().at("cache_pico") == 125000);
    auto after = effective_quote(entry, "2027-01-01");
    REQUIRE(after.at("effective_from") == "2027-01-01");
    REQUIRE(after.at("tiers").as_array().size() == 1);
    REQUIRE(after.at("tiers").as_array().front().at("output_pico") == 20000000);
    REQUIRE(after.at("tiers").as_array().front().at("cache_pico").is_null());
    REQUIRE(estimate_tokens({{"promptTokenCount", 200001}, {"candidatesTokenCount", 0}}, before)
                .at("amount_nano") == 500002500);
    REQUIRE(utc_date().size() == 10);
}
TEST_CASE("Malformed official catalogs are rejected", "[pricing]") {
    auto check = [](auto change) {
        auto data = catalog_data();
        change(data);
        REQUIRE_THROWS_AS(parse_price_catalog(data, 0), Error);
    };
    auto model = [](json::value &data) -> json::object & { return data.at("models").at("example").as_object(); };
    auto first = [&](json::value &data) -> json::object & {
        return model(data).at("periods").at(0).at("tiers").at(0).as_object();
    };
    check([](json::value &data) { data.as_object()["schema_version"] = 2; });
    check([](json::value &data) { data.as_object()["currency"] = "EUR"; });
    check([](json::value &data) { data.as_object().erase("published_at"); });
    check([](json::value &data) { data.as_object()["models"] = json::object{}; });
    check([&](json::value &data) { first(data)["input"] = -1; });
    check([&](json::value &data) { first(data)["input"] = 0.0000001; });
    check([&](json::value &data) { first(data).erase("output"); });
    check([&](json::value &data) { first(data)["above_tokens"] = 5; });
    check([&](json::value &data) { model(data)["periods"].at(0).as_object()["from"] = "2026-01-01"; });
    check([&](json::value &data) { model(data)["periods"].at(1).as_object()["from"] = "2027/01/01"; });
    check([&](json::value &data) {
        auto &periods = model(data)["periods"].as_array();
        periods.push_back(periods.at(1));
    });
    check([](json::value &data) {
        auto &models = data.at("models").as_object();
        models["bad id"] = models.at("example");
    });
}
TEST_CASE("Pricing consumes the protocol decoder's combined output counts", "[pricing][protocol]") {
    json::array wire{nullptr, nullptr,
                     json::array{10, 2, 15, 1, nullptr, nullptr, nullptr, nullptr, nullptr, 3}};
    auto usage = decode_chunk(wire).at("usageMetadata").as_object();
    REQUIRE(usage.at("candidatesTokenCount") == 5);
    auto result = estimate_tokens(usage, prices());
    REQUIRE(result.at("status") == "priced");
    REQUIRE(result.at("tokens").at("total") == 15);
    REQUIRE(result.at("tokens").at("output") == 2);
    REQUIRE(result.at("tokens").at("reasoning") == 3);
    REQUIRE(result.at("amount_nano") == 61375);
}
TEST_CASE("Unknown prices and usage are distinct from free tokens", "[pricing]") {
    auto quote = prices();
    REQUIRE(estimate_tokens({}, quote).at("amount_nano").is_null());
    json::object usage{{"promptTokenCount", 10}, {"candidatesTokenCount", 20}};
    REQUIRE(estimate_tokens(usage, {}).at("reason") == "missing_price");
    REQUIRE(estimate_tokens(usage, quote, true).at("reason") == "unsupported_charge_dimensions");
    usage["promptTokensDetails"] =
        json::array{json::object{{"modality", "AUDIO"}, {"tokenCount", 10}}};
    REQUIRE(estimate_tokens(usage, quote).at("amount_nano").is_null());
    usage.erase("promptTokensDetails");
    usage["cachedContentTokenCount"] = 11;
    REQUIRE(estimate_tokens(usage, quote).at("amount_nano").is_null());
    usage.erase("cachedContentTokenCount");
    auto &base = quote.at("tiers").as_array().front().as_object();
    base["input_pico"] = 0;
    base["output_pico"] = 0;
    REQUIRE(estimate_tokens(usage, quote).at("amount_nano") == 0);
    base["input_pico"] = 50;
    REQUIRE(estimate_tokens(usage, quote).at("amount_nano") == 1);
    base["cache_pico"] = nullptr;
    usage["cachedContentTokenCount"] = 1;
    REQUIRE(estimate_tokens(usage, quote).at("amount_nano").is_null());
}
TEST_CASE("Cost overflow and missing prices return unpriced with empty lines instead of throwing",
          "[pricing][usage]") {
    auto quote = prices();
    // Single-line overflow: >200k tier output is 15 USD/M (15,000,000 pico/token), so
    // 1e12 * 15,000 = 1.5e16 > exact_limit.
    json::object line_overflow_usage{{"promptTokenCount", 250000},
                                     {"candidatesTokenCount", 1000000000000LL}};
    auto line_overflow = estimate_tokens(line_overflow_usage, quote);
    CHECK(line_overflow.at("status") == "unpriced");
    CHECK(line_overflow.at("reason") == "cost_overflow");
    CHECK(line_overflow.at("amount_nano").is_null());
    CHECK(line_overflow.at("lines").as_array().empty());

    // Sum overflow across lines where each individual line fits exact_limit.
    json::object huge_quote = quote;
    auto tier = huge_quote.at("tiers").as_array().front().as_object();
    tier["input_pico"] = 500000000000LL;
    tier["output_pico"] = 500000000000LL;
    tier["cache_pico"] = 0;
    huge_quote["tiers"] = json::array{tier};
    json::object sum_overflow_usage{{"promptTokenCount", 10000000LL},
                                    {"candidatesTokenCount", 10000000LL}};
    auto sum_overflow = estimate_tokens(sum_overflow_usage, huge_quote);
    CHECK(sum_overflow.at("status") == "unpriced");
    CHECK(sum_overflow.at("reason") == "cost_overflow");
    CHECK(sum_overflow.at("amount_nano").is_null());
    CHECK(sum_overflow.at("lines").as_array().empty());

    // Missing price on a later line clears previously pushed lines.
    auto partial_quote = quote;
    partial_quote.at("tiers").as_array()[1].as_object()["cache_pico"] = nullptr;
    json::object partial_usage{{"promptTokenCount", 250000},
                               {"cachedContentTokenCount", 1000},
                               {"candidatesTokenCount", 10}};
    auto missing = estimate_tokens(partial_usage, partial_quote);
    CHECK(missing.at("status") == "unpriced");
    CHECK(missing.at("reason") == "missing_price");
    CHECK(missing.at("amount_nano").is_null());
    CHECK(missing.at("lines").as_array().empty());

    // summarize_request returns unpriced(cost_overflow) without throwing.
    auto make_attempt = [](std::int64_t amount) {
        return json::object{{"account", "acc"},
                            {"kind", "initial"},
                            {"finish_reason", "STOP"},
                            {"usage", json::object{{"promptTokenCount", 1}}},
                            {"cost", json::object{{"status", "priced"},
                                                  {"reason", ""},
                                                  {"amount_nano", amount},
                                                  {"currency", "USD"}}}};
    };
    json::object record{{"status", "running"},
                        {"generation_completed", true},
                        {"delivery_completed", true},
                        {"error", ""},
                        {"attempts", json::array{make_attempt(5000000000000000LL),
                                                 make_attempt(5000000000000000LL)}}};
    auto live = record;
    REQUIRE_NOTHROW(summarize_request(live, false));
    CHECK(live.at("cost").at("status") == "unpriced");
    CHECK(live.at("cost").at("reason") == "cost_overflow");
    CHECK(live.at("cost").at("amount_nano").is_null());
    CHECK(live.at("cost").at("known_amount_nano").is_null());

    REQUIRE_NOTHROW(summarize_request(record, true));
    CHECK(record.at("cost").at("status") == "unpriced");
    CHECK(record.at("cost").at("reason") == "cost_overflow");
    CHECK(record.at("cost").at("amount_nano").is_null());
    CHECK(record.at("cost").at("known_amount_nano").is_null());
}
