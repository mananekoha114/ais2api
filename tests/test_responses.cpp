// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/responses.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
json::object read(std::string_view s) { return json::parse(s).as_object(); }
json::object chunk(std::string text) {
    return {
        {"candidates",
         json::array{json::object{
             {"content", json::object{{"role", "model"},
                                      {"parts", json::array{json::object{{"text", text}}}}}}}}}};
}
} // namespace
TEST_CASE("Responses requests translate instructions formats and tools", "[responses]") {
    auto in = read(
        R"({"model":"m","instructions":"root","input":[{"role":"user","content":[{"type":"input_text","text":"hi"}]}],"max_output_tokens":100,"reasoning":{"effort":"low"},"tools":[{"type":"function","name":"f","parameters":{"type":"object"},"strict":false}],"tool_choice":"auto","text":{"format":{"type":"json_object"}}})");
    auto chat = responses_to_chat(in);
    REQUIRE(chat.at("messages").as_array().size() == 2);
    REQUIRE(chat.at("max_completion_tokens") == 100);
    REQUIRE(chat.at("tools").as_array()[0].as_object().at("function").as_object().at("name") ==
            "f");
    REQUIRE_NOTHROW(normalize_openai(chat, Config{}));
    REQUIRE_FALSE(responses_options(in).if_contains("input"));
}
TEST_CASE("Responses reject unsupported state and input items", "[responses]") {
    for (
        auto text :
        {R"({"model":"m","input":"x","store":true})",
         R"({"model":"m","input":"x","background":true})",
         R"({"model":"m","input":"x","previous_response_id":"resp_old"})",
         R"({"model":"m","input":[{"type":"item_reference","id":"x"}]})",
         R"({"model":"m","input":[{"role":"user","content":[{"type":"input_image","image_url":"file:///secret"}]}]})"})
        REQUIRE_THROWS_AS(responses_to_chat(read(text)), Error);
}
TEST_CASE("Responses function history preserves signatures without storage", "[responses]") {
    auto gemini = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"f","args":{"n":1}},"thoughtSignature":"sig"}]}}]})");
    auto response = responses_response(gemini, "m", "resp_test", {});
    auto history = response.at("output").as_array();
    auto call = history[0].as_object().at("call_id");
    history.push_back(
        json::object{{"type", "function_call_output"}, {"call_id", call}, {"output", "done"}});
    auto req = normalize_openai(responses_to_chat({{"model", "m"}, {"input", history}}), Config{});
    auto &part =
        req.gemini.at("contents").as_array()[0].as_object().at("parts").as_array()[0].as_object();
    REQUIRE(part.at("thoughtSignature") == "sig");
    REQUIRE(part.at("functionCall").as_object().at("args").as_object().at("n") == 1);
}
TEST_CASE("Responses usage incomplete status and incremental SSE lifecycles", "[responses]") {
    auto gemini = chunk("hello");
    gemini.at("candidates").as_array()[0].as_object()["finishReason"] = "MAX_TOKENS";
    gemini["usageMetadata"] = json::object{{"promptTokenCount", 10},
                                           {"candidatesTokenCount", 5},
                                           {"totalTokenCount", 15},
                                           {"thoughtsTokenCount", 2}};
    auto r = responses_response(gemini, "m", "resp_test", {});
    REQUIRE(r.at("status") == "incomplete");
    REQUIRE(
        r.at("usage").as_object().at("output_tokens_details").as_object().at("reasoning_tokens") ==
        2);
    ResponsesStream stream("m", "resp_test", {}, 8192);
    auto events = stream.start();
    auto created = events[0].as_object().at("response").as_object().at("created_at");
    auto first = stream.push(chunk("hel"));
    REQUIRE(first.size() == 3);
    REQUIRE(first.back().as_object().at("delta") == "hel");
    auto second = stream.push(chunk("lo"));
    REQUIRE(second.size() == 1);
    REQUIRE(second[0].as_object().at("delta") == "lo");
    auto end = stream.finish();
    REQUIRE(end.back().as_object().at("type") == "response.completed");
    REQUIRE(end.back().as_object().at("response").as_object().at("created_at") == created);
    std::uint64_t sequence = 0;
    for (auto *list : {&events, &first, &second, &end})
        for (auto &event : *list)
            REQUIRE(event.as_object().at("sequence_number").as_uint64() == sequence++);
    REQUIRE(stream.finish().empty());
}
TEST_CASE("Responses streams emit function calls failures and enforce limits", "[responses]") {
    ResponsesStream stream("m", "resp_test", {}, 8192);
    auto call = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"f","args":{"n":1}},"thoughtSignature":"sig"}]}}]})");
    auto events = stream.push(call);
    REQUIRE(events.back().as_object().at("type") == "response.function_call_arguments.delta");
    auto done = stream.finish();
    REQUIRE(done[0].as_object().at("type") == "response.function_call_arguments.done");
    ResponsesStream limited("m", "resp_limit", {}, 128);
    REQUIRE_THROWS_AS(limited.push(chunk(std::string(512, 'x'))), Error);
    auto failed = limited.fail(Error(413, "response_too_large", "too big"));
    REQUIRE(failed.back().as_object().at("type") == "response.failed");
    REQUIRE(
        failed.back().as_object().at("response").as_object().at("error").as_object().at("code") ==
        "response_too_large");
    REQUIRE(limited.finish().empty());
}
TEST_CASE("Responses stable discovery order and signed ids across repeated snapshots",
          "[responses]") {
    struct Directory {
        fs::path path = fs::temp_directory_path() / ("ais2api-responses-" + random_hex(8));
        Directory() { fs::create_directories(path); }
        ~Directory() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } dir;
    Store store(dir.path);
    ResponsesStream stream("m", "resp_stable", {}, 32768, &store, "principal");
    auto calls = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"f","args":{}}},{"functionCall":{"name":"f","args":{}}}]}}]})");
    auto first = stream.push(calls);
    auto a = first[2].as_object().at("item").as_object();
    auto b = first[4].as_object().at("item").as_object();
    REQUIRE(a.at("call_id") != b.at("call_id"));
    auto text = stream.push(chunk("answer"));
    REQUIRE(text[0].as_object().at("output_index") == 2);
    auto done = stream.finish();
    auto final = done.back().as_object().at("response").as_object().at("output").as_array();
    REQUIRE(final.size() == 3);
    REQUIRE(final[0].as_object().at("id") == a.at("id"));
    REQUIRE(final[0].as_object().at("call_id") == a.at("call_id"));
    REQUIRE(final[1].as_object().at("call_id") == b.at("call_id"));
}

