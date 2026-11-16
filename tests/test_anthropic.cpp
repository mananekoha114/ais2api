// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/anthropic.hpp"
#include "ais2api/tokenizer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <map>

using namespace ais2api;

namespace {
json::object parse(std::string_view text) { return json::parse(text).as_object(); }
const json::array &parts_of(const json::object &body, std::size_t index) {
    return body.at("contents").as_array().at(index).as_object().at("parts").as_array();
}
json::array names_of(const json::object &body) {
    json::array names;
    if (auto tools = body.if_contains("tools"))
        for (auto &function : tools->as_array()[0].as_object().at("functionDeclarations").as_array())
            names.push_back(function.as_object().at("name"));
    return names;
}
std::uint16_t status_of(const json::object &input, bool discard) {
    try {
        (void)anthropic_to_gemini(input, discard);
    } catch (const Error &error) {
        return static_cast<std::uint16_t>(error.status);
    }
    return 200;
}
json::object chunk(json::array parts, std::string finish = {}) {
    json::object candidate{{"index", 0}, {"content", json::object{{"role", "model"}, {"parts", std::move(parts)}}}};
    if (!finish.empty()) candidate["finishReason"] = finish;
    return {{"candidates", json::array{std::move(candidate)}}};
}
// Rebuilds a message from stream events the way an SDK accumulates them.
json::object replay(const json::array &events) {
    json::object message;
    std::map<std::size_t, std::string> inputs;
    for (auto &value : events) {
        auto &event = value.as_object();
        const auto type = std::string(event.at("type").as_string());
        if (type == "message_start") message = event.at("message").as_object();
        else if (type == "content_block_start") message.at("content").as_array().push_back(event.at("content_block"));
        else if (type == "content_block_delta") {
            const auto index = static_cast<std::size_t>(event.at("index").to_number<std::uint64_t>());
            auto &block = message.at("content").as_array().at(index).as_object();
            auto &delta = event.at("delta").as_object();
            const auto kind = std::string(delta.at("type").as_string());
            if (kind == "text_delta") block.at("text").as_string().append(delta.at("text").as_string());
            else if (kind == "thinking_delta") block.at("thinking").as_string().append(delta.at("thinking").as_string());
            else if (kind == "signature_delta") block["signature"] = delta.at("signature");
            else inputs[index] += std::string(delta.at("partial_json").as_string());
        } else if (type == "content_block_stop") {
            const auto index = static_cast<std::size_t>(event.at("index").to_number<std::uint64_t>());
            if (inputs.contains(index)) message.at("content").as_array().at(index).as_object()["input"] = json::parse(inputs[index]);
        } else if (type == "message_delta") {
            for (auto &kv : event.at("delta").as_object()) message[kv.key()] = kv.value();
            message["usage"] = event.at("usage");
        }
    }
    return message;
}
} // namespace

TEST_CASE("Anthropic requests lower to native Gemini requests", "[anthropic]") {
    const auto input = parse(R"({"model":"gemini-test","max_tokens":256,"stream":true,
        "system":[{"type":"text","text":"Be brief.","cache_control":{"type":"ephemeral"}}],
        "messages":[{"role":"user","content":"Hello"},
                    {"role":"assistant","content":[{"type":"text","text":"Hi"}]},
                    {"role":"user","content":[{"type":"text","text":"Again"},
                        {"type":"image","source":{"type":"base64","media_type":"image/png","data":"iVBORw0KGgo="}}]}],
        "temperature":0.5,"top_p":0.9,"top_k":40,"stop_sequences":["END"],"metadata":{"user_id":"u"}})");
    auto lowered = anthropic_to_gemini(input, false);
    CHECK(lowered.model == "gemini-test");
    CHECK(lowered.stream);
    CHECK_FALSE(lowered.thinking);
    CHECK(lowered.ignored.empty());
    const auto expected = parse(R"({"contents":[{"role":"user","parts":[{"text":"Hello"}]},
        {"role":"model","parts":[{"text":"Hi"}]},
        {"role":"user","parts":[{"text":"Again"},{"inlineData":{"mimeType":"image/png","data":"iVBORw0KGgo="}}]}],
        "generationConfig":{"maxOutputTokens":256,"temperature":0.5,"topP":0.9,"topK":40,"stopSequences":["END"],
            "thinkingConfig":{"includeThoughts":false}},
        "systemInstruction":{"role":"user","parts":[{"text":"Be brief."}]}})");
    CHECK(lowered.gemini == expected);
    Config config;
    CHECK_FALSE(normalize_gemini(lowered.model, lowered.gemini, true, config).include_thoughts);
    // max_tokens is required except when counting.
    auto counting = input;
    counting.erase("max_tokens");
    CHECK(status_of(counting, true) == 400);
    CHECK(anthropic_to_gemini(counting, false, true).gemini.at("generationConfig").as_object().if_contains("maxOutputTokens") == nullptr);
}

