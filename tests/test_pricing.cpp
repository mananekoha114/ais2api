// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/pricing.hpp"
#include "ais2api/protocol.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
json::object prices() {
    return parse_price_catalog(
        "litellm",
        json::parse(
            R"({"gemini/example":{"litellm_provider":"gemini","mode":"chat","input_cost_per_token":0.00000125,"output_cost_per_token":0.00001,"cache_read_input_token_cost":0.000000125,"input_cost_per_token_above_200k_tokens":0.0000025,"output_cost_per_token_above_200k_tokens":0.000015,"cache_read_input_token_cost_above_200k_tokens":0.00000025},"vertex_ai/example":{"litellm_provider":"vertex_ai","mode":"chat","input_cost_per_token":1,"output_cost_per_token":1}})"),
        123);
}
} // namespace
TEST_CASE("Gemini pricing separates cached input and reasoning", "[pricing]") {
    auto catalog = prices();
    REQUIRE(catalog.size() == 1);
    auto quote = catalog.at("example").as_object();
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
TEST_CASE("Pricing consumes the protocol decoder's combined output counts", "[pricing][protocol]") {
    json::array wire{nullptr, nullptr,
                     json::array{10, 2, 15, 1, nullptr, nullptr, nullptr, nullptr, nullptr, 3}};
    auto usage = decode_chunk(wire).at("usageMetadata").as_object();
    REQUIRE(usage.at("candidatesTokenCount") == 5);
    auto result = estimate_tokens(usage, prices().at("example").as_object());
    REQUIRE(result.at("status") == "priced");
    REQUIRE(result.at("tokens").at("total") == 15);
    REQUIRE(result.at("tokens").at("output") == 2);
    REQUIRE(result.at("tokens").at("reasoning") == 3);
    REQUIRE(result.at("amount_nano") == 61375);
}
TEST_CASE("Unknown prices and usage are distinct from free tokens", "[pricing]") {
    auto quote = prices().at("example").as_object();
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
TEST_CASE("models.dev Google catalog uses per-million rates and context tiers", "[pricing]") {
    auto data = json::parse(
        R"({"google":{"models":{"example":{"cost":{"input":1.25,"output":10,"cache_read":0.125,"tiers":[{"input":2.5,"output":15,"cache_read":0.25,"tier":{"type":"context","size":200000}}]}}}}})");
    auto catalog = parse_price_catalog("models.dev", data, 456);
    auto &quote = catalog.at("example").as_object();
    REQUIRE(quote.at("tiers").as_array().front().at("input_pico") == 1250000);
    REQUIRE(estimate_tokens({{"promptTokenCount", 200001}, {"candidatesTokenCount", 0}}, quote)
                .at("amount_nano") == 500002500);
    REQUIRE_THROWS_AS(parse_price_catalog("models.dev", json::object{}, 0), Error);
    data.at("google").at("models").at("example").at("cost").as_object()["input"] = -1;
    REQUIRE_THROWS_AS(parse_price_catalog("models.dev", data, 0), Error);
}