TEST_CASE("Responses defaults and input media retain the supported codec contract", "[responses]") {
    auto result = responses_response(chunk("answer"), "m", "resp_defaults", {});
    REQUIRE(result.at("tools").as_array().empty());
    REQUIRE(result.at("tool_choice") == "auto");
    REQUIRE(result.at("parallel_tool_calls") == true);
    REQUIRE(result.at("instructions").is_null());
    REQUIRE(result.at("text").as_object().at("format").as_object().at("type") == "text");
    auto image = responses_to_chat(read(
        R"({"model":"m","input":[{"role":"user","content":[{"type":"input_text","text":"describe"},{"type":"input_image","image_url":"data:image/png;base64,AQID"}]}]})"));
    auto request = normalize_openai(image, Config{});
    auto &parts = request.gemini.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[1].as_object().at("inlineData").as_object().at("data") == "AQID");
    auto remote = responses_to_chat(read(
        R"({"model":"m","input":[{"role":"user","content":[{"type":"input_image","image_url":"https://example.com/image.png"}]}]})"));
    REQUIRE(remote.at("messages")
                .as_array()[0]
                .as_object()
                .at("content")
                .as_array()[0]
                .as_object()
                .at("image_url")
                .as_object()
                .at("url") == "https://example.com/image.png");
    auto schema = responses_to_chat(read(
        R"({"model":"m","input":"x","text":{"format":{"type":"json_schema","name":"answer","strict":false,"schema":{"type":"object","title":"Answer","additionalProperties":false,"properties":{"answer":{"type":"string","default":"yes"}}}}}})"));
    REQUIRE(normalize_openai(schema, Config{})
                .gemini.at("generationConfig")
                .as_object()
                .at("responseMimeType") == "application/json");
    for (auto extra : {R"("parallel_tool_calls":false)", R"("metadata":{"key":42})"}) {
        REQUIRE_THROWS_AS(
            responses_to_chat(read(std::string(R"({"model":"m","input":"x",)") + extra + "}")),
            Error);
    }
}

TEST_CASE("Responses repeated explicit function calls keep one invocation", "[responses]") {
    auto call = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"id":"original-call","name":"f","args":{"n":1}},"thoughtSignature":"sig"}]}}]})");
    ResponsesStream stream("m", "resp_repeat", {}, 32768);
    auto first = stream.push(call);
    auto original = first[2].as_object().at("item").as_object();
    REQUIRE(stream.push(call).empty());
    auto done = stream.finish();
    auto output = done.back().as_object().at("response").as_object().at("output").as_array();
    REQUIRE(output.size() == 1);
    REQUIRE(output[0].as_object().at("call_id") == original.at("call_id"));
    REQUIRE(output[0].as_object().at("id") == original.at("id"));
    ResponsesStream conflicting("m", "resp_conflict", {}, 32768);
    conflicting.push(call);
    auto revised = call;
    revised.at("candidates")
        .as_array()[0]
        .as_object()
        .at("content")
        .as_object()
        .at("parts")
        .as_array()[0]
        .as_object()
        .at("functionCall")
        .as_object()["args"] = json::object{{"n", 2}};
    REQUIRE_THROWS_AS(conflicting.push(revised), Error);
    auto failure = conflicting.fail(Error(502, "upstream_protocol_error", "conflict"));
    REQUIRE(failure.back().as_object().at("type") == "response.failed");
    REQUIRE(failure.back()
                .as_object()
                .at("response")
                .as_object()
                .at("output")
                .as_array()[0]
                .as_object()
                .at("arguments") == R"({"n":1})");
}

