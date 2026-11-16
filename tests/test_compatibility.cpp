// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
namespace {
json::object read_request(std::string_view text) { return json::parse(text).as_object(); }
json::object chat_request() { return read_request(R"({"model":"gemini-test","messages":[{"role":"user","content":"hello"}]})"); }
}

TEST_CASE("Unsupported Chat controls require opt-in and never reach the upstream", "[protocol][compat]") {
    Config config;
    auto base = chat_request();
    for (auto extra : {R"({"n":2})", R"({"store":true})", R"({"parallel_tool_calls":false})",
                       R"({"tool_choice":"required"})", R"({"tool_choice":{"type":"function","function":{"name":"lookup"}}})",
                       R"({"seed":42})", R"({"logit_bias":{"42":-1}})", R"({"service_tier":"priority"})",
                       R"({"reasoning_effort":"none"})", R"({"reasoning_effort":"xhigh"})"}) {
        auto request = base;
        for (const auto &entry : read_request(extra)) request[entry.key()] = entry.value();
        config.ignore_unsupported_parameters = false;
        REQUIRE_THROWS_AS(normalize_openai(request, config), Error);
        config.ignore_unsupported_parameters = true;
        auto accepted = normalize_openai(request, config);
        REQUIRE(accepted.ignored_parameters.size() == 1);
        REQUIRE(accepted.gemini.at("contents").as_array()[0].as_object().at("parts").as_array()[0].as_object().at("text") == "hello");
        REQUIRE_NOTHROW(encode_request(accepted, {}, "snapshot"));
    }
}

TEST_CASE("Strict schemas and unsupported root forms degrade only when opted in", "[protocol][compat]") {
    Config config;
    auto request = read_request(R"({"model":"gemini-test","messages":[{"role":"user","content":"hello"}],"response_format":{"type":"json_schema","json_schema":{"name":"reply","strict":true,"schema":{"type":"object","properties":{"n":{"type":"integer","exclusiveMinimum":0}}}}},"tools":[{"type":"function","function":{"name":"lookup","strict":true,"parameters":{"$ref":"https://example.com/schema"}}}]})");
    REQUIRE_THROWS_AS(normalize_openai(request, config), Error);
    config.ignore_unsupported_parameters = true;
    auto normalized = normalize_openai(request, config);
    auto &function = normalized.gemini.at("tools").as_array()[0].as_object().at("functionDeclarations").as_array()[0].as_object();
    REQUIRE(function.at("name") == "lookup");
    REQUIRE_FALSE(function.contains("strict"));
    REQUIRE_FALSE(function.contains("parameters"));
    // An integer exclusive bound converts exactly and is not reported.
    REQUIRE(normalized.gemini.at("generationConfig").as_object().at("responseSchema").as_object()
        .at("properties").as_object().at("n").as_object() == read_request(R"({"type":"integer","minimum":1})"));
    REQUIRE(normalized.ignored_parameters.size() == 3);
    for (const auto &schema : json::array{true, json::object{}, read_request(R"({"$ref":"#"})"), read_request(R"({"type":["string","number"]})")}) {
        auto candidate = chat_request();
        candidate["response_format"] = json::object{{"type", "json_schema"}, {"json_schema", json::object{{"name", "reply"}, {"schema", schema}}}};
        auto accepted = normalize_openai(candidate, config);
        auto &generation = accepted.gemini.at("generationConfig").as_object();
        REQUIRE_FALSE(generation.contains("responseSchema"));
        REQUIRE(generation.at("responseMimeType") == "application/json");
    }
}

TEST_CASE("Compatibility keeps malformed and unknown request fields as errors", "[protocol][compat]") {
    Config config;
    config.ignore_unsupported_parameters = true;
    for (auto extra : {R"({"n":0})", R"({"n":"2"})", R"({"store":"yes"})", R"({"parallel_tool_calls":"no"})",
                       R"({"seed":"42"})", R"({"logit_bias":{"42":"high"}})", R"({"service_tier":7})",
                       R"({"tool_choice":{"type":"function","function":{"name":""}}})", R"({"unexpected_option":true})",
                       R"({"response_format":{"type":"json_schema","json_schema":{"schema":{"type":"string","patern":"x"}}}})",
                       R"({"response_format":{"type":"json_schema","json_schema":{"strict":"yes","schema":{"type":"string"}}}})",
                       R"({"response_format":{"type":"json_schema","json_schema":{"schema":{"type":"number","multipleOf":"x"}}}})"}) {
        auto request = chat_request();
        for (const auto &entry : read_request(extra)) request[entry.key()] = entry.value();
        REQUIRE_THROWS_AS(normalize_openai(request, config), Error);
    }
}

TEST_CASE("Gemini opt-in discards unsupported controls and preserves content and supported tools", "[protocol][compat]") {
    Config config;
    auto request = read_request(R"({"contents":[{"parts":[{"text":"hello"}]}],"cached_content":"cachedContents/example","generation_config":{"candidate_count":2,"audio_timestamp":true,"speech_config":{},"response_modalities":["AUDIO"]},"tools":[{"url_context":{},"code_execution":{}}],"tool_config":{"function_calling_config":{"mode":"VALIDATED"}}})");
    REQUIRE_THROWS_AS(normalize_gemini("gemma-test", request, false, config), Error);
    config.ignore_unsupported_parameters = true;
    auto accepted = normalize_gemini("gemma-test", request, false, config);
    REQUIRE(accepted.ignored_parameters.size() == 7);
    REQUIRE_FALSE(accepted.gemini.contains("cachedContent"));
    auto &tools = accepted.gemini.at("tools").as_array()[0].as_object();
    REQUIRE_FALSE(tools.contains("urlContext"));
    REQUIRE(tools.contains("codeExecution"));
    REQUIRE_NOTHROW(encode_request(accepted, {}, "snapshot"));
    for (auto invalid : {R"({"candidateCount":"2"})", R"({"audioTimestamp":"yes"})", R"({"responseModalities":[7]})"}) {
        auto bad = read_request(R"({"contents":[{"parts":[{"text":"hello"}]}]})");
        bad["generationConfig"] = read_request(invalid);
        REQUIRE_THROWS_AS(normalize_gemini("gemini-test", bad, false, config), Error);
    }
}
