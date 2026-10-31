// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace ais2api;
namespace {
json::object obj(std::string_view text) { return json::parse(text).as_object(); }
json::array one(json::value value) {
    json::array a;
    a.push_back(std::move(value));
    return a;
}
json::value response_wire(json::array parts, json::value finish = nullptr) {
    json::array candidate{json::array{parts, "model"}, finish};
    json::array chunk{one(candidate), nullptr,
                      json::array{10, 2, 15, 1, nullptr, nullptr, nullptr, nullptr, nullptr, 3}};
    chunk.resize(8);
    chunk[7] = "r1";
    return chunk;
}
} // namespace

TEST_CASE("Gemini normalization preserves payloads and rejects extension conflicts", "[protocol]") {
    Config cfg;
    cfg.model_aliases["short"] = "gemini-test";
    auto request = normalize_gemini(
        "short",
        obj(R"({"contents":[{"role":"user","parts":[{"text":"hello"}]}],"ais2api":{"gemini":{"generationConfig":{"temperature":0.4}}}})"),
        true, cfg);
    REQUIRE(request.model == "gemini-test");
    REQUIRE(request.stream);
    REQUIRE(request.gemini.at("generationConfig").as_object().at("temperature") == 0.4);
    REQUIRE_THROWS_AS(
        normalize_gemini(
            "x",
            obj(R"({"contents":[{"parts":[{"text":"x"}]}],"generationConfig":{"temperature":1},"ais2api":{"gemini":{"generationConfig":{"temperature":1}}}})"),
            false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_gemini("x",
                         obj(R"({"contents":[{"parts":[{"fileData":{"fileUri":"files/a"}}]}]})"),
                         false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_gemini(
            "x",
            obj(R"({"contents":[{"parts":[{"text":"x","inlineData":{"mimeType":"image/png","data":"AA=="}}]}]})"),
            false, cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_gemini("x", obj(R"({"contents":[{"parts":[{"text":"x"}]}],"extra_body":{}})"),
                         false, cfg),
        Error);
}

TEST_CASE("JSPB contents contain typed Struct values and individual signatures", "[protocol]") {
    auto contents =
        json::parse(
            R"([{"role":"model","parts":[{"text":"a","thoughtSignature":"s1"},{"inlineData":{"mimeType":"image/png","data":"AA=="},"thoughtSignature":"s2"},{"functionCall":{"id":"c1","name":"calculate","args":{"enabled":true,"empty":[],"nested":{"n":2},"nil":null,"one":["a"]}},"thoughtSignature":"s3"}]}])")
            .as_array();
    auto encoded = encode_contents(contents);
    auto &parts = encoded[0].as_array()[0].as_array();
    REQUIRE(parts[0].as_array()[14] == "s1");
    REQUIRE(parts[1].as_array()[14] == "s2");
    // The response protocol places FunctionCall at slot 3, unlike request slot 10.
    json::array response_parts{parts[0], parts[1]};
    auto function = parts[2].as_array();
    function[3] = function[10];
    function[10] = nullptr;
    response_parts.push_back(function);
    auto decoded = decode_chunk(response_wire(response_parts, 1));
    auto &returned = decoded.at("candidates")
                         .as_array()[0]
                         .as_object()
                         .at("content")
                         .as_object()
                         .at("parts")
                         .as_array();
    REQUIRE(returned[0].as_object().at("thoughtSignature") == "s1");
    REQUIRE(returned[1].as_object().at("inlineData") ==
            contents[0].as_object().at("parts").as_array()[1].as_object().at("inlineData"));
    REQUIRE(returned[2].as_object().at("functionCall") ==
            contents[0].as_object().at("parts").as_array()[2].as_object().at("functionCall"));
    REQUIRE(decoded.at("usageMetadata").as_object().at("candidatesTokenCount") == 5);
    REQUIRE(decoded.at("usageMetadata").as_object().at("thoughtsTokenCount") == 3);
}

TEST_CASE("Schemas encode recursive properties union and constraints without metadata injection",
          "[protocol]") {
    auto s = encode_schema(obj(
        R"({"type":"object","properties":{"items":{"type":"array","items":{"type":["string","null"],"enum":["yes","no"]},"minItems":1},"count":{"type":"integer","minimum":0,"maximum":9}},"required":["items"],"propertyOrdering":["items","count"]})"));
    REQUIRE(s[0] == 6);
    REQUIRE(s.size() == 23);
    auto &entries = s[6].as_array();
    auto &item = entries[0].as_array()[1].as_array();
    REQUIRE(item[0] == 5);
    REQUIRE(item[5].as_array()[3] == true);
    REQUIRE(item[21] == 1);
    auto union_schema = encode_schema(obj(
        R"({"type":"string","oneOf":[{"type":"string","enum":["a"]},{"type":"string","enum":["b"]}],"example":"a"})"));
    REQUIRE(union_schema[16].as_array().size() == 2);
    REQUIRE(union_schema[15].as_array()[2] == "a");
    auto closed = encode_schema(obj(R"({"type":"object","additionalProperties":false})"));
    REQUIRE(closed[0] == 6);
    REQUIRE(closed[2].as_string().find("Do not include") != json::string::npos);
    auto title = encode_schema(obj(R"({"type":"string","title":"Label","default":"yes"})"));
    REQUIRE(title[0] == 1);
    REQUIRE(title[2].as_string().find("Label") != json::string::npos);
}

TEST_CASE("Client schemas lower nested metadata maps and local references without new wire fields", "[protocol][schema]") {
    auto encoded = encode_schema(obj(R"({
        "$schema":"https://json-schema.org/draft/2020-12/schema",
        "$defs":{"a/b~c":{"type":"string","title":"Query","default":"hello"}},
        "type":"object","additionalProperties":false,
        "properties":{
            "additionalProperties":{"$ref":"#/$defs/a~1b~0c"},
            "options":{"type":"object","additionalProperties":{"type":"integer"}},
            "enabled":{"type":"boolean","enum":[true,false]},
            "kind":{"type":"string","const":"lookup"},
            "list":{"type":"array","items":{"$ref":"#/$defs/a~1b~0c"}}
        },"required":["additionalProperties"]})"));
    REQUIRE(encoded[0] == 6);
    REQUIRE(encoded.size() <= 23);
    const auto &props = encoded[6].as_array();
    REQUIRE(props[0].as_array()[0] == "additionalProperties");
    REQUIRE(props[0].as_array()[1].as_array()[0] == 1);
    REQUIRE(props[1].as_array()[1].as_array()[2].as_string().find("integer") != json::string::npos);
    REQUIRE(props[2].as_array()[1].as_array()[2].as_string().find("true,false") != json::string::npos);
    REQUIRE(props[3].as_array()[1].as_array()[4] == json::array{"lookup"});
    REQUIRE(props[4].as_array()[1].as_array()[5] == props[0].as_array()[1]);
    REQUIRE(encode_schema(obj(R"({"type":"object","additionalProperties":true})")) == json::array{6});
    REQUIRE(encode_schema(obj(R"({"definitions":{"T":{"type":"integer"}},"$ref":"#/definitions/T"})")) == json::array{3});
    REQUIRE(encode_schema(obj(R"({"const":"lookup"})"))[4] == json::array{"lookup"});
    REQUIRE(encode_schema(obj(R"({"enum":["a","b"]})"))[0] == 1);
}

TEST_CASE("Schema expansion rejects unresolved recursive remote and conflicting references", "[protocol][schema]") {
    for (auto source : {
        R"({"$ref":"#"})",
        R"({"$ref":"#/$defs/missing"})",
        R"({"$ref":"https://example.com/schema.json"})",
        R"({"$defs":{"T":{"type":"string"}},"$ref":"#/$defs/T","type":"integer"})",
        R"({"type":"object","additionalProperties":7})",
        R"({"type":"string","minLength":-1})",
        R"({"type":"object","properties":{"x":{"type":"string","unknownConstraint":true}}})"
    }) REQUIRE_THROWS_AS(encode_schema(obj(source)), Error);
    json::object nested{{"type", "string"}};
    for (unsigned i = 0; i < 66; ++i) nested = json::object{{"type", "array"}, {"items", nested}};
    REQUIRE_THROWS_AS(encode_schema(nested), Error);
    json::object properties;
    for (unsigned i = 0; i < 100; ++i)
        properties["p" + std::to_string(i)] = json::object{{"$ref", "#/$defs/T"}};
    auto expanded = json::object{{"type", "object"}, {"properties", properties},
        {"$defs", json::object{{"T", json::object{{"type", "string"}, {"description", std::string(128 * 1024, 'x')}}}}}};
    REQUIRE_THROWS_AS(encode_schema(expanded), Error);
}

TEST_CASE("Cherry style tool schemas normalize in Chat and Gemini and record keyword downgrades", "[protocol][schema]") {
    auto schema = obj(R"({"type":"object","title":"Search","properties":{"query":{"type":"string","default":"hi"}},"additionalProperties":false})");
    auto chat = obj(R"({"model":"gemini-test","messages":[{"role":"user","content":"find it"}],"tools":[{"type":"function","function":{"name":"search","parameters":{}}}]})");
    chat.at("tools").as_array()[0].as_object().at("function").as_object()["parameters"] = schema;
    auto request = normalize_openai(chat, Config{});
    REQUIRE(request.ignored_parameters.size() == 3);
    auto wire = encode_request(request, {}, "s");
    auto parameters = wire[6].as_array()[0].as_array()[1].as_array()[0].as_array()[2];
    REQUIRE(parameters.as_array()[0] == 6);
    REQUIRE(parameters.as_array()[6].as_array()[0].as_array()[0] == "query");
    auto native = obj(R"({"contents":[{"parts":[{"text":"hi"}]}],"tools":[{"functionDeclarations":[{"name":"search","parametersJsonSchema":{}}]}],"generation_config":{"response_json_schema":{}}})");
    native.at("tools").as_array()[0].as_object().at("functionDeclarations").as_array()[0].as_object()["parametersJsonSchema"] = schema;
    native.at("generation_config").as_object()["response_json_schema"] = schema;
    request = normalize_gemini("gemini-test", native, false, Config{});
    REQUIRE(request.ignored_parameters.size() == 6);
    wire = encode_request(request, {}, "s");
    REQUIRE(wire[3].as_array()[8] == parameters);
    auto invalid_chat = chat;
    invalid_chat["parallel_tool_calls"] = false;
    REQUIRE_THROWS_AS(normalize_openai(invalid_chat, Config{}), Error);
    invalid_chat["parallel_tool_calls"] = true;
    REQUIRE_NOTHROW(normalize_openai(invalid_chat, Config{}));
    auto &function = invalid_chat.at("tools").as_array()[0].as_object().at("function").as_object();
    function["strict"] = true;
    REQUIRE_THROWS_AS(normalize_openai(invalid_chat, Config{}), Error);
}

TEST_CASE("Request encoding replaces stale template generation fields", "[protocol]") {
    Config cfg;
    auto r = normalize_gemini(
        "gemini-test",
        obj(R"({"contents":[{"parts":[{"text":"new"}]}],"generationConfig":{"maxOutputTokens":19,"thinkingConfig":{"thinkingLevel":"LOW"}},"tools":[{"functionDeclarations":[{"name":"alpha","parametersJsonSchema":{"type":"object"}},{"name":"beta"}]}],"toolConfig":{"functionCallingConfig":{"mode":"AUTO","allowedFunctionNames":["alpha"]}}})"),
        false, cfg);
    json::array base;
    base.resize(14);
    base[5] = "stale";
    base[6] = "stale";
    base[11] = "stale cache";
    base[13] = json::array{1};
    auto wire = encode_request(r, base, "!fresh");
    REQUIRE(wire[0] == "models/gemini-test");
    REQUIRE(wire[4] == "!fresh");
    REQUIRE(wire[5].is_null());
    REQUIRE(wire[11].is_null());
    REQUIRE(wire[13] == json::parse(R"([[null,null,"UTC"],null,1])"));
    REQUIRE(wire[3].as_array()[3] == 19);
    REQUIRE(wire[3].as_array()[16].as_array()[3] == 1);
    REQUIRE(wire[6].as_array()[0].as_array()[1].as_array().size() == 1);
}

TEST_CASE("Model family defaults are explicit and do not inherit captured sampling", "[protocol]") {
    auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}]})");
    json::array base;
    base.resize(14);
    base[3] = json::parse("[null,null,null,999,0.9,0.8,40]");
    base[6] = json::parse("[[[]]]");
    base[7] = "stale-model";
    base[11] = "stale-cache";
    base[13] = json::array{1};
    for (auto model : {"gemini-text", "gemma-text", "other-model"}) {
        auto wire = encode_request(normalize_gemini(model, body, false, Config{}), base, "s");
        auto &generation = wire[3].as_array();
        REQUIRE(generation[16] == json::array{1, nullptr, nullptr, 3});
        for (auto index : {3, 4, 5, 6}) REQUIRE(generation[index].is_null());
        if (std::string_view(model) == "other-model") REQUIRE(wire[2].is_null());
        else REQUIRE(wire[2] == json::parse("[[null,null,7,5],[null,null,8,5],[null,null,9,5],[null,null,10,5]]"));
        for (auto index : {6, 7, 11, 13})
            REQUIRE((wire.size() <= static_cast<std::size_t>(index) || wire[index].is_null()));
    }
    auto explicit_safety = body;
    explicit_safety["safetySettings"] = json::parse(R"([{"category":"HARM_CATEGORY_HARASSMENT","threshold":"BLOCK_ONLY_HIGH"}])");
    auto wire = encode_request(normalize_gemini("gemini-text", explicit_safety, false, Config{}), {}, "s");
    REQUIRE(wire[2] == json::parse("[[null,null,7,3]]"));
    explicit_safety["safetySettings"] = json::array{};
    REQUIRE(encode_request(normalize_gemini("gemma-text", explicit_safety, false, Config{}), {}, "s")[2] == json::array{});
}

TEST_CASE("Exact model defaults fill generation and thinking fields without overriding requests", "[protocol]") {
    Config config;
    config.model_defaults = obj(R"({"gemini-custom":{"generation_config":{"temperature":0.7,"top_p":0.8,"thinking_config":{"thinking_level":"LOW","include_thoughts":false,"thinking_budget":100}},"safetySettings":[]}})");
    auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}],"generationConfig":{"temperature":0.2,"thinkingConfig":{"thinkingLevel":"MEDIUM","includeThoughts":true}}})");
    auto request = normalize_gemini("gemini-custom", body, false, config);
    REQUIRE(request.include_thoughts);
    REQUIRE(request.ignored_parameters == json::array{"generationConfig.thinkingConfig.thinkingBudget"});
    auto wire = encode_request(request, {}, "s");
    REQUIRE(wire[3].as_array()[4] == 0.2);
    REQUIRE(wire[3].as_array()[5] == 0.8);
    REQUIRE(wire[3].as_array()[16] == json::array{1, nullptr, nullptr, 2});
    REQUIRE(wire[2] == json::array{});
    body["generationConfig"] = json::object{{"thinkingConfig", json::object{{"includeThoughts", true}}}};
    request = normalize_gemini("gemini-custom", body, false, config);
    REQUIRE(request.include_thoughts);
    REQUIRE(encode_request(request, {}, "s")[3].as_array()[16] == json::array{1, nullptr, nullptr, 1});
}