TEST_CASE("Responses native text and reasoning signatures replay in their respective items",
          "[responses]") {
    auto native = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"thinking","thought":true,"thoughtSignature":"reasoning-sig"},{"text":"answer"},{"text":"","thoughtSignature":"text-sig"},{"inlineData":{"mimeType":"image/png","data":"AQID"}}]}}]})");
    auto result = responses_response(native, "m", "resp_native", {});
    const auto &items = result.at("output").as_array();
    REQUIRE(items.size() == 2);
    CHECK(items[0].as_object().at("type") == "reasoning");
    CHECK(items[0].as_object().at("thought_signature") == "reasoning-sig");
    CHECK(items[1].as_object().at("type") == "message");
    CHECK(items[1].as_object().at("thought_signature") == "text-sig");
    auto request = normalize_openai(
        responses_to_chat({{"model", "m"}, {"input", result.at("output")}}), Config{});
    auto &turns = request.gemini.at("contents").as_array();
    REQUIRE(turns.size() == 1);
    const auto &parts = turns[0].as_object().at("parts").as_array();
    REQUIRE(parts.size() == 4);
    REQUIRE(parts[0].as_object().at("thoughtSignature") == "reasoning-sig");
    REQUIRE(parts[1].as_object().at("text") == "answer");
    REQUIRE(parts[2].as_object().at("thoughtSignature") == "text-sig");
    REQUIRE(parts[3].as_object().at("inlineData").as_object().at("data") == "AQID");
}