TEST_CASE("Claude Code's default request shape is accepted without the compatibility switch", "[anthropic]") {
    const auto input = parse(R"({"model":"gemini-test","max_tokens":32000,"stream":true,
        "thinking":{"type":"adaptive","display":"omitted"},"output_config":{"effort":"high"},
        "context_management":{"edits":[{"type":"clear_thinking_20251015","keep":"all"}]},
        "metadata":{"user_id":"x"},
        "system":[{"type":"text","text":"You are a coding agent."}],
        "tools":[{"name":"Read","description":"Read a file","input_schema":{"type":"object",
                    "properties":{"limit":{"type":"integer","exclusiveMinimum":0},"args":{"description":"Any value"}}}},
                 {"name":"DeferredToolPlaceholder","description":"d","input_schema":{"type":"object","properties":{}},"defer_loading":true}],
        "messages":[{"role":"user","content":[{"type":"text","text":"question"}]},
                    {"role":"system","content":[{"type":"text","text":"reminder","cache_control":{"type":"ephemeral"}}]}]})");
    auto lowered = anthropic_to_gemini(input, false);
    CHECK(lowered.thinking);
    CHECK(lowered.ignored.empty());
    CHECK(lowered.gemini.at("generationConfig").at("thinkingConfig") == parse(R"({"includeThoughts":false,"thinkingLevel":"HIGH"})"));
    // A mid-conversation system message joins the user turn it follows.
    REQUIRE(lowered.gemini.at("contents").as_array().size() == 1);
    CHECK(parts_of(lowered.gemini, 0) == json::array{json::object{{"text", "question"}}, json::object{{"text", "reminder"}}});
    CHECK(names_of(lowered.gemini) == json::array{"Read", "DeferredToolPlaceholder"});
    Config config;
    auto request = normalize_gemini(lowered.model, lowered.gemini, true, config);
    const auto &read = request.gemini.at("tools").as_array()[0].at("functionDeclarations").as_array()[0].as_object();
    CHECK(read.at("parameters").at("properties").at("limit").at("minimum") == 1);
    CHECK(request.json_text_args == parse(R"({"Read":[["args"]]})"));
    for (auto [effort, level] : {std::pair{"low", "LOW"}, {"medium", "MEDIUM"}, {"xhigh", "HIGH"}, {"max", "HIGH"}}) {
        auto copy = input;
        copy["output_config"] = json::object{{"effort", effort}};
        CHECK(anthropic_to_gemini(copy, false).gemini.at("generationConfig").at("thinkingConfig").at("thinkingLevel") == level);
    }
    auto summarized = input;
    summarized["thinking"] = json::object{{"type", "adaptive"}};
    CHECK(anthropic_to_gemini(summarized, false).gemini.at("generationConfig").at("thinkingConfig").at("includeThoughts") == true);
}