TEST_CASE("Image wire defaults and cleanup follow the model profile", "[protocol]") {
    auto body = obj(R"({"contents":[{"parts":[{"text":"draw"}]}]})");
    auto wire = encode_request(normalize_gemini("gemini-image", body, false, Config{}), {}, "s");
    REQUIRE(wire[2].is_null());
    REQUIRE(wire[3].as_array()[14] == json::array{2});
    REQUIRE(wire[3].as_array()[16] == json::array{1, nullptr, nullptr, 4});
    body["generationConfig"] = json::parse(R"({"temperature":0.3,"responseMimeType":"application/json","mediaResolution":"MEDIA_RESOLUTION_HIGH","responseModalities":["TEXT","IMAGE"],"thinkingConfig":{"thinkingLevel":"HIGH","includeThoughts":false,"thinkingBudget":0},"imageConfig":{"imageSize":"2K"}})");
    body["safetySettings"] = json::parse(R"([{"category":"HARM_CATEGORY_HARASSMENT","threshold":"OFF"}])");
    auto request = normalize_gemini("gemini-image", body, false, Config{});
    REQUIRE_FALSE(request.include_thoughts);
    REQUIRE(request.ignored_parameters == json::array{"generationConfig.thinkingConfig.thinkingBudget"});
    wire = encode_request(request, {}, "s");
    REQUIRE(wire[2].is_null());
    auto &generation = wire[3].as_array();
    REQUIRE(generation[4] == 0.3);
    for (auto index : {7, 13, 17}) REQUIRE(generation[index].is_null());
    REQUIRE(generation[14] == json::array{2, 1});
    REQUIRE(generation[16] == json::array{1, nullptr, nullptr, 3});
    REQUIRE(generation[26] == json::array{nullptr, "2K"});
}