TEST_CASE("Responses opt-in sanitizer discards known optional features", "[responses]") {
    auto input = read(R"({"model":"m","input":"hello","store":true,"background":true,"previous_response_id":"resp_old","conversation":{"id":"conv_old"},"include":["reasoning.encrypted_content"],"truncation":"auto","service_tier":"priority","parallel_tool_calls":false,"reasoning":{"effort":"low","summary":"concise"},"tools":[{"type":"web_search"},{"type":"function","name":"f"},{"type":"file_search","vector_store_ids":["vs_1"]}],"tool_choice":{"type":"web_search"}})");
    json::array ignored;
    discard_unsupported_responses(input, ignored);
    REQUIRE(ignored.size() == 11);
    REQUIRE(input.at("tools").as_array().size() == 2);
    REQUIRE(input.at("reasoning").as_object().at("effort") == "low");
    REQUIRE(input.at("input") == "hello");
    REQUIRE_NOTHROW(responses_to_chat(input));
    REQUIRE_FALSE(responses_options(input).contains("tool_choice"));

    auto unsupported_xhigh = read(R"({"model":"gemini-4-argon","input":"hello","reasoning":{"effort":"xhigh"}})");
    json::array ignored_xhigh;
    discard_unsupported_responses(unsupported_xhigh, ignored_xhigh);
    REQUIRE(ignored_xhigh == json::array{"reasoning.effort"});
    REQUIRE_FALSE(responses_options(unsupported_xhigh).contains("reasoning"));

    Config cfg;
    cfg.ignore_unsupported_parameters = true;
    cfg.model_aliases["neon"] = "gemini-4-neon";
    auto supported_xhigh = read(R"({"model":"neon","input":"hello","reasoning":{"effort":"xhigh"}})");
    json::array kept_ignored;
    discard_unsupported_responses(supported_xhigh, kept_ignored, cfg);
    REQUIRE(kept_ignored.empty());
    REQUIRE(responses_options(supported_xhigh).at("reasoning").as_object().at("effort") == "xhigh");
    auto lowered_xhigh = normalize_openai(responses_to_chat(supported_xhigh), cfg);
    REQUIRE(lowered_xhigh.ignored_parameters.empty());
    REQUIRE(encode_request(lowered_xhigh, {}, "s")[3].as_array()[16].as_array()[3] == 6);
}
TEST_CASE("Responses sanitizer preserves malformed and unknown fields", "[responses]") {
    for (auto field : {R"("store":"true")", R"("previous_response_id":4)",
                       R"("conversation":{"oops":"x"})", R"("include":["typo"])",
                       R"("truncation":"automatic")", R"("service_tier":"fastest")",
                       R"("parallel_tool_calls":0)", R"("reasoning":{"summary":"typo"})",
                       R"("tools":[{"type":"web_serch"}])",
                       R"("tools":[{"type":"web_search","search_context_size":4}])",
                       R"("tool_choice":{"type":"web_search","typo":true})"}) {
        auto input = read(std::string(R"({"model":"m","input":"hello",)") + field + "}");
        auto original = input;
        json::array ignored;
        discard_unsupported_responses(input, ignored);
        REQUIRE(input == original);
        REQUIRE(ignored.empty());
        REQUIRE_THROWS_AS(responses_to_chat(input), Error);
    }
}

TEST_CASE("Responses sanitizer removes strict flags before echo options", "[responses]") {
    auto input = read(R"({"model":"m","input":"x","tools":[{"type":"function","name":"f","strict":true}],"text":{"format":{"type":"json_schema","name":"r","strict":true,"schema":{"type":"object"}}}})");
    json::array ignored;
    discard_unsupported_responses(input, ignored);
    REQUIRE(ignored.size() == 2);
    auto options = responses_options(input);
    REQUIRE_FALSE(options.at("tools").as_array()[0].as_object().contains("strict"));
    REQUIRE_FALSE(options.at("text").as_object().at("format").as_object().contains("strict"));
    REQUIRE(input.at("text").as_object().at("format").as_object().contains("schema"));
}