TEST_CASE("Thinking signatures and tool calls round-trip through Anthropic messages", "[anthropic]") {
    const auto gemini = parse(R"({"candidates":[{"index":0,"content":{"role":"model","parts":[
        {"text":"pondering","thought":true},
        {"text":"Let me read it.","thoughtSignature":"sig-text"},
        {"functionCall":{"name":"Read","args":{"path":"a.txt"}},"thoughtSignature":"sig-call"}]},"finishReason":"STOP"}],
        "usageMetadata":{"promptTokenCount":100,"cachedContentTokenCount":40,"candidatesTokenCount":30,"thoughtsTokenCount":10}})");
    auto message = anthropic_response(gemini, "gemini-test", "msg_test", true);
    CHECK(message.at("id") == "msg_test");
    CHECK(message.at("type") == "message");
    CHECK(message.at("role") == "assistant");
    CHECK(message.at("model") == "gemini-test");
    CHECK(message.at("stop_reason") == "tool_use");
    CHECK(message.at("stop_sequence").is_null());
    CHECK(message.at("usage") == parse(R"({"input_tokens":60,"cache_creation_input_tokens":0,"cache_read_input_tokens":40,"output_tokens":30})"));
    const auto content = message.at("content").as_array();
    REQUIRE(content.size() == 3);
    CHECK(content[0] == parse(R"({"type":"thinking","thinking":"pondering","signature":"ais2api.gemini.sig-text"})"));
    CHECK(content[1] == parse(R"({"type":"text","text":"Let me read it."})"));
    const auto &call = content[2].as_object();
    CHECK(call.at("type") == "tool_use");
    CHECK(call.at("name") == "Read");
    CHECK(call.at("input") == parse(R"({"path":"a.txt"})"));
    const auto id = std::string(call.at("id").as_string());
    CHECK(client_call(id).signature == "sig-call");

    // The next request carries the turn back with a tool result and an image.
    json::object result{{"type", "tool_result"},
                        {"tool_use_id", id},
                        {"content", json::array{json::object{{"type", "text"}, {"text", "file body"}},
                                                parse(R"({"type":"image","source":{"type":"base64","media_type":"image/png","data":"iVBORw0KGgo="}})")}}};
    json::object next{{"model", "gemini-test"},
                      {"max_tokens", 64},
                      {"thinking", json::object{{"type", "adaptive"}}},
                      {"messages", json::array{json::object{{"role", "user"}, {"content", "read a.txt"}},
                                               json::object{{"role", "assistant"}, {"content", content}},
                                               json::object{{"role", "user"}, {"content", json::array{result}}}}}};
    auto lowered = anthropic_to_gemini(next, false);
    CHECK(lowered.ignored.empty());
    const auto model_parts = parse(R"({"p":[{"text":"pondering","thought":true},{"text":"","thoughtSignature":"sig-text"},
        {"text":"Let me read it."},{"functionCall":{"name":"Read","args":{"path":"a.txt"}},"thoughtSignature":"sig-call"}]})");
    CHECK(parts_of(lowered.gemini, 1) == model_parts.at("p"));
    const auto user_parts = parse(R"({"p":[{"functionResponse":{"name":"Read","response":{"result":"file body"},
        "parts":[{"inlineData":{"mimeType":"image/png","data":"iVBORw0KGgo="}}]}}]})");
    CHECK(parts_of(lowered.gemini, 2) == user_parts.at("p"));
    Config config;
    auto request = normalize_gemini(lowered.model, lowered.gemini, false, config);
    CHECK_NOTHROW(encode_request(request, {}, "!snapshot"));
    // An error result and an unknown tool id.
    result["is_error"] = true;
    result["content"] = "missing";
    next.at("messages").as_array()[2].as_object()["content"] = json::array{result};
    CHECK(parts_of(anthropic_to_gemini(next, false).gemini, 2)[0].at("functionResponse").at("response") ==
          parse(R"({"error":"missing"})"));
    result["tool_use_id"] = "toolu_unknown";
    next.at("messages").as_array()[2].as_object()["content"] = json::array{result};
    CHECK(status_of(next, true) == 400);
}

TEST_CASE("Thinking from other sources never reaches the upstream", "[anthropic]") {
    const auto input = parse(R"({"model":"gemini-test","max_tokens":64,"messages":[
        {"role":"user","content":"q"},
        {"role":"assistant","content":[{"type":"thinking","thinking":"t","signature":"EqQBCkYIBxgCKkB"},
            {"type":"redacted_thinking","data":"opaque"},{"type":"text","text":"answer"}]},
        {"role":"user","content":"more"}]})");
    auto lowered = anthropic_to_gemini(input, false);
    CHECK(parts_of(lowered.gemini, 1) == json::array{json::object{{"text", "t"}, {"thought", true}}, json::object{{"text", "answer"}}});
    CHECK(lowered.ignored == json::array{"messages.content.thinking.signature"});
}