TEST_CASE("Wire parser tolerates arbitrary byte splits and bounds the current chunk",
          "[protocol]") {
    auto chunk = response_wire(one(json::array{nullptr, "snow 雪 and \"quoted\" [ ]"}), 1);
    auto bytes =
        std::string(")]}'\n[[") + json::serialize(chunk) + "," + json::serialize(chunk) + "]]";
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        WireStreamParser parser(4096);
        std::vector<json::value> result;
        auto emit = [&](json::value v) { result.push_back(std::move(v)); };
        parser.feed(std::string_view(bytes).substr(0, split), emit);
        parser.feed(std::string_view(bytes).substr(split), emit);
        parser.finish();
        REQUIRE(result.size() == 2);
        REQUIRE(result[0] == chunk);
        REQUIRE(parser.buffered_bytes() == 0);
    }
    WireStreamParser bytewise;
    std::size_t count = 0;
    for (char c : bytes)
        bytewise.feed(std::string_view(&c, 1), [&](json::value) { ++count; });
    bytewise.finish();
    REQUIRE(count == 2);
    WireStreamParser bounded(16);
    REQUIRE_THROWS_AS(bounded.feed(bytes, [](json::value) {}), Error);
    WireStreamParser truncated;
    truncated.feed("[[[null,", [](json::value) {});
    REQUIRE_THROWS_AS(truncated.finish(), Error);
    WireStreamParser malformed;
    REQUIRE_THROWS_AS(malformed.feed("[[[],]]", [](json::value) {}), Error);
    WireStreamParser invalid_utf8;
    std::string bad = "[[[\"";
    bad.push_back(static_cast<char>(0xff));
    bad += "\"]]]";
    REQUIRE_THROWS_AS(invalid_utf8.feed(bad, [](json::value) {}), Error);
}

TEST_CASE("Wire parser emits complete chunks before the enclosing response ends", "[protocol][stream-timing]") {
    auto first = response_wire(one(json::array{nullptr, "first"}), 1);
    auto second = response_wire(one(json::array{nullptr, "second"}), 1);
    WireStreamParser parser;
    std::vector<json::value> received;
    auto emit = [&](json::value value) { received.push_back(std::move(value)); };
    parser.feed(std::string(")]}'\n[[") + json::serialize(first), emit);
    REQUIRE(received.size() == 1);
    REQUIRE(received.front() == first);
    parser.feed("," + json::serialize(second), emit);
    REQUIRE(received.size() == 2);
    REQUIRE(received.back() == second);
    parser.feed("]]", emit);
    parser.finish();
    REQUIRE(received.size() == 2);
}

TEST_CASE("Content gate ignores metadata and merge preserves part boundaries", "[protocol]") {
    json::object aggregate;
    json::object metadata{{"responseId", "id"},
                          {"usageMetadata", json::object{{"promptTokenCount", 2}}}};
    REQUIRE_FALSE(has_content(metadata));
    auto a = decode_chunk(response_wire(one(json::array{nullptr, "one"})));
    auto b = decode_chunk(response_wire(one(json::array{nullptr, "two"}), 1));
    REQUIRE(has_content(a));
    merge_event(aggregate, a, 4096);
    merge_event(aggregate, b, 4096);
    auto &parts = aggregate.at("candidates")
                      .as_array()[0]
                      .as_object()
                      .at("content")
                      .as_object()
                      .at("parts")
                      .as_array();
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0].as_object().at("text") == "onetwo");
    auto before = aggregate;
    REQUIRE_THROWS_AS(merge_event(aggregate, a, 4), Error);
    REQUIRE(aggregate == before);
}

TEST_CASE("OpenAI standard multimodal and tool messages normalize without wire aliases",
          "[protocol]") {
    Config cfg;
    auto r = normalize_openai(
        obj(R"({"model":"m","messages":[{"role":"system","content":"rules"},{"role":"user","content":[{"type":"text","text":"look"},{"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]},{"role":"assistant","content":null,"tool_calls":[{"id":"c1","type":"function","function":{"name":"f","arguments":"{\"a\":true}"}}]},{"role":"tool","tool_call_id":"c1","content":"{\"ok\":true}"}],"tools":[{"type":"function","function":{"name":"f","parameters":{"type":"object"}}}],"tool_choice":"auto","stream":true,"stream_options":{"include_usage":true},"ais2api":{"gemini":{"generationConfig":{"thinkingConfig":{"thinkingLevel":"HIGH"}}}}})"),
        cfg);
    REQUIRE(r.protocol == Protocol::openai);
    REQUIRE(r.stream);
    REQUIRE(r.include_usage);
    auto &contents = r.gemini.at("contents").as_array();
    REQUIRE(contents.size() == 3);
    REQUIRE(contents[0].as_object().at("parts").as_array().size() == 2);
    REQUIRE(contents[2]
                .as_object()
                .at("parts")
                .as_array()[0]
                .as_object()
                .at("functionResponse")
                .as_object()
                .at("name") == "f");
    REQUIRE_THROWS_AS(
        normalize_openai(
            obj(R"({"model":"m","messages":[{"role":"user","content":"x"}],"temperature":1,"ais2api":{"gemini":{"generationConfig":{"temperature":1}}}})"),
            cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_openai(obj(R"({"model":"m","messages":[{"role":"user","content":"x"}],"n":2})"),
                         cfg),
        Error);
}

