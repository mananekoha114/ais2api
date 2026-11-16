// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/protocol.hpp"
#include "ais2api/schema.hpp"
#include <catch2/catch_test_macros.hpp>
#include <utility>

using namespace ais2api;
namespace {
json::object schema_object(std::string_view text) { return json::parse(text).as_object(); }
void require_invalid_schema(const json::object &schema) {
    try {
        (void)prepare_schema(schema, nullptr, "schema", true);
        FAIL("Expected invalid schema");
    } catch (const UnsupportedSchema &) {
        FAIL("Malformed schema must not be treated as an unsupported capability");
    } catch (const Error &e) {
        REQUIRE(e.status == 400);
        REQUIRE(e.code == "invalid_argument");
    }
}
}

TEST_CASE("Schema compatibility drops only known validated keywords", "[schema][compat]") {
    auto schema = schema_object(R"({"type":"object","properties":{"value":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":9,"multipleOf":2}},"patternProperties":{"^x":{"type":"string"}},"additionalItems":false,"prefixItems":[{"type":"string"}],"contains":{"type":"number"},"minContains":1,"maxContains":3,"uniqueItems":true,"dependencies":{"x":["y"]},"dependentRequired":{"x":["y"]},"dependentSchemas":{"x":{"type":"object"}},"if":{"type":"object"},"then":{"type":"object"},"else":false,"unevaluatedProperties":false,"unevaluatedItems":false,"contentEncoding":"base64","contentMediaType":"text/plain","contentSchema":{"type":"string"},"readOnly":true,"writeOnly":false,"deprecated":true,"examples":[{}]})");
    REQUIRE_THROWS_AS(prepare_schema(schema), Error);
    json::array ignored;
    auto result = prepare_schema(schema, &ignored, "response_format.schema", true);
    REQUIRE(result.size() == 2);
    // Exclusive bounds of a number become inclusive bounds plus a hint.
    const auto &value = result.at("properties").as_object().at("value").as_object();
    REQUIRE(value.size() == 4);
    REQUIRE(value.at("minimum") == 0);
    REQUIRE(value.at("maximum") == 9);
    REQUIRE(ignored.size() == 25);
    REQUIRE(ignored.front().is_string());
}

TEST_CASE("Schema compatibility preserves established metadata conversions", "[schema][compat]") {
    auto schema = schema_object(R"({"type":"object","title":"Result","default":{},"additionalProperties":false})");
    REQUIRE(prepare_schema(schema) == prepare_schema(schema, nullptr, "schema", true));
    REQUIRE(prepare_schema(schema).at("description").is_string());
}

TEST_CASE("Schema compatibility rejects invalid inputs before dropping constraints", "[schema][compat]") {
    for (auto text : {
        R"({"type":"string","patern":"x"})",
        R"({"type":"string","readOnly":"yes"})",
        R"({"type":"number","multipleOf":0})",
        R"({"type":"array","minContains":-1})",
        R"({"type":"object","dependentRequired":{"x":[4]}})",
        R"({"type":"object","patternProperties":{"x":{"type":"string","patern":"x"}}})",
        R"({"type":"object","if":{"type":"string","minLength":"x"}})",
        R"({"$ref":"#/~2"})",
        R"({"$ref":"#/missing"})",
        R"({"$defs":{"x":{"type":"string"}},"$ref":"#/$defs/x","type":"number"})",
        R"({"type":["string",7]})",
        R"({"type":"invalid"})"})
        require_invalid_schema(schema_object(text));
}

TEST_CASE("Unsupported whole schema forms use a distinct capability error", "[schema][compat]") {
    for (auto text : {
        R"({})",
        R"({"$ref":"https://example.com/schema"})",
        R"({"$ref":"#"})",
        R"({"type":["string","number"]})",
        R"({"type":"object","properties":{"x":true}})",
        R"({"type":"array","items":[{"type":"string"}]})",
        R"({"type":"array","items":false})",
        R"({"anyOf":[true,{"type":"string"}]})"}) {
        auto schema = schema_object(text);
        REQUIRE_THROWS_AS(prepare_schema(schema), UnsupportedSchema);
        REQUIRE_THROWS_AS(prepare_schema(schema, nullptr, "schema", true), UnsupportedSchema);
    }
}