TEST_CASE("clear_thinking keeps only the latest thinking turns", "[anthropic]") {
    const auto input = parse(R"({"model":"gemini-test","max_tokens":64,
        "context_management":{"edits":[{"type":"clear_thinking_20251015","keep":{"type":"thinking_turns","value":1}}]},
        "messages":[{"role":"user","content":"a"},
            {"role":"assistant","content":[{"type":"thinking","thinking":"old","signature":"ais2api.gemini.s1"},{"type":"text","text":"a1"}]},
            {"role":"user","content":"b"},
            {"role":"assistant","content":[{"type":"thinking","thinking":"new","signature":"ais2api.gemini.s2"},{"type":"text","text":"b1"}]},
            {"role":"user","content":"c"}]})");
    auto lowered = anthropic_to_gemini(input, false);
    CHECK(parts_of(lowered.gemini, 1) == json::array{json::object{{"text", "a1"}}});
    CHECK(parts_of(lowered.gemini, 3).size() == 3);
    CHECK(parts_of(lowered.gemini, 3)[1] == parse(R"({"text":"","thoughtSignature":"s2"})"));
}

TEST_CASE("Fields the upstream cannot honor fail by default and drop with the switch", "[anthropic][compat]") {
    const auto base = parse(R"({"model":"gemini-test","max_tokens":64,"messages":[{"role":"user","content":"q"}],
        "tools":[{"name":"lookup","input_schema":{"type":"object","properties":{}}}]})");
    for (auto [extra, name] : {std::pair{R"({"tool_choice":{"type":"any"}})", "tool_choice"},
                               {R"({"tool_choice":{"type":"auto","disable_parallel_tool_use":true}})", "tool_choice.disable_parallel_tool_use"},
                               {R"({"tools":[{"type":"web_search_20260209","name":"web_search"}]})", "tools.web_search_20260209"},
                               {R"({"tools":[{"name":"lookup","strict":true,"input_schema":{"type":"object"}}]})", "tools.strict"},
                               {R"({"context_management":{"edits":[{"type":"clear_tool_uses_20250919"}]}})", "context_management.edits"},
                               {R"({"output_config":{"task_budget":{"type":"tokens","total":20000}}})", "output_config.task_budget"},
                               {R"({"service_tier":"auto"})", "service_tier"},
                               {R"({"mcp_servers":[]})", "mcp_servers"}}) {
        CAPTURE(extra);
        auto input = base;
        for (auto &kv : parse(extra)) input[kv.key()] = kv.value();
        CHECK(status_of(input, false) == 400);
        auto lowered = anthropic_to_gemini(input, true);
        CHECK(std::find(lowered.ignored.begin(), lowered.ignored.end(), json::value(name)) != lowered.ignored.end());
    }
    // Unknown fields and content that cannot be represented stay errors.
    for (auto extra : {R"({"bogus":true})",
                       R"({"messages":[{"role":"user","content":[{"type":"image","source":{"type":"url","url":"https://example.com/a.png"}}]}]})",
                       R"({"messages":[{"role":"user","content":[{"type":"search_result","source":"s","title":"t","content":[]}]}]})",
                       R"({"thinking":{"type":"enabled","budget_tokens":1024,"display":"loud"}})",
                       R"({"output_config":{"effort":"extreme"}})"}) {
        CAPTURE(extra);
        auto input = base;
        for (auto &kv : parse(extra)) input[kv.key()] = kv.value();
        CHECK(status_of(input, true) == 400);
    }
    // A thinking budget is noted, like Gemini's thinkingBudget.
    auto budget = base;
    budget["thinking"] = json::object{{"type", "enabled"}, {"budget_tokens", 2048}};
    CHECK(anthropic_to_gemini(budget, false).ignored == json::array{"thinking.budget_tokens"});
    auto none = base;
    none["tool_choice"] = json::object{{"type", "none"}};
    CHECK(anthropic_to_gemini(none, false).gemini.at("toolConfig") == parse(R"({"functionCallingConfig":{"mode":"NONE"}})"));
}