TEST_CASE("OpenAI rich assistant roundtrip retains native image and thought signature",
          "[protocol]") {
    Config cfg;
    json::array text{nullptr, "answer"};
    text.resize(15);
    text[14] = "signed";
    auto gemini = decode_chunk(response_wire(
        json::array{text, json::array{nullptr, nullptr, json::array{"image/png", "AA=="}}}, 1));
    auto result = openai_response(gemini, "m", "chat-1");
    auto message = result.at("choices").as_array()[0].as_object().at("message");
    REQUIRE(message.as_object().at("content") == "answer");
    REQUIRE(message.as_object().at("ais2api").as_object().at("gemini_content").is_object());
    json::object request{
        {"model", "m"},
        {"messages",
         json::array{json::object{{"role", "user"}, {"content", "question"}}, message}}};
    auto normalized = normalize_openai(request, cfg);
    auto &parts = normalized.gemini.at("contents").as_array()[1].as_object().at("parts").as_array();
    REQUIRE(parts[0].as_object().at("thoughtSignature") == "signed");
    REQUIRE(parts.size() == 2);
    auto delta = openai_delta(gemini, "m", "chat-1");
    REQUIRE(delta.at("choices")
                .as_array()[0]
                .as_object()
                .at("delta")
                .as_object()
                .at("ais2api")
                .as_object()
                .at("gemini_delta") == gemini);
}

TEST_CASE("Compressed boolean and wrapped Struct lists decode without collapsing singleton values",
          "[protocol]") {
    json::array bool_value{nullptr, nullptr, nullptr, 1};
    json::array element{nullptr, nullptr, "single"};
    json::array list_value{nullptr, nullptr, nullptr, nullptr, nullptr, one(one(element))};
    json::array fields{json::array{"flag", bool_value}, json::array{"list", list_value}};
    json::array function{"f", one(one(fields)), "id"};
    json::array p{nullptr, nullptr, nullptr, function};
    auto decoded = decode_chunk(response_wire(one(p)));
    auto &args = decoded.at("candidates")
                     .as_array()[0]
                     .as_object()
                     .at("content")
                     .as_object()
                     .at("parts")
                     .as_array()[0]
                     .as_object()
                     .at("functionCall")
                     .as_object()
                     .at("args")
                     .as_object();
    REQUIRE(args.at("flag") == true);
    REQUIRE(args.at("list") == json::array{"single"});
}

TEST_CASE("OpenAI tools retain independent ids and native signatures through aggregation",
          "[protocol]") {
    json::array first{nullptr, nullptr, nullptr, json::array{"left", json::array{}, "left-id"}};
    first.resize(15);
    first[14] = "left-signature";
    json::array second{nullptr, nullptr, nullptr, json::array{"right", json::array{}, "right-id"}};
    second.resize(15);
    second[14] = "right-signature";
    auto event = decode_chunk(response_wire(json::array{first, second}, 1));
    auto response = openai_response(event, "m", "reply");
    auto &msg = response.at("choices").as_array()[0].as_object().at("message").as_object();
    auto &calls = msg.at("tool_calls").as_array();
    REQUIRE(calls.size() == 2);
    REQUIRE(string_or(calls[0].as_object(), "id").starts_with("call_gemini_"));
    REQUIRE(string_or(calls[1].as_object(), "id").starts_with("call_gemini_"));
    auto roundtrip =
        normalize_openai(json::object{{"model", "m"}, {"messages", json::array{msg}}}, Config{});
    auto &parts = roundtrip.gemini.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0].as_object().at("thoughtSignature") == "left-signature");
    REQUIRE(parts[1].as_object().at("thoughtSignature") == "right-signature");
    REQUIRE(parts[0].as_object().at("functionCall").as_object().at("id") == "left-id");
    REQUIRE(parts[1].as_object().at("functionCall").as_object().at("id") == "right-id");
    auto delta = openai_delta(event, "m", "reply");
    auto &streamed = delta.at("choices")
                         .as_array()[0]
                         .as_object()
                         .at("delta")
                         .as_object()
                         .at("tool_calls")
                         .as_array();
    REQUIRE(streamed[0].as_object().at("index") == 0);
    REQUIRE(streamed[1].as_object().at("index") == 1);
}

TEST_CASE("Finish and usage convert to standard OpenAI semantics", "[protocol]") {
    auto event = decode_chunk(response_wire(one(json::array{nullptr, "partial"}), 2));
    auto response = openai_response(event, "m", "r");
    REQUIRE(response.at("choices").as_array()[0].as_object().at("finish_reason") == "length");
    auto &usage = response.at("usage").as_object();
    REQUIRE(usage.at("prompt_tokens") == 10);
    REQUIRE(usage.at("completion_tokens") == 5);
    REQUIRE(usage.at("completion_tokens_details").as_object().at("reasoning_tokens") == 3);
    auto blocked = openai_response(decode_chunk(response_wire({}, 3)), "m", "r");
    REQUIRE(blocked.at("choices").as_array()[0].as_object().at("finish_reason") ==
            "content_filter");
    auto nullable = encode_schema(obj(R"({"anyOf":[{"type":"null"},{"type":"string"}]})"));
    REQUIRE(nullable[0] == 1);
    REQUIRE(nullable[3] == true);
    REQUIRE(nullable[17].as_array().size() == 1);
}