TEST_CASE("Schema compatibility preserves local ref lowering and expansion limits", "[schema][compat]") {
    auto local = schema_object(R"({"$defs":{"value":{"type":"string","readOnly":true}},"type":"object","properties":{"x":{"$ref":"#/$defs/value"}}})");
    auto result = prepare_schema(local, nullptr, "schema", true);
    REQUIRE(result.at("properties").as_object().at("x").as_object().at("type") == "string");
    json::object deep{{"type", "string"}};
    for (unsigned i = 0; i < 66; ++i)
        deep = json::object{{"type", "array"}, {"items", std::move(deep)}};
    require_invalid_schema(deep);
    json::object large{{"type", "string"}, {"description", std::string(8 * 1024 * 1024, 'x')}};
    require_invalid_schema(large);
}

TEST_CASE("Unsupported schema siblings cannot hide invalid references", "[schema][compat]") {
    for (auto text : {
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"#/missing"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"#/~2"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"https://example.com/bad schema"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"https://example.com/%XX"}}})",
        R"({"$defs":{"x":{"type":"string"}},"type":"object","properties":{"a":true,"b":{"$ref":"#/$defs/x","type":"number"}}})",
        R"({"type":"object","properties":{"a":true,"b":{"$ref":"#","type":"string"}}})"})
        require_invalid_schema(schema_object(text));
    REQUIRE_THROWS_AS(prepare_schema(schema_object(R"({"type":"object","properties":{"a":true,"b":{"$ref":"#"}}})"), nullptr, "schema", true), UnsupportedSchema);
}

TEST_CASE("Local schema refs decode URI fragments before JSON Pointer tokens", "[schema][ref]") {
    auto schema = schema_object(R"({"$defs":{
        "A B":{"type":"string"},"A%20B":{"type":"integer"},
        "\u732b":{"type":"boolean"},"100%":{"type":"number"},
        "a/b~c":{"type":"array","items":{"type":"string"}},
        "outer":{"$defs":{"inner":{"type":"object"}}},
        "outer/$defs/inner":{"type":"integer"}
    },"type":"object","properties":{}})");
    for (const auto &[ref, type] : {
        std::pair{"#/$defs/A%20B", "string"},
        std::pair{"#/$defs/A%2520B", "integer"},
        std::pair{"#/$defs/%e7%8c%ab", "boolean"},
        std::pair{"#/$defs/100%25", "number"},
        std::pair{"#/$defs/a%7E1b%7e0c", "array"},
        std::pair{"#%2F$defs%2Fouter%2F$defs%2Finner", "object"},
        std::pair{"#/$defs/outer~1$defs~1inner", "integer"}}) {
        CAPTURE(ref);
        schema.at("properties").as_object()["value"] = json::object{{"$ref", ref}};
        for (bool compatibility : {false, true}) {
            auto result = prepare_schema(schema, nullptr, "schema", compatibility);
            REQUIRE(result.at("properties").as_object().at("value").as_object().at("type") == type);
        }
    }
}