TEST_CASE("Mid-conversation system messages place text and change tools in order", "[anthropic]") {
    const auto input = parse(R"({"model":"gemini-test","max_tokens":64,
        "tools":[{"name":"A","input_schema":{"type":"object"}},{"name":"B","defer_loading":true,"input_schema":{"type":"object"}}],
        "messages":[{"role":"system","content":"early"},
            {"role":"user","content":"q"},
            {"role":"assistant","content":[{"type":"tool_use","id":"toolu_1","name":"A","input":{}}]},
            {"role":"system","content":[{"type":"text","text":"note"},
                {"type":"tool_removal","tool":{"type":"tool_reference","name":"A"}},
                {"type":"tool_addition","tool":{"type":"tool_definition","definition":{"name":"C","input_schema":{"type":"object"}}}}]},
            {"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"done"},{"type":"text","text":"more"}]},
            {"role":"system","content":"for one turn","clear_at":"next_user_message"},
            {"role":"user","content":"last"},
            {"role":"system","content":"","output_config":{"effort":"low"}}]})");
    auto lowered = anthropic_to_gemini(input, false);
    CHECK(lowered.gemini.at("systemInstruction") == parse(R"({"role":"user","parts":[{"text":"early"}]})"));
    CHECK(names_of(lowered.gemini) == json::array{"B", "C"});
    // System text after a function call follows its responses.
    const auto expected = parse(R"({"p":[{"functionResponse":{"name":"A","response":{"result":"done"},"id":"toolu_1"}},
        {"text":"note"},{"text":"more"},{"text":"last"}]})");
    CHECK(parts_of(lowered.gemini, 2) == expected.at("p"));
    CHECK(lowered.gemini.at("generationConfig").at("thinkingConfig").at("thinkingLevel") == "LOW");
    auto unknown = input;
    unknown.at("messages").as_array()[3].as_object()["content"] =
        parse(R"({"c":[{"type":"tool_addition","tool":{"type":"tool_reference","name":"Z"}}]})").at("c");
    CHECK(status_of(unknown, true) == 400);
}

TEST_CASE("Anthropic SSE follows the Messages event order and matches the unary message", "[anthropic]") {
    const std::vector<json::object> deltas{
        chunk(json::array{json::object{{"text", "pon"}, {"thought", true}}}),
        chunk(json::array{json::object{{"text", "dering"}, {"thought", true}}}),
        chunk(json::array{json::object{{"text", "Hello "}, {"thoughtSignature", "s1"}}}),
        chunk(json::array{json::object{{"text", "world"}}}),
        chunk(json::array{parse(R"({"functionCall":{"name":"lookup","args":{"q":1}}})")}, "STOP"),
        json::object{{"usageMetadata", parse(R"({"promptTokenCount":10,"candidatesTokenCount":5})")}}};
    AnthropicStream stream("gemini-test", "msg_stream", true);
    json::array events;
    json::object aggregate;
    for (const auto &delta : deltas) {
        for (auto &event : stream.push(delta)) events.push_back(event);
        merge_event(aggregate, delta, 1 << 20);
    }
    for (auto &event : stream.finish()) events.push_back(event);
    std::vector<std::string> sequence;
    for (auto &event : events) {
        auto &object = event.as_object();
        auto type = std::string(object.at("type").as_string());
        if (type == "content_block_delta") type = std::string(object.at("delta").at("type").as_string());
        else if (type == "content_block_start") type += ":" + std::string(object.at("content_block").at("type").as_string());
        sequence.push_back(type);
    }
    const std::vector<std::string> expected{
        "message_start", "content_block_start:thinking", "thinking_delta", "thinking_delta", "signature_delta",
        "content_block_stop", "content_block_start:text", "text_delta", "text_delta", "content_block_stop",
        "content_block_start:tool_use", "input_json_delta", "content_block_stop", "message_delta", "message_stop"};
    CHECK(sequence == expected);
    CHECK(events.back().at("type") == "message_stop");
    auto streamed = replay(events);
    CHECK(streamed == anthropic_response(aggregate, "gemini-test", "msg_stream", true));
    CHECK(streamed.at("stop_reason") == "tool_use");
    CHECK(streamed.at("content").as_array()[1].at("text") == "Hello world");
    CHECK(stream.fail(Error(503, "no_account", "none")).at(0) == parse(R"({"type":"error","error":{"type":"overloaded_error","message":"none"}})"));
}

TEST_CASE("Without client thinking a text signature has no carrier but call signatures stay", "[anthropic]") {
    const auto gemini = chunk(json::array{json::object{{"text", "Hi"}, {"thoughtSignature", "s1"}},
                                          parse(R"({"functionCall":{"name":"f","args":{}},"thoughtSignature":"s2"})")},
                              "STOP");
    auto message = anthropic_response(gemini, "m", "msg", false);
    const auto &content = message.at("content").as_array();
    REQUIRE(content.size() == 2);
    CHECK(content[0] == parse(R"({"type":"text","text":"Hi"})"));
    CHECK(client_call(std::string(content[1].at("id").as_string())).signature == "s2");
    // Stop reasons.
    CHECK(anthropic_response(chunk(json::array{json::object{{"text", "x"}}}, "MAX_TOKENS"), "m", "msg", false).at("stop_reason") == "max_tokens");
    CHECK(anthropic_response(chunk(json::array{}, "SAFETY"), "m", "msg", false).at("stop_reason") == "refusal");
    CHECK(anthropic_response(chunk(json::array{json::object{{"text", "x"}}}, "STOP"), "m", "msg", false).at("stop_reason") == "end_turn");
    std::size_t dropped = 0;
    (void)anthropic_response(chunk(json::array{parse(R"({"inlineData":{"mimeType":"image/png","data":"AA=="}})")}), "m", "msg", false, &dropped);
    CHECK(dropped == 1);
}