TEST_CASE("Tool registry maintains stream indices while signatures roundtrip statelessly",
          "[protocol]") {
    struct Fixture {
        fs::path directory = fs::temp_directory_path() / ("ais2api-protocol-" + random_hex(10));
        std::unique_ptr<Store> store = std::make_unique<Store>(directory);
        ~Fixture() {
            store.reset();
            std::error_code ec;
            fs::remove_all(directory, ec);
        }
    } fixture;
    auto &store = *fixture.store;
    json::array first{nullptr, nullptr, nullptr, json::array{"f", json::array{}, "call-a"}};
    first.resize(15);
    first[14] = "sig-a";
    json::array second{nullptr, nullptr, nullptr, json::array{"g", json::array{}, "call-b"}};
    second.resize(15);
    second[14] = "sig-b";
    auto a =
        openai_delta(decode_chunk(response_wire(one(first))), "m", "response", &store, "key-one");
    auto b =
        openai_delta(decode_chunk(response_wire(one(second))), "m", "response", &store, "key-one");
    auto index = [](const json::object &delta) {
        return delta.at("choices")
            .as_array()[0]
            .as_object()
            .at("delta")
            .as_object()
            .at("tool_calls")
            .as_array()[0]
            .as_object()
            .at("index");
    };
    REQUIRE(index(a) == 0);
    REQUIRE(index(b) == 1);
    auto repeat =
        openai_delta(decode_chunk(response_wire(one(first))), "m", "response", &store, "key-one");
    REQUIRE(index(repeat) == 0);
    REQUIRE(store.get_tool("key-one", "call-a").has_value());
    REQUIRE_FALSE(store.get_tool("key-one", "call-a")->if_contains("thoughtSignature"));
    REQUIRE_FALSE(store.get_tool("key-two", "call-a").has_value());
    auto finish =
        openai_delta(decode_chunk(response_wire({}, 1)), "m", "response", &store, "key-one");
    REQUIRE(finish.at("choices").as_array()[0].as_object().at("finish_reason") == "tool_calls");
    Config cfg;
    auto request = obj(
        R"({"model":"m","messages":[{"role":"assistant","content":null,"tool_calls":[{"id":"call-a","type":"function","function":{"name":"f","arguments":"{}"}}]}]})");
    auto raw = normalize_openai(request, cfg, &store, "key-one");
    REQUIRE_FALSE(raw.gemini.at("contents").as_array()[0].as_object().at("parts")
                      .as_array()[0].as_object().if_contains("thoughtSignature"));
    auto public_call = a.at("choices").as_array()[0].as_object().at("delta")
                           .as_object().at("tool_calls").as_array()[0].as_object();
    public_call.erase("index");
    REQUIRE(string_or(public_call, "id").starts_with("call_gemini_"));
    request.at("messages").as_array()[0].as_object()["tool_calls"] = json::array{public_call};
    auto restored = normalize_openai(request, cfg, &store, "key-one");
    auto isolated = normalize_openai(request, cfg, &store, "key-two");
    REQUIRE(restored.gemini == isolated.gemini);
    auto &part = restored.gemini.at("contents").as_array()[0].as_object().at("parts")
                     .as_array()[0].as_object();
    REQUIRE(part.at("thoughtSignature") == "sig-a");
    REQUIRE(part.at("functionCall").as_object().at("id") == "call-a");
}

TEST_CASE("Gemini client controls validate without inventing upstream fields", "[protocol]") {
    auto request = normalize_gemini("m", obj(R"({"contents":[{"parts":[{"text":"x"}]}],
        "generationConfig":{"candidateCount":1,"seed":42,"responseModalities":["text","image"],
        "thinkingConfig":{"thinkingBudget":0,"includeThoughts":false,"thinkingLevel":"low"}}})"), false, Config{});
    REQUIRE_FALSE(request.include_thoughts);
    REQUIRE(request.ignored_parameters == json::array{"generationConfig.seed", "generationConfig.thinkingConfig.thinkingBudget"});
    auto wire = encode_request(request, {}, "!synthetic").at(3).as_array();
    REQUIRE(wire.at(14) == json::array{2, 1});
    REQUIRE(wire.at(16) == json::array{1, nullptr, nullptr, 1});
    for (auto control : {R"({"thinkingBudget":-1})", R"({"includeThoughts":false})"}) {
        auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}]})");
        body["generationConfig"] = json::object{{"thinkingConfig", json::parse(control)}};
        auto encoded = encode_request(normalize_gemini("m", body, false, Config{}), {}, "!synthetic").at(3).as_array();
        REQUIRE(encoded.at(16) == json::array{1, nullptr, nullptr, 3});
    }
    for (auto bad : {R"({"candidateCount":2})", R"({"candidateCount":0})", R"({"candidateCount":1.0})",
                     R"({"seed":"42"})", R"({"thinkingConfig":{"thinkingBudget":-2}})",
                     R"({"thinkingConfig":{"thinkingBudget":0.5}})", R"({"thinkingConfig":{"includeThoughts":"false"}})"}) {
        auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}]})");
        body["generationConfig"] = json::parse(bad);
        REQUIRE_THROWS_AS(normalize_gemini("m", body, false, Config{}), Error);
    }
}

TEST_CASE("Gemini client aliases preserve caller-owned function data", "[protocol]") {
    auto body = obj(R"({"contents":[{"parts":[
        {"inline_data":{"mime_type":"image/png","data":"AA=="}},
        {"function_call":{"name":"f","args":{"thinking_config":{"top_p":2}}},"thought_signature":"sig"},
        {"function_response":{"name":"f","response":{"inline_data":"untouched"}}}]}],
        "system_instruction":{"parts":[{"text":"rules"}]},
        "generation_config":{"max_output_tokens":16,"top_p":0.5,"thinking_config":{"thinking_level":"minimal","include_thoughts":true}},
        "tools":[{"function_declarations":[{"name":"f","parameters":{"type":"object","properties":{"inline_data":{"type":"string"}}}}]},{"google_search":{}}],
        "tool_config":{"function_calling_config":{"mode":"AUTO","allowed_function_names":["f"]}}})");
    auto request = normalize_gemini("m", body, false, Config{});
    auto &parts = request.gemini.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(parts[0].as_object().at("inlineData").as_object().at("mimeType") == "image/png");
    REQUIRE(parts[1].as_object().at("functionCall").as_object().at("args") == obj(R"({"thinking_config":{"top_p":2}})"));
    REQUIRE(parts[2].as_object().at("functionResponse").as_object().at("response") == obj(R"({"inline_data":"untouched"})"));
    auto wire = encode_request(request, {}, "!synthetic");
    REQUIRE(wire.at(6).as_array().size() == 2);
    REQUIRE(wire.at(3).as_array().at(16).as_array().at(3) == 4);
    body["generationConfig"] = json::object{};
    REQUIRE_THROWS_AS(normalize_gemini("m", body, false, Config{}), Error);
}

TEST_CASE("Retired civic filter disabled by clients does not block generation", "[protocol]") {
    auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}],"safetySettings":[
        {"category":"HARM_CATEGORY_HARASSMENT","threshold":"OFF"},
        {"category":"HARM_CATEGORY_HATE_SPEECH","threshold":"OFF"},
        {"category":"HARM_CATEGORY_SEXUALLY_EXPLICIT","threshold":"OFF"},
        {"category":"HARM_CATEGORY_DANGEROUS_CONTENT","threshold":"OFF"},
        {"category":"HARM_CATEGORY_CIVIC_INTEGRITY","threshold":"OFF"}]})");
    auto wire = encode_request(normalize_gemini("m", body, false, Config{}), {}, "!synthetic");
    REQUIRE(wire.at(2) == json::array{json::array{nullptr, nullptr, 7, 5}, json::array{nullptr, nullptr, 8, 5},
                                   json::array{nullptr, nullptr, 9, 5}, json::array{nullptr, nullptr, 10, 5}});
    auto &setting = body.at("safetySettings").as_array().back().as_object();
    setting["threshold"] = "BLOCK_NONE";
    REQUIRE(encode_request(normalize_gemini("m", body, false, Config{}), {}, "!synthetic").at(2).as_array().size() == 4);
    setting["threshold"] = "BLOCK_LOW_AND_ABOVE";
    REQUIRE_THROWS_AS(encode_request(normalize_gemini("m", body, false, Config{}), {}, "!synthetic"), Error);
    setting["category"] = "UNKNOWN_CATEGORY";
    setting["threshold"] = "OFF";
    REQUIRE_THROWS_AS(encode_request(normalize_gemini("m", body, false, Config{}), {}, "!synthetic"), Error);
}