TEST_CASE("Encoded local schema refs retain malformed and recursive reference policy", "[schema][ref]") {
    for (auto ref : {"#/$defs/%", "#/$defs/%2", "#/$defs/%GG", "#/$defs/%7E2", "#/$defs/%7E"}) {
        CAPTURE(ref);
        require_invalid_schema(json::object{{"$defs", json::object{}}, {"$ref", ref}});
    }
    auto recursive = schema_object(R"({"$defs":{"A B":{"type":"object","properties":{"next":{"$ref":"#/$defs/A%20B"}}}},"$ref":"#/$defs/A%20B"})");
    REQUIRE_THROWS_AS(prepare_schema(recursive), UnsupportedSchema);
    REQUIRE_THROWS_AS(prepare_schema(recursive, nullptr, "schema", true), UnsupportedSchema);
}

TEST_CASE("Exclusive bounds lower to inclusive bounds by default", "[schema][compat]") {
    auto lowered = [](std::string_view text, json::array *ignored = nullptr) {
        return prepare_schema(schema_object(text), ignored);
    };
    // Integers convert exactly and record nothing as ignored.
    json::array ignored;
    auto limit = lowered(R"({"type":"integer","exclusiveMinimum":0,"maximum":9007199254740991})", &ignored);
    CHECK(limit == schema_object(R"({"type":"integer","minimum":1,"maximum":9007199254740991})"));
    CHECK(ignored.empty());
    CHECK(lowered(R"({"type":"integer","exclusiveMaximum":10})") == schema_object(R"({"type":"integer","maximum":9})"));
    CHECK(lowered(R"({"type":"integer","exclusiveMaximum":10,"maximum":5})") == schema_object(R"({"type":"integer","maximum":5})"));
    CHECK(lowered(R"({"type":"integer","exclusiveMinimum":0.5,"exclusiveMaximum":2.5})") ==
          schema_object(R"({"type":"integer","minimum":1,"maximum":2})"));
    CHECK(lowered(R"({"type":["integer","null"],"exclusiveMinimum":-1})") ==
          schema_object(R"({"type":["integer","null"],"minimum":0})"));
    // Draft 4 flags.
    CHECK(lowered(R"({"type":"integer","minimum":3,"exclusiveMinimum":true})") == schema_object(R"({"type":"integer","minimum":4})"));
    CHECK(lowered(R"({"type":"integer","maximum":3,"exclusiveMaximum":false})") == schema_object(R"({"type":"integer","maximum":3})"));
    CHECK(lowered(R"({"type":"integer","exclusiveMinimum":true})") == schema_object(R"({"type":"integer"})"));
    // Numbers keep the limit inclusively and say the rest in the description.
    auto positive = lowered(R"({"type":"number","exclusiveMinimum":0})", &ignored);
    CHECK(positive.at("minimum") == 0);
    CHECK(positive.at("description") == "Must be greater than 0.");
    CHECK(ignored == json::array{"schema.exclusiveMinimum"});
    auto draft4 = lowered(R"({"type":"number","description":"Ratio.","maximum":1.5,"exclusiveMaximum":true})");
    CHECK(draft4.at("maximum") == 1.5);
    CHECK(draft4.at("description") == "Ratio.\nMust be less than 1.5.");
    // An inclusive bound that already excludes the limit needs no hint.
    CHECK(lowered(R"({"type":"number","minimum":5,"exclusiveMinimum":0})") == schema_object(R"({"type":"number","minimum":5})"));
}

TEST_CASE("Nested schemas without a type accept any JSON value as text", "[schema][compat]") {
    auto schema = schema_object(R"({"$defs":{"any":{}},"type":"object","properties":{
        "args":{"description":"Input."},
        "list":{"type":"array","items":{}},
        "ref":{"$ref":"#/$defs/any"},
        "union":{"anyOf":[{"type":"string"},{}]},
        "map":{"type":"object","additionalProperties":{}}}})");
    json::array paths;
    auto result = prepare_schema(schema, nullptr, "schema", false, &paths);
    const auto &properties = result.at("properties").as_object();
    CHECK(properties.at("args").as_object().at("type") == "string");
    CHECK(properties.at("args").as_object().at("description") == "Input.\nAny JSON value, written as JSON text.");
    CHECK(properties.at("list").as_object().at("items").as_object().at("type") == "string");
    CHECK(properties.at("ref").as_object().at("type") == "string");
    CHECK(properties.at("union").as_object().at("anyOf").as_array()[1].as_object().at("type") == "string");
    // An empty additionalProperties schema allows anything, like true.
    CHECK(properties.at("map") == schema_object(R"({"type":"object"})"));
    // Only values reachable through properties and items are recorded.
    CHECK(paths == json::array{json::array{"args"}, json::array{"list", nullptr}, json::array{"ref"}});
    // The root still needs a type, and so does a schema with constraints.
    for (auto text : {R"({})", R"({"description":"x"})", R"({"type":"object","properties":{"x":{"minLength":1}}})"})
        REQUIRE_THROWS_AS(prepare_schema(schema_object(text)), UnsupportedSchema);
}

TEST_CASE("JSON text arguments decode back into values", "[schema][protocol]") {
    Config config;
    auto body = schema_object(R"({"contents":[{"role":"user","parts":[{"text":"hi"}]}],
        "tools":[{"functionDeclarations":[{"name":"run","parameters":{"type":"object","properties":{
            "args":{},"steps":{"type":"array","items":{"type":"object","properties":{"input":{}}}},"name":{"type":"string"}}}}]}]})");
    auto request = normalize_gemini("gemini-test", body, false, config);
    CHECK(request.json_text_args == schema_object(R"({"run":[["args"],["steps",null,"input"]]})"));
    auto event = schema_object(R"({"candidates":[{"content":{"role":"model","parts":[
        {"functionCall":{"name":"run","args":{"args":"[1,{\"a\":true}]","name":"[1]",
            "steps":[{"input":"{\"q\":2}"},{"input":"plain text"}]}}},
        {"functionCall":{"name":"other","args":{"args":"[3]"}}}]}}]})");
    decode_json_text_args(event, request.json_text_args);
    const auto expected = schema_object(R"({"candidates":[{"content":{"role":"model","parts":[
        {"functionCall":{"name":"run","args":{"args":[1,{"a":true}],"name":"[1]",
            "steps":[{"input":{"q":2}},{"input":"plain text"}]}}},
        {"functionCall":{"name":"other","args":{"args":"[3]"}}}]}}]})");
    CHECK(event == expected);
}