TEST_CASE("A signature after visible content opens no trailing thinking block", "[anthropic]") {
    // Gemini sends a text answer's signature last; the answer stays the last block.
    auto late = anthropic_response(chunk(json::array{json::object{{"text", "planning"}, {"thought", true}},
                                                     json::object{{"text", "answer"}},
                                                     json::object{{"text", ""}, {"thoughtSignature", "late"}}},
                                         "STOP"),
                                   "m", "msg", true);
    const auto &blocks = late.at("content").as_array();
    REQUIRE(blocks.size() == 2);
    CHECK(blocks[0] == parse(R"({"type":"thinking","thinking":"planning","signature":""})"));
    CHECK(blocks[1] == parse(R"({"type":"text","text":"answer"})"));
    // Before any visible block, an empty thinking block still carries it.
    auto early = anthropic_response(chunk(json::array{json::object{{"text", "x"}, {"thoughtSignature", "early"}}}, "STOP"),
                                    "m", "msg", true);
    CHECK(early.at("content") == json::parse(R"([{"type":"thinking","thinking":"","signature":"ais2api.gemini.early"},{"type":"text","text":"x"}])"));
}

TEST_CASE("Anthropic errors use the API's error types", "[anthropic]") {
    for (auto [status, type] : {std::pair{400, "invalid_request_error"}, {401, "authentication_error"}, {403, "permission_error"},
                                {404, "not_found_error"}, {413, "request_too_large"}, {429, "rate_limit_error"},
                                {503, "overloaded_error"}, {504, "timeout_error"}, {502, "api_error"}}) {
        auto body = anthropic_error(Error(status, "code", "message"));
        CHECK(body.at("type") == "error");
        CHECK(body.at("error").at("type") == type);
        CHECK(body.at("error").at("message") == "message");
    }
}

TEST_CASE("count_tokens estimates text, tools and media locally", "[anthropic][tokenizer]") {
    auto count = [](std::string_view text) {
        return anthropic_input_tokens(anthropic_to_gemini(parse(text), false, true).gemini);
    };
    CHECK(count(R"({"model":"m","messages":[{"role":"user","content":"Hello, world!"}]})") == count_tokens("Hello, world!"));
    CHECK(count(R"({"model":"m","system":"Be brief.","messages":[{"role":"user","content":"Hello, world!"}]})") ==
          count_tokens("Hello, world!") + count_tokens("Be brief."));
    const auto image = count(R"({"model":"m","messages":[{"role":"user","content":[{"type":"text","text":"Hello, world!"},
        {"type":"image","source":{"type":"base64","media_type":"image/png","data":"iVBORw0KGgo="}}]}]})");
    CHECK(image == count_tokens("Hello, world!") + 1120);
    // Two page objects; the page tree does not count.
    const auto pdf = count(R"({"model":"m","messages":[{"role":"user","content":[{"type":"document","source":{"type":"base64",
        "media_type":"application/pdf","data":"JVBERi0xLjQKMSAwIG9iaiA8PCAvVHlwZSAvUGFnZXMgL0NvdW50IDIgPj4KMiAwIG9iaiA8PCAvVHlwZSAvUGFnZSA+PgozIDAgb2JqIDw8IC9UeXBlL1BhZ2UgPj4K"}}]}]})");
    CHECK(pdf == 1120);
    const auto tool = count(R"({"model":"m","tools":[{"name":"lookup","input_schema":{"type":"object"}}],
        "messages":[{"role":"user","content":"Hello, world!"}]})");
    CHECK(tool == count_tokens("Hello, world!") + count_tokens(R"({"name":"lookup","parameters":{"type":"object"}})"));
}