TEST_CASE("Unverified forced execution still fails explicitly", "[protocol]") {
    Config cfg;
    REQUIRE_THROWS_AS(
        normalize_openai(
            obj(R"({"model":"m","messages":[{"role":"user","content":"x"}],"tool_choice":"required"})"),
            cfg),
        Error);
    REQUIRE_THROWS_AS(
        normalize_openai(
            obj(R"({"model":"m","messages":[{"role":"assistant","tool_calls":[{"id":"bad","type":"function"}]}]})"),
            cfg),
        Error);
    auto signed_only = normalize_gemini(
        "m", obj(R"({"contents":[{"role":"model","parts":[{"thoughtSignature":"opaque"}]}]})"),
        false, cfg);
    auto wire = encode_request(signed_only, {}, "!snapshot");
    REQUIRE(wire[1].as_array()[0].as_array()[0].as_array()[0].as_array()[14] == "opaque");
}

TEST_CASE("Thought filtering retains attached signatures and model actions", "[protocol]") {
    auto event = obj(R"({"candidates":[{"content":{"role":"model","parts":[
        {"text":"hidden","thought":true,"thoughtSignature":"keep"},
        {"text":"answer"},
        {"functionCall":{"name":"f","args":{}},"thought":true,"thoughtSignature":"tool"}]}}]})");
    filter_thoughts(event);
    auto &parts = event.at("candidates").as_array()[0].as_object().at("content").as_object().at("parts").as_array();
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0] == json::object{{"text", ""}, {"thoughtSignature", "keep"}});
    REQUIRE(parts[1].as_object().at("text") == "answer");
    REQUIRE(parts[2].as_object().if_contains("functionCall"));
    REQUIRE(parts[2].as_object().at("thoughtSignature") == "tool");
}

TEST_CASE("Native Gemini returns thoughts only on request", "[protocol]") {
    auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}]})");
    REQUIRE_FALSE(normalize_gemini("m", body, false, Config{}).include_thoughts);
    REQUIRE(normalize_openai(obj(R"({"model":"m","messages":[{"role":"user","content":"x"}]})"), Config{})
                .include_thoughts);
    Config config;
    config.model_defaults = obj(R"({"m":{"generationConfig":{"thinkingConfig":{"includeThoughts":true}}}})");
    REQUIRE(normalize_gemini("m", body, false, config).include_thoughts);
    // Upstream is still asked for thoughts so signatures and filtering stay local.
    REQUIRE(encode_request(normalize_gemini("m", body, false, Config{}), {}, "s")[3].as_array()[16] ==
            json::array{1, nullptr, nullptr, 3});
}

TEST_CASE("Default thinking levels and Gemini 4 thinking controls follow the published guide", "[protocol]") {
    auto level = [](std::string model, std::string_view thinking) {
        auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}]})");
        if (!thinking.empty())
            body["generationConfig"] = json::object{{"thinkingConfig", json::parse(thinking)}};
        auto request = normalize_gemini(std::move(model), body, false, Config{});
        return std::pair{encode_request(request, {}, "s")[3].as_array()[16].as_array()[3], request.ignored_parameters};
    };
    for (auto [model, wire] : {std::pair{"gemini-4-argon", 2}, {"Gemini-4-Argon-search", 2}, {"gemini-3.8-flash", 2},
                               {"gemini-3.6-flash", 2}, {"gemini-3.1-flash-lite", 4}, {"gemini-3.5-flash-lite", 4},
                               {"gemini-3-flash", 3}, {"gemini-2.5-pro", 3}, {"gemini-3.1-flash-lite-image", 4}})
        REQUIRE(level(model, "").first == wire);
    REQUIRE(level("gemini-4-argon", R"({"thinkingBudget":0})") == std::pair{json::value(1), json::array{}});
    REQUIRE(level("gemini-4-argon", R"({"thinkingBudget":1024})") ==
            std::pair{json::value(2), json::array{"generationConfig.thinkingConfig.thinkingBudget"}});
    REQUIRE(level("gemini-4-argon", R"({"thinkingLevel":"high"})").first == 3);
    // Older families keep ignoring budgets and accept both fields.
    REQUIRE(level("gemini-3-flash", R"({"thinkingBudget":0,"thinkingLevel":"minimal"})").first == 4);
    for (auto bad : {R"({"thinkingBudget":0,"thinkingLevel":"low"})", R"({"thinkingLevel":"MINIMAL"})"})
        REQUIRE_THROWS_AS(level("gemini-4-argon", bad), Error);
}

TEST_CASE("Gemini 4 output stays within the AI Studio per-request limit", "[protocol]") {
    auto max_tokens = [](std::string model, std::string_view generation) {
        auto body = obj(R"({"contents":[{"parts":[{"text":"x"}]}]})");
        if (!generation.empty()) body["generationConfig"] = json::parse(generation);
        auto request = normalize_gemini(std::move(model), body, false, Config{});
        return std::pair{encode_request(request, {}, "s")[3].as_array()[3], request.ignored_parameters};
    };
    REQUIRE(max_tokens("gemini-4-argon", "") == std::pair{json::value(32768), json::array{}});
    REQUIRE(max_tokens("gemini-4-argon", R"({"maxOutputTokens":8192})") == std::pair{json::value(8192), json::array{}});
    REQUIRE(max_tokens("gemini-4-argon", R"({"maxOutputTokens":32768})").first == 32768);
    for (auto large : {R"({"maxOutputTokens":65536})", R"({"maxOutputTokens":18446744073709551615})"})
        REQUIRE(max_tokens("gemini-4-argon", large) ==
                std::pair{json::value(32768), json::array{"generationConfig.maxOutputTokens"}});
    // Other families keep the upstream default and the caller's value.
    REQUIRE(max_tokens("gemini-3-pro", "").first.is_null());
    REQUIRE(max_tokens("gemini-3-pro", R"({"maxOutputTokens":65536})").first == 65536);
    REQUIRE_THROWS_AS(max_tokens("gemini-4-argon", R"({"maxOutputTokens":-1})"), Error);
    auto chat = normalize_openai(
        obj(R"({"model":"gemini-4-argon","max_tokens":100000,"messages":[{"role":"user","content":"x"}]})"), Config{});
    REQUIRE(encode_request(chat, {}, "s")[3].as_array()[3] == 32768);
}

TEST_CASE("Native usage reports visible output separately from thoughts", "[protocol]") {
    auto event = decode_chunk(json::array{
        nullptr, nullptr, json::array{10, 2, 15, 0, nullptr, nullptr, nullptr, nullptr, nullptr, 3}});
    REQUIRE(event.at("usageMetadata").as_object().at("candidatesTokenCount") == 5);
    official_usage(event);
    REQUIRE(event.at("usageMetadata") ==
            obj(R"({"promptTokenCount":10,"candidatesTokenCount":2,"totalTokenCount":15,"thoughtsTokenCount":3})"));
    auto plain = decode_chunk(json::array{nullptr, nullptr, json::array{4, 1}});
    official_usage(plain);
    REQUIRE(plain.at("usageMetadata") == obj(R"({"promptTokenCount":4,"candidatesTokenCount":1,"totalTokenCount":5})"));
}