TEST_CASE("Responses sanitizer understands common built-in tool configuration", "[responses]") {
    for (auto tool : {
        R"({"type":"code_interpreter","container":{"type":"auto","file_ids":["file_1"],"memory_limit":"4g"}})",
        R"({"type":"mcp","server_label":"docs","headers":{"Authorization":"secret"},"allowed_tools":["lookup"],"require_approval":"never"})",
        R"({"type":"mcp","server_label":"docs","allowed_tools":{"tool_names":["lookup"],"read_only":true},"require_approval":{"always":{"tool_names":["write"]},"never":{"tool_names":["lookup"]}}})"}) {
        auto input = read(std::string(R"({"model":"m","input":"x","tools":[)") + tool + "]}");
        json::array ignored;
        discard_unsupported_responses(input, ignored);
        REQUIRE(input.at("tools").as_array().empty());
        REQUIRE(ignored.size() == 1);
        REQUIRE_NOTHROW(responses_to_chat(input));
    }
}
TEST_CASE("Responses web_search maps to Chat web_search_options", "[responses][grounding]") {
    for (bool sanitize : {false, true}) {
        auto input = read(R"({"model":"m","input":"x","tools":[{"type":"web_search","search_context_size":"high","filters":{"allowed_domains":["example.com"]},"user_location":{"type":"approximate","city":"Tokyo","country":"JP","timezone":"Asia/Tokyo"}},{"type":"function","name":"f"}]})");
        json::array ignored;
        if (sanitize) discard_unsupported_responses(input, ignored);
        REQUIRE(ignored.empty());
        auto chat = responses_to_chat(input);
        REQUIRE(chat.at("web_search_options") == json::parse(R"({"search_context_size":"high","user_location":{"type":"approximate","approximate":{"city":"Tokyo","country":"JP","timezone":"Asia/Tokyo"}},"filters":{"allowed_domains":["example.com"]}})"));
        REQUIRE(chat.at("tools").as_array().size() == 1);
    }
    auto only = responses_to_chat(read(R"({"model":"m","input":"x","tools":[{"type":"web_search_preview"}]})"));
    REQUIRE(only.at("web_search_options") == json::object{});
    REQUIRE_FALSE(only.contains("tools"));
    REQUIRE_THROWS_AS(responses_to_chat(read(R"({"model":"m","input":"x","tools":[{"type":"web_search"},{"type":"web_search_preview"}]})")), Error);
}
TEST_CASE("Responses sanitizer preserves malformed built-in configuration", "[responses]") {
    for (auto tool : {
        R"({"type":"code_interpreter","container":{"type":"auto","memory_limit":"2g"}})",
        R"({"type":"code_interpreter","container":{"type":"auto","file_ids":[3]}})",
        R"({"type":"code_interpreter","container":{"type":"auto","typo":true}})",
        R"({"type":"web_search","filters":{"allowed_domains":[false]}})",
        R"({"type":"web_search","filters":{"allowed_domains":[],"typo":true}})",
        R"({"type":"web_search","user_location":{"type":"approximate","city":1}})",
        R"({"type":"web_search","user_location":{"type":"exact"}})",
        R"({"type":"mcp","server_label":"docs","headers":{"x":3}})",
        R"({"type":"mcp","server_label":"docs","allowed_tools":{"tool_names":[],"read_only":"true"}})",
        R"({"type":"mcp","server_label":"docs","require_approval":{"always":{"tool_names":[],"typo":true}}})",
        R"({"type":"mcp","server_label":"docs","require_approval":"sometimes"})"}) {
        auto input = read(std::string(R"({"model":"m","input":"x","tools":[)") + tool + "]}");
        auto original = input;
        json::array ignored;
        discard_unsupported_responses(input, ignored);
        REQUIRE(input == original);
        REQUIRE(ignored.empty());
        REQUIRE_THROWS_AS(responses_to_chat(input), Error);
    }
}
TEST_CASE("Incremental Responses text deltas match full recomputation event for event",
          "[responses][stream-performance]") {
    const auto chunks = [](std::initializer_list<const char*> values) {
        std::vector<json::object> result;
        for (auto value : values) result.push_back(read(value));
        return result;
    };
    const std::vector<std::vector<json::object>> streams{
        chunks({R"({"candidates":[{"content":{"role":"model","parts":[{"text":"Hel"}]}}]})",
                R"({"candidates":[{"content":{"role":"model","parts":[{"text":"lo"}]}}],"usageMetadata":{"promptTokenCount":2}})",
                R"({"candidates":[{"index":0,"content":{"parts":[{"text":" wor\"ld\n"}]}}],"modelVersion":"v"})",
                R"({"candidates":[{"content":{"role":"model","parts":[{"text":""}]}}]})",
                R"({"candidates":[{"content":{"role":"model","parts":[{"text":"!"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":2,"candidatesTokenCount":3,"totalTokenCount":5}})"}),
        chunks({R"({"candidates":[{"content":{"parts":[{"text":"think ","thought":true}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":"more","thought":true}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":"answer"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":" goes","thought":false}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":"late thought","thought":true}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":" on"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":"","thoughtSignature":"text-sig"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":" after"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":" signed"}]}}],"usageMetadata":{"thoughtsTokenCount":4}})"}),
        chunks({R"({"candidates":[{"content":{"parts":[{"text":"look","thoughtSignature":"s1"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":"ing"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"inlineData":{"mimeType":"image/png","data":"AQID"}}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":"caption"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":" text"}]},"safetyRatings":[{"category":"x"}]}]})",
                R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"f","args":{"q":"v"}}}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":"after call"}]}}]})",
                R"({"candidates":[{"content":{"parts":[{"text":" continues"}]},"finishReason":"MAX_TOKENS"}]})"}),
        chunks({R"({"candidates":[{"index":1,"content":{"parts":[{"text":"second"}]}}]})",
                R"({"candidates":[{"index":0,"content":{"parts":[{"text":"first"}]}}]})",
                R"({"candidates":[{"index":1,"content":{"parts":[{"text":" more"}]}}]})",
                R"({"candidates":[{"index":0,"content":{"parts":[{"text":" more"}]}},{"index":1,"content":{"parts":[{"text":"!"}]}}]})",
                R"({"candidates":[{"index":0,"content":{"parts":[{"text":" end","extra":1}]}}]})"})};
    // created_at is a wall-clock second; it is not part of the comparison.
    const auto normalize = [](json::array events) {
        for (auto& value : events)
            if (auto response = value.as_object().if_contains("response"))
                response->as_object().erase("created_at");
        return json::serialize(events);
    };
    for (std::size_t limit : {std::size_t{65536}, std::size_t{900}, std::size_t{1400}}) {
        for (std::size_t s = 0; s < streams.size(); ++s) {
            INFO("stream " << s << " limit " << limit);
            ResponsesStream fast("m", "resp_diff", {}, limit), full("m", "resp_diff", {}, limit);
            full.incremental = false;
            bool failed = false;
            for (const auto& delta : streams[s]) {
                std::string fast_events, full_events, fast_error, full_error;
                try { fast_events = normalize(fast.push(delta)); } catch (const Error& e) { fast_error = e.code; }
                try { full_events = normalize(full.push(delta)); } catch (const Error& e) { full_error = e.code; }
                CHECK(fast_events == full_events);
                REQUIRE(fast_error == full_error);
                if (!fast_error.empty()) { failed = true; break; }
            }
            const Error error(413, "response_too_large", "too big");
            CHECK(normalize(failed ? fast.fail(error) : fast.finish()) ==
                  normalize(failed ? full.fail(error) : full.finish()));
        }
    }
}