TEST_CASE("Finish reasons use the public enum numbers", "[protocol]") {
    auto reason = [](int code) {
        auto event = decode_chunk(json::parse("[[[null," + std::to_string(code) + "]]]"));
        return event.at("candidates").as_array()[0].as_object().at("finishReason");
    };
    for (auto [code, name] : {std::pair{5, "OTHER"}, {6, "LANGUAGE"}, {12, "UNEXPECTED_TOOL_CALL"},
                              {14, "IMAGE_PROHIBITED_CONTENT"}, {15, "IMAGE_OTHER"}, {16, "NO_IMAGE"},
                              {17, "IMAGE_RECITATION"}, {18, "FINISH_REASON_UNSPECIFIED"}})
        REQUIRE(reason(code) == name);
}

TEST_CASE("Executable code enums and function-response media survive request encoding",
          "[protocol]") {
    auto contents =
        json::parse(
            R"json([{"role":"user","parts":[{"functionResponse":{"name":"f","id":"c","response":{"ok":true},"parts":[{"text":"attachment"},{"inlineData":{"mimeType":"image/png","data":"AA=="},"thoughtSignature":"imgsig"}]}}]},{"role":"model","parts":[{"executableCode":{"language":"PYTHON","code":"print(2)"}},{"codeExecutionResult":{"outcome":"OUTCOME_FAILED","output":"failure"}},{"text":"reason","thought":true}]}])json")
            .as_array();
    auto encoded = encode_contents(contents);
    auto &attached = encoded[0].as_array()[0].as_array();
    REQUIRE(attached.size() == 3);
    REQUIRE(attached[2].as_array()[14] == "imgsig");
    auto &generated = encoded[1].as_array()[0].as_array();
    REQUIRE(generated[0].as_array()[7] == json::array{1, "print(2)"});
    REQUIRE(generated[1].as_array()[8] == json::array{2, "failure"});
    REQUIRE(generated[2].as_array()[12] == 1);
    auto decoded = decode_chunk(response_wire(generated));
    auto &result = decoded.at("candidates")
                       .as_array()[0]
                       .as_object()
                       .at("content")
                       .as_object()
                       .at("parts")
                       .as_array();
    REQUIRE(result[0].as_object().at("executableCode").as_object().at("language") == "PYTHON");
    REQUIRE(result[1].as_object().at("codeExecutionResult").as_object().at("outcome") ==
            "OUTCOME_FAILED");
    REQUIRE(result[2].as_object().at("thought") == true);
}