TEST_CASE("Responses coalesces assistant items, avoids phantom messages, persists tools on finish, accepts null, and rolls back first push",
          "[responses][regression]") {
    // 1. Coalescing reasoning + message + parallel function_calls into one model turn.
    const auto multi = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"plan","thought":true,"thoughtSignature":"r-sig"},{"text":"call tools"},{"functionCall":{"name":"f","args":{"a":1}},"thoughtSignature":"c1-sig"},{"functionCall":{"name":"g","args":{"b":2}},"thoughtSignature":"c2-sig"}]}}]})");
    const auto multi_resp = responses_response(multi, "m", "resp_multi", {});
    const auto &multi_out = multi_resp.at("output").as_array();
    REQUIRE(multi_out.size() == 4);
    CHECK(multi_out[0].as_object().at("thought_signature") == "r-sig");
    CHECK_FALSE(multi_out[1].as_object().contains("thought_signature"));
    auto multi_input = multi_out;
    multi_input.push_back(json::object{{"type", "function_call_output"},
                                       {"call_id", multi_out[2].as_object().at("call_id")},
                                       {"output", "r1"}});
    multi_input.push_back(json::object{{"type", "function_call_output"},
                                       {"call_id", multi_out[3].as_object().at("call_id")},
                                       {"output", "r2"}});
    const auto multi_req =
        normalize_openai(responses_to_chat({{"model", "m"}, {"input", multi_input}}), Config{});
    const auto &multi_turns = multi_req.gemini.at("contents").as_array();
    REQUIRE(multi_turns.size() == 2);
    CHECK(multi_turns[0].as_object().at("role") == "model");
    CHECK(multi_turns[0].as_object().at("parts").as_array().size() == 4);
    CHECK(multi_turns[1].as_object().at("role") == "user");
    CHECK(multi_turns[1].as_object().at("parts").as_array().size() == 2);

    // 2. Reasoning-only or reasoning + tool call omits phantom empty message and duplicate signature.
    const auto reasoning_only = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"think","thought":true,"thoughtSignature":"r-sig"}]}}]})");
    const auto ro_resp = responses_response(reasoning_only, "m", "resp_ro", {});
    REQUIRE(ro_resp.at("output").as_array().size() == 1);
    CHECK(ro_resp.at("output").as_array()[0].as_object().at("type") == "reasoning");
    CHECK(ro_resp.at("output").as_array()[0].as_object().at("thought_signature") == "r-sig");

    const auto reasoning_and_text = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"think","thought":true,"thoughtSignature":"r-sig"},{"text":"visible"}]}}]})");
    const auto rt_resp = responses_response(reasoning_and_text, "m", "resp_rt", {});
    REQUIRE(rt_resp.at("output").as_array().size() == 2);
    CHECK(rt_resp.at("output").as_array()[0].as_object().at("thought_signature") == "r-sig");
    CHECK_FALSE(rt_resp.at("output").as_array()[1].as_object().contains("thought_signature"));

    // 3. ResponsesStream persists tool metadata in finish(), not intermediate push().
    struct Directory {
        fs::path path = fs::temp_directory_path() / ("ais2api-resp-store-" + random_hex(8));
        Directory() { fs::create_directories(path); }
        ~Directory() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } dir;
    Store store(dir.path);
    ResponsesStream stream("m", "resp_persist", {}, 32768, &store, "principal");
    const auto unsigned_call = read(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"lookup","args":{"q":1}}}]}}]})");
    const auto pushed = stream.push(unsigned_call);
    const auto call_id =
        string_or(pushed[2].as_object().at("item").as_object(), "call_id");
    CHECK_FALSE(store.get_tool("principal", call_id).has_value());
    (void)stream.finish();
    const auto stored = store.get_tool("principal", call_id);
    REQUIRE(stored.has_value());
    CHECK(stored->at("name") == "lookup");
    const auto follow_up = normalize_openai(
        responses_to_chat(
            {{"model", "m"},
             {"input", json::array{json::object{{"type", "function_call_output"},
                                                {"call_id", call_id},
                                                {"output", "ok"}}}}}),
        Config{}, &store, "principal");
    CHECK(follow_up.gemini.at("contents")
              .as_array()[0]
              .as_object()
              .at("parts")
              .as_array()[0]
              .as_object()
              .at("functionResponse")
              .as_object()
              .at("name") == "lookup");

    // 4. Explicit JSON null on optional Responses parameters.
    const auto nullable = read(R"({
        "model":"m",
        "input":"hi",
        "instructions":null,
        "stream":null,
        "store":null,
        "background":null,
        "previous_response_id":null,
        "conversation":null,
        "include":null,
        "truncation":null,
        "service_tier":null,
        "metadata":null,
        "parallel_tool_calls":null,
        "max_output_tokens":null,
        "reasoning":{"effort":null,"summary":null},
        "text":null,
        "temperature":null,
        "top_p":null,
        "tools":null,
        "tool_choice":null,
        "user":null,
        "ais2api":null
    })");
    const auto chat_nullable = responses_to_chat(nullable);
    REQUIRE_NOTHROW(normalize_openai(chat_nullable, Config{}));
    const auto opts = responses_options(nullable);
    const auto defaulted = responses_response(chunk("ok"), "m", "resp_null", opts);
    CHECK(defaulted.at("temperature") == 1.0);
    CHECK(defaulted.at("tools").as_array().empty());
    CHECK(defaulted.at("text").at("format").at("type") == "text");

    // 5. ResponsesStream::push rollback on first delta restores started_, sequence_, and output_bound_.
    ResponsesStream limited("m", "resp_rollback", {}, 128);
    REQUIRE_THROWS_AS(limited.push(chunk(std::string(512, 'x'))), Error);
    const auto failed_events = limited.fail(Error(413, "response_too_large", "too big"));
    REQUIRE(failed_events.size() == 3);
    CHECK(failed_events[0].as_object().at("type") == "response.created");
    CHECK(failed_events[0].as_object().at("sequence_number") == 0);
    CHECK(failed_events[1].as_object().at("type") == "response.in_progress");
    CHECK(failed_events[1].as_object().at("sequence_number") == 1);
    CHECK(failed_events[2].as_object().at("type") == "response.failed");
    CHECK(failed_events[2].as_object().at("sequence_number") == 2);
}