TEST_CASE("Singleton wire containers retain their exact protobuf layers", "[protocol]") {
    auto input =
        json::parse(
            R"([{"role":"model","parts":[{"functionCall":{"name":"f","id":"c","args":{"enabled":true}}}]}])")
            .as_array();
    auto encoded = encode_contents(input);
    auto &function = encoded[0].as_array()[0].as_array()[0].as_array()[10];
    REQUIRE(function == json::parse(R"(["f",[[["enabled",[null,null,null,true]]]],"c"])"));
    auto request = normalize_gemini(
        "m", obj(R"({"contents":[{"parts":[{"text":"x"}]}],"tools":[{"codeExecution":{}}]})"),
        false, Config{});
    auto wire = encode_request(request, {}, "!s");
    REQUIRE(wire[6] == json::parse("[[[]]]"));
    auto response = json::parse(
        R"([ [ [ [ [ [null,"text"] ], "model" ], 2 ] ], null, [1,2,3], null, null, null, null, "r" ])");
    auto decoded = decode_chunk(response);
    REQUIRE(has_content(decoded));
    REQUIRE(decoded.at("candidates").as_array()[0].as_object().at("finishReason") == "MAX_TOKENS");
}
namespace {
struct ToolRegressionStore {
    fs::path directory = fs::temp_directory_path() / ("ais2api-tools-regression-" + random_hex(8));
    std::unique_ptr<Store> store = std::make_unique<Store>(directory);
    ~ToolRegressionStore() {
        store.reset();
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
};
json::object native_tool_event(std::optional<std::string> signature = {}, std::string id = {}) {
    json::object function{{"name", "same_function"}, {"args", {{"value", 7}}}};
    if (!id.empty())
        function["id"] = id;
    json::object part{{"functionCall", function}};
    if (signature)
        part["thoughtSignature"] = *signature;
    return {{"candidates",
             json::array{json::object{
                 {"index", 0}, {"content", {{"role", "model"}, {"parts", json::array{part}}}}}}}};
}
json::object stream_tool(const json::object &chunk) {
    return chunk.at("choices")
        .as_array()[0]
        .as_object()
        .at("delta")
        .as_object()
        .at("tool_calls")
        .as_array()[0]
        .as_object();
}
} // namespace
TEST_CASE("Repeated idless stream functions allocate independent calls and signatures",
          "[protocol][regression]") {
    ToolRegressionStore fixture;
    auto &store = *fixture.store;
    auto original_a = native_tool_event("signature-a");
    auto original_b = native_tool_event("signature-b");
    auto a = openai_delta(original_a, "m", "idless-stream", &store, "owner");
    auto b = openai_delta(original_b, "m", "idless-stream", &store, "owner");
    auto unsigned_call = openai_delta(native_tool_event(), "m", "idless-stream", &store, "owner");
    auto first = stream_tool(a), second = stream_tool(b), third = stream_tool(unsigned_call);
    REQUIRE(first.at("index") == 0);
    REQUIRE(second.at("index") == 1);
    REQUIRE(third.at("index") == 2);
    auto first_id = string_or(first, "id"), second_id = string_or(second, "id"),
         third_id = string_or(third, "id");
    REQUIRE(first_id != second_id);
    REQUIRE(second_id != third_id);
    REQUIRE(first_id != third_id);
    REQUIRE(first_id.starts_with("call_gemini_"));
    REQUIRE(second_id.starts_with("call_gemini_"));
    REQUIRE(third_id.starts_with("call_gemini_"));
    REQUIRE_FALSE(store.get_tool("owner", first_id));
    REQUIRE_FALSE(store.get_tool("owner", second_id));
    REQUIRE_FALSE(store.get_tool("owner", third_id));
    REQUIRE(json::serialize(*store.get_tool("owner", "stream:idless-stream")).find("signature-") == std::string::npos);
    REQUIRE_FALSE(store.get_tool("other-owner", first_id));
    auto &native = a.at("choices")
                       .as_array()[0]
                       .as_object()
                       .at("delta")
                       .as_object()
                       .at("ais2api")
                       .as_object()
                       .at("gemini_delta")
                       .as_object()
                       .at("candidates")
                       .as_array()[0]
                       .as_object()
                       .at("content")
                       .as_object()
                       .at("parts")
                       .as_array()[0]
                       .as_object();
    REQUIRE(native.at("functionCall").as_object().at("id") == first.at("id"));
    REQUIRE(native.at("thoughtSignature") == "signature-a");
    REQUIRE_FALSE(original_a.at("candidates")
                      .as_array()[0]
                      .as_object()
                      .at("content")
                      .as_object()
                      .at("parts")
                      .as_array()[0]
                      .as_object()
                      .at("functionCall")
                      .as_object()
                      .if_contains("id"));
    first.erase("index");
    second.erase("index");
    third.erase("index");
    json::object request{
        {"model", "m"},
        {"messages", json::array{json::object{{"role", "assistant"},
                                              {"content", nullptr},
                                              {"tool_calls", json::array{first, second, third}}}}}};
    auto restored = normalize_openai(request, Config{}, &store, "other-owner");
    auto &parts = restored.gemini.at("contents").as_array()[0].as_object().at("parts").as_array();
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0].as_object().at("thoughtSignature") == "signature-a");
    REQUIRE(parts[1].as_object().at("thoughtSignature") == "signature-b");
    REQUIRE_FALSE(parts[2].as_object().if_contains("thoughtSignature"));
    for (const auto &part : parts)
        REQUIRE_FALSE(part.as_object().at("functionCall").as_object().if_contains("id"));
}
TEST_CASE("Upstream tool ids retain their stream index among idless calls",
          "[protocol][regression]") {
    ToolRegressionStore fixture;
    auto &store = *fixture.store;
    auto anonymous =
        stream_tool(openai_delta(native_tool_event(), "m", "mixed-stream", &store, "owner"));
    auto named = stream_tool(openai_delta(native_tool_event("upstream-signature", "upstream-id"),
                                          "m", "mixed-stream", &store, "owner"));
    auto repeat = stream_tool(openai_delta(native_tool_event("upstream-signature", "upstream-id"),
                                           "m", "mixed-stream", &store, "owner"));
    auto next =
        stream_tool(openai_delta(native_tool_event(), "m", "mixed-stream", &store, "owner"));
    REQUIRE(anonymous.at("index") == 0);
    REQUIRE(string_or(named, "id").starts_with("call_gemini_"));
    REQUIRE(named.at("index") == 1);
    REQUIRE(repeat.at("id") == named.at("id"));
    REQUIRE(repeat.at("index") == named.at("index"));
    REQUIRE(next.at("index") == 2);
    REQUIRE(next.at("id") != anonymous.at("id"));
}
TEST_CASE("Signed idless function output roundtrips standard and native messages together",
          "[protocol][regression]") {
    json::object original = obj(
        R"json({"candidates":[{"content":{"role":"model","parts":[{"text":"answer","thoughtSignature":"text-signature"},{"functionCall":{"name":"left","args":{}},"thoughtSignature":"left-signature"},{"functionCall":{"name":"right","args":{"value":true}},"thoughtSignature":"right-signature"}]}}]})json");
    auto response = openai_response(original, "m", "stateless-response");
    auto message = response.at("choices").as_array()[0].as_object().at("message").as_object();
    auto &calls = message.at("tool_calls").as_array();
    REQUIRE(calls.size() == 2);
    auto left = string_or(calls[0].as_object(), "id"),
         right = string_or(calls[1].as_object(), "id");
    REQUIRE(left != right);
    auto &native =
        message.at("ais2api").as_object().at("gemini_content").as_object().at("parts").as_array();
    REQUIRE(native[1].as_object().at("functionCall").as_object().at("id") == json::value(left));
    REQUIRE(native[2].as_object().at("functionCall").as_object().at("id") == json::value(right));
    json::object request{
        {"model", "m"},
        {"messages",
         json::array{
             message,
             json::object{{"role", "tool"}, {"tool_call_id", left}, {"content", "{\"ok\":true}"}},
             json::object{{"role", "tool"}, {"tool_call_id", right}, {"content", "done"}}}}};
    auto roundtrip = normalize_openai(request, Config{});
    auto &contents = roundtrip.gemini.at("contents").as_array();
    auto &restored = contents[0].as_object().at("parts").as_array();
    auto expected_native = native;
    expected_native[1].as_object().at("functionCall").as_object().erase("id");
    expected_native[2].as_object().at("functionCall").as_object().erase("id");
    REQUIRE(restored == expected_native);
    REQUIRE_FALSE(restored[1].as_object().at("functionCall").as_object().if_contains("id"));
    REQUIRE_FALSE(restored[2].as_object().at("functionCall").as_object().if_contains("id"));
    REQUIRE(restored[0].as_object().at("thoughtSignature") == "text-signature");
    REQUIRE(restored[1].as_object().at("thoughtSignature") == "left-signature");
    REQUIRE(restored[2].as_object().at("thoughtSignature") == "right-signature");
    REQUIRE(contents.size() == 2);
    auto &results = contents[1].as_object().at("parts").as_array();
    REQUIRE(results.size() == 2);
    REQUIRE_FALSE(results[0].as_object().at("functionResponse").as_object().if_contains("id"));
    REQUIRE_FALSE(results[1].as_object().at("functionResponse").as_object().if_contains("id"));
    auto args_conflict = request;
    args_conflict.at("messages")
        .as_array()[0]
        .as_object()
        .at("tool_calls")
        .as_array()[0]
        .as_object()
        .at("function")
        .as_object()["arguments"] = "{\"changed\":true}";
    REQUIRE_THROWS_AS(normalize_openai(args_conflict, Config{}), Error);
    auto id_conflict = request;
    id_conflict.at("messages")
        .as_array()[0]
        .as_object()
        .at("tool_calls")
        .as_array()[0]
        .as_object()["id"] = "unrelated";
    REQUIRE_THROWS_AS(normalize_openai(id_conflict, Config{}), Error);
    REQUIRE_FALSE(original.at("candidates")
                      .as_array()[0]
                      .as_object()
                      .at("content")
                      .as_object()
                      .at("parts")
                      .as_array()[1]
                      .as_object()
                      .at("functionCall")
                      .as_object()
                      .if_contains("id"));
}
TEST_CASE("json_size matches serialized length without serializing", "[protocol][stream-performance]") {
    std::string every_byte;
    for (int c = 0; c < 256; ++c) every_byte += static_cast<char>(c);
    json::array values{nullptr, true, false, 0, -1, std::numeric_limits<std::int64_t>::min(),
                       std::numeric_limits<std::uint64_t>::max(), 1.5, -0.0, 1e300, 3.0,
                       "", "plain", "\xe4\xbd\xa0\xe5\xa5\xbd", json::string_view(every_byte.data(), every_byte.size()),
                       json::array{}, json::object{}, json::array{1, json::array{json::object{}}, "x"}};
    json::object nested{{"", 1}, {"k\"\\n", values}, {"inner", {{"a", json::array{}}, {"b", nullptr}}}};
    values.push_back(nested);
    for (const auto& value : values) CHECK(json_size(value) == json::serialize(value).size());
    CHECK(json_size(values) == json::serialize(values).size());
    CHECK(json_size(nested) == json::serialize(nested).size());
}

TEST_CASE("Upstream additions are skipped and reported instead of failing generation", "[protocol][grounding]") {
    // Part slots 9 and 15, candidate slot 9 and chunk slot 8 carry additions;
    // a scalar where a part belongs is skipped as well.
    auto chunk = json::parse(R"([[[[[[null,"answer",null,null,null,null,null,null,null,["moved"],null,null,null,null,null,[["https://example.com","Example"]]],null,5],"model"],1,null,null,null,null,null,null,null,[["query"]]]],null,null,null,null,null,null,null,["grounding"]])");
    auto event = decode_chunk(chunk);
    auto &parts = event.at("candidates").as_array()[0].as_object().at("content").as_object().at("parts").as_array();
    REQUIRE(parts == json::array{json::object{{"text", "answer"}}});
    REQUIRE(take_upstream_ignored(event) ==
            json::parse(R"(["chunk[8]","candidate[9]","part[9]","part[15]","part"])"));
    REQUIRE_FALSE(event.contains(upstream_ignored_key));
    REQUIRE(take_upstream_ignored(event).empty());
    auto plain = decode_chunk(response_wire(one(json::array{nullptr, "plain"})));
    REQUIRE_FALSE(plain.contains(upstream_ignored_key));
}
