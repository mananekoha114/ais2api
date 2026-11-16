// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/schema.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>

namespace ais2api {
namespace {
[[noreturn]] void bad(std::string message) {
    throw Error(400, "invalid_argument", "schema: " + std::move(message));
}
// A null result denotes a valid reference whose capability is unsupported.
// This check never follows reference edges, so legal recursion remains valid.
const json::object *reference_target(const json::object &root, std::string_view ref) {
    if (ref.empty()) bad("empty $ref is not supported");
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const auto c = static_cast<unsigned char>(ref[i]);
        if (c <= 0x20 || c == 0x7f) bad("invalid URI in $ref");
        if (c == '%') {
            if (i + 2 >= ref.size() || !std::isxdigit(static_cast<unsigned char>(ref[i + 1])) || !std::isxdigit(static_cast<unsigned char>(ref[i + 2])))
                bad("invalid URI escape in $ref");
            i += 2;
        }
    }
    if (!ref.starts_with('#')) return nullptr;
    // URI fragments are decoded once before interpreting JSON Pointer syntax.
    // In particular, an encoded slash separates tokens, while ~1 denotes a
    // slash inside a token. Do not decode percent sequences introduced here.
    std::string fragment;
    auto hex = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    for (std::size_t i = 1; i < ref.size(); ++i) {
        if (ref[i] != '%') fragment += ref[i];
        else {
            fragment += static_cast<char>((hex(ref[i + 1]) << 4) | hex(ref[i + 2]));
            i += 2;
        }
    }
    ref = fragment;
    if (ref.empty()) return &root;
    if (!ref.starts_with('/')) return nullptr;
    const json::object *current = &root;
    ref.remove_prefix(1);
    while (true) {
        const auto slash = ref.find('/');
        auto token = ref.substr(0, slash);
        std::string key;
        for (std::size_t i = 0; i < token.size(); ++i) {
            if (token[i] != '~') key += token[i];
            else {
                if (++i == token.size() || (token[i] != '0' && token[i] != '1'))
                    bad("invalid JSON Pointer escape in $ref");
                key += token[i] == '0' ? '~' : '/';
            }
        }
        auto value = current->if_contains(key);
        if (value && value->is_bool() && slash == std::string_view::npos)
            return nullptr;
        if (!value || !value->is_object()) bad("$ref does not resolve to a schema object");
        current = &value->as_object();
        if (slash == std::string_view::npos) return current;
        ref.remove_prefix(slash + 1);
    }
}
void validate_reference(const json::object &root, const json::object &source) {
    auto ref = source.if_contains("$ref");
    if (!ref) return;
    if (!ref->is_string()) bad("$ref must be a string");
    const json::object *target = reference_target(root, std::string_view(ref->as_string()));
    std::set<const json::object *> seen;
    while (target && seen.insert(target).second) {
        if (seen.size() > 4096) bad("schema reference chain exceeds node limit");
        for (const auto &kv : source) {
            if (kv.key() == "$ref" || kv.key() == "description" || kv.key() == "title" || kv.key() == "default") continue;
            if (auto old = target->if_contains(kv.key()); old && *old != kv.value())
                bad("conflicting $ref siblings are not supported");
        }
        auto next = target->if_contains("$ref");
        if (!next) break;
        if (!next->is_string()) bad("$ref must be a string");
        target = reference_target(root, std::string_view(next->as_string()));
    }
}
// Validate the original tree before capability checks, so dropping a root or
// a keyword cannot hide malformed schemas or misspelled fields.
void validate(const json::value &value, const json::object &root, unsigned depth, std::size_t &nodes) {
    if (depth > 64 || ++nodes > 4096) bad("schema exceeds depth or node limit");
    if (value.is_bool()) return;
    if (!value.is_object()) bad("schema must be an object or boolean");
    validate_reference(root, value.as_object());
    for (const auto &kv : value.as_object()) {
        const auto key = std::string_view(kv.key());
        const auto &v = kv.value();
        auto require = [&](bool valid) { if (!valid) bad(std::string(key) + " has an invalid value type"); };
        if (key == "properties" || key == "$defs" || key == "definitions" || key == "patternProperties" || key == "dependentSchemas") {
            require(v.is_object());
            for (const auto &entry : v.as_object()) validate(entry.value(), root, depth + 1, nodes);
        } else if (key == "oneOf" || key == "anyOf" || key == "allOf" || key == "prefixItems") {
            require(v.is_array());
            for (const auto &entry : v.as_array()) validate(entry, root, depth + 1, nodes);
        } else if (key == "items" && v.is_array()) {
            for (const auto &entry : v.as_array()) validate(entry, root, depth + 1, nodes);
        } else if (key == "items" || key == "not" || key == "additionalProperties" || key == "additionalItems" || key == "contains" || key == "if" || key == "then" || key == "else" || key == "unevaluatedProperties" || key == "unevaluatedItems" || key == "contentSchema" || key == "propertyNames") {
            validate(v, root, depth + 1, nodes);
        } else if (key == "required" || key == "propertyOrdering") {
            require(v.is_array());
            for (const auto &entry : v.as_array()) require(entry.is_string());
        } else if (key == "dependencies" || key == "dependentRequired") {
            require(v.is_object());
            for (const auto &entry : v.as_object()) {
                if (entry.value().is_array()) {
                    for (const auto &name : entry.value().as_array()) require(name.is_string());
                } else {
                    require(key != "dependentRequired");
                    validate(entry.value(), root, depth + 1, nodes);
                }
            }
        } else if (key == "type") {
            require(v.is_string() || v.is_array());
            auto valid_type = [&](const json::value &t) {
                require(t.is_string());
                std::string name(t.as_string());
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                require(name == "string" || name == "number" || name == "integer" || name == "boolean" || name == "array" || name == "object" || name == "null");
            };
            if (v.is_array()) { require(!v.as_array().empty()); for (const auto &t : v.as_array()) valid_type(t); }
            else valid_type(v);
        } else if (key == "title" || key == "$schema" || key == "$id" || key == "$comment" || key == "$ref" || key == "$anchor" || key == "$dynamicAnchor" || key == "format" || key == "description" || key == "pattern" || key == "contentEncoding" || key == "contentMediaType") require(v.is_string());
        else if (key == "nullable" || key == "uniqueItems" || key == "readOnly" || key == "writeOnly" || key == "deprecated") require(v.is_bool());
        else if (key == "minimum" || key == "maximum" || key == "multipleOf") { require(v.is_number()); if (key == "multipleOf") require(json::value_to<double>(v) > 0); }
        else if (key == "exclusiveMinimum" || key == "exclusiveMaximum") require(v.is_number() || v.is_bool());
        else if (key == "minProperties" || key == "maxProperties" || key == "minLength" || key == "maxLength" || key == "minItems" || key == "maxItems" || key == "minContains" || key == "maxContains") require(v.is_uint64() || (v.is_int64() && v.as_int64() >= 0));
        else if (key == "enum" || key == "examples") require(v.is_array());
        else if (key != "default" && key != "const" && key != "example") bad("unsupported field " + std::string(key));
    }
}
std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
// The single non-null type of a schema, lowercased, or empty.
std::string concrete_type(const json::object &schema) {
    auto type = schema.if_contains("type");
    if (!type) return {};
    if (type->is_string()) return lowercase(type->as_string());
    std::string found;
    if (type->is_array())
        for (const auto &t : type->as_array())
            if (t.is_string() && lowercase(t.as_string()) != "null") {
                if (!found.empty()) return {};
                found = lowercase(t.as_string());
            }
    return found;
}
// A number as people write it: 1.5 rather than serialize's 1.5E0.
std::string number_text(const json::value &value) {
    if (!value.is_double()) return json::serialize(value);
    char digits[32];
    const auto result = std::to_chars(digits, digits + sizeof digits, value.as_double());
    return result.ec == std::errc{} ? std::string(digits, result.ptr) : json::serialize(value);
}
// Whether `a` is a tighter lower (or, with `lower` false, upper) bound than `b`.
bool tighter(const json::value &a, const json::value &b, bool lower) {
    if (a.is_int64() && b.is_int64()) return lower ? a.as_int64() > b.as_int64() : a.as_int64() < b.as_int64();
    const auto x = json::value_to<double>(a), y = json::value_to<double>(b);
    return lower ? x > y : x < y;
}
// The nearest integer strictly above (or below) `value`, when it fits int64.
std::optional<std::int64_t> integer_beyond(const json::value &value, bool above) {
    if (value.is_int64()) {
        const auto v = value.as_int64();
        if (above ? v == INT64_MAX : v == INT64_MIN) return std::nullopt;
        return above ? v + 1 : v - 1;
    }
    if (value.is_uint64()) {
        // Every uint64 value exceeds INT64_MAX; only one step down can fit.
        if (above || value.as_uint64() - 1 > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
        return static_cast<std::int64_t>(value.as_uint64() - 1);
    }
    const auto d = value.as_double();
    const auto r = above ? std::floor(d) + 1 : std::ceil(d) - 1;
    if (!std::isfinite(r) || r < -9.2e18 || r > 9.2e18) return std::nullopt;
    return static_cast<std::int64_t>(r);
}
class Lowerer {
    const json::object &root_;
    json::array *ignored_;
    std::string location_;
    bool ignore_unsupported_;
    json::array *json_text_;
    std::set<const json::object *> active_;
    std::size_t nodes_ = 0;
    std::size_t bytes_ = 0;
    static constexpr std::size_t byte_limit = 8 * 1024 * 1024;

    void charge(std::size_t size) {
        if (size > byte_limit - bytes_) bad("schema expansion exceeds byte limit");
        bytes_ += size;
    }

    void ignored(std::string_view keyword) {
        if (!ignored_) return;
        json::value name(location_ + "." + std::string(keyword));
        if (std::find(ignored_->begin(), ignored_->end(), name) == ignored_->end())
            ignored_->push_back(std::move(name));
    }
    const json::object &resolve(std::string_view ref) {
        if (auto target = reference_target(root_, ref)) return *target;
        throw UnsupportedSchema("remote, anchor or boolean reference schema is not supported");
    }
    void hint(json::object &schema, std::string text) {
        charge(text.size());
        auto p = schema.if_contains("description");
        if (p && !p->is_string()) bad("description must be a string");
        auto description = p ? std::string(p->as_string()) : std::string{};
        if (!description.empty()) description += '\n';
        schema["description"] = description + text;
    }
    // The upstream has inclusive minimum and maximum only. Integer bounds
    // convert exactly; other bounds become the inclusive limit plus a hint.
    void exclusive_bounds(json::object &schema) {
        const bool integer = concrete_type(schema) == "integer";
        for (const bool lower_bound : {true, false}) {
            const std::string key = lower_bound ? "exclusiveMinimum" : "exclusiveMaximum";
            const char *inclusive = lower_bound ? "minimum" : "maximum";
            auto p = schema.if_contains(key);
            if (!p) continue;
            if (!p->is_number() && !p->is_bool()) bad(key + " has an invalid value type");
            std::optional<json::value> bound;
            if (p->is_number()) bound = *p;
            // Draft 4 spells it as a flag that makes minimum or maximum exclusive.
            else if (p->as_bool())
                if (auto q = schema.if_contains(inclusive)) bound = *q;
            schema.erase(key);
            if (!bound) continue;
            const auto current = schema.if_contains(inclusive);
            const bool draft4 = current && *current == *bound;
            if (integer)
                if (auto step = integer_beyond(*bound, lower_bound)) {
                    json::value value(*step);
                    if (draft4 || !current || tighter(value, *current, lower_bound)) schema[inclusive] = value;
                    continue;
                }
            if (!current || tighter(*bound, *current, lower_bound)) schema[inclusive] = *bound;
            else if (!draft4) continue; // an inclusive bound already excludes it
            hint(schema, std::string(lower_bound ? "Must be greater than " : "Must be less than ") +
                             number_text(*bound) + ".");
            ignored(key);
        }
    }
public:
    Lowerer(const json::object &root, json::array *ignored, std::string_view location, bool ignore_unsupported,
            json::array *json_text)
        : root_(root), ignored_(ignored), location_(location), ignore_unsupported_(ignore_unsupported),
          json_text_(json_text) {}

    // `path` leads from the root through properties and items, or is null
    // inside unions, not and additionalProperties.
    json::object lower(const json::object &source, unsigned depth, const json::array *path, bool root) {
        if (depth > 64 || ++nodes_ > 4096) bad("schema expansion exceeds depth or node limit");
        // Charge before copying, including each repeated reference target.
        // This bounds live intermediate copies, not just the final result.
        charge(json::serialize(source).size());
        if (!active_.insert(&source).second) throw UnsupportedSchema("recursive $ref is not supported by the upstream");
        auto schema = source;
        if (auto ref = source.if_contains("$ref")) {
            if (!ref->is_string()) bad("$ref must be a string");
            auto target = lower(resolve(std::string_view(ref->as_string())), depth + 1, path, root);
            schema.erase("$ref");
            // Metadata siblings are common in Pydantic schemas. Conflicting
            // validation siblings require intersection semantics; do not weaken them.
            for (const auto &kv : schema) {
                auto old = target.if_contains(kv.key());
                if (old && *old != kv.value() && kv.key() != "description" &&
                    kv.key() != "title" && kv.key() != "default")
                    bad("conflicting $ref siblings are not supported");
                target[kv.key()] = kv.value();
            }
            schema = std::move(target);
        }
        exclusive_bounds(schema);
        // Validate known unsupported keyword values before dropping constraints.
        // Unknown fields remain errors even in compatibility mode.
        for (auto it = schema.begin(); it != schema.end();) {
            const std::string key(it->key());
            const auto &v = it->value();
            bool known = true, valid = true;
            if (key == "multipleOf") valid = v.is_number() && json::value_to<double>(v) > 0;
            else if (key == "uniqueItems" || key == "readOnly" || key == "writeOnly" || key == "deprecated") valid = v.is_bool();
            else if (key == "minContains" || key == "maxContains") valid = (v.is_uint64() || (v.is_int64() && v.as_int64() >= 0));
            else if (key == "contentEncoding" || key == "contentMediaType" || key == "$anchor" || key == "$dynamicAnchor") valid = v.is_string();
            else if (key == "examples") valid = v.is_array();
            else if (key == "patternProperties" || key == "dependentSchemas" || key == "dependencies" || key == "dependentRequired") {
                valid = v.is_object();
                if (valid) for (const auto &entry : v.as_object()) {
                    const auto &x = entry.value();
                    if (key == "dependentRequired" || (key == "dependencies" && x.is_array())) {
                        if (!x.is_array()) valid = false;
                        else for (const auto &name : x.as_array()) valid = valid && name.is_string();
                    } else valid = valid && (x.is_object() || x.is_bool());
                }
            } else if (key == "prefixItems") {
                valid = v.is_array();
                if (valid) for (const auto &x : v.as_array()) valid = valid && (x.is_object() || x.is_bool());
            } else if (key == "additionalItems" || key == "contains" || key == "if" || key == "then" || key == "else" || key == "unevaluatedProperties" || key == "unevaluatedItems" || key == "contentSchema" || key == "propertyNames") valid = v.is_object() || v.is_bool();
            else known = false;
            if (!known) { ++it; continue; }
            if (!valid) bad(key + " has an invalid value type");
            if (!ignore_unsupported_) bad("unsupported field " + key);
            ignored(key);
            it = schema.erase(it);
        }
        for (auto keyword : {"$defs", "definitions"}) {
            if (auto p = schema.if_contains(keyword); p && !p->is_object())
                bad(std::string(keyword) + " must be an object");
            schema.erase(keyword);
        }
        for (auto keyword : {"title", "$schema", "$id", "$comment"}) {
            if (auto p = schema.if_contains(keyword)) {
                if (!p->is_string()) bad(std::string(keyword) + " must be a string");
                if (std::string_view(keyword) == "title" && !p->as_string().empty())
                    hint(schema, "Title: " + std::string(p->as_string()));
                ignored(keyword);
                schema.erase(keyword);
            }
        }
        if (auto p = schema.if_contains("default")) {
            hint(schema, "Default: " + json::serialize(*p));
            ignored("default");
            schema.erase("default");
        }
        if (auto p = schema.if_contains("additionalProperties")) {
            if (!p->is_bool() && !p->is_object())
                bad("additionalProperties must be boolean or a schema object");
            if (!schema.if_contains("type")) schema["type"] = "object";
            p = schema.if_contains("additionalProperties");
            // An empty schema allows any value, like true.
            if (p->is_object() && !p->as_object().empty()) {
                auto additional = lower(p->as_object(), depth + 1, nullptr, false);
                hint(schema, "Additional property values follow this schema: " + json::serialize(additional));
            } else if (p->is_bool() && !p->as_bool())
                hint(schema, "Do not include properties other than those declared in properties.");
            ignored("additionalProperties");
            schema.erase("additionalProperties");
        }
        if (auto p = schema.if_contains("const")) {
            auto value = *p;
            if (auto values = schema.if_contains("enum"); values &&
                (!values->is_array() || std::find(values->as_array().begin(), values->as_array().end(), value) == values->as_array().end()))
                bad("const conflicts with enum");
            schema["enum"] = json::array{value};
            schema.erase("const");
        }
        if (!schema.if_contains("type"))
            if (auto values = schema.if_contains("enum"); values && values->is_array() && !values->as_array().empty()) {
                const auto &first = values->as_array().front();
                std::string type = first.is_string() ? "string" : first.is_bool() ? "boolean" :
                    (first.is_int64() || first.is_uint64()) ? "integer" : first.is_number() ? "number" : "";
                bool homogeneous = !type.empty();
                for (const auto &v : values->as_array()) {
                    if (type == "string") homogeneous = homogeneous && v.is_string();
                    else if (type == "boolean") homogeneous = homogeneous && v.is_bool();
                    else if (type == "integer") homogeneous = homogeneous && (v.is_int64() || v.is_uint64());
                    else if (type == "number") homogeneous = homogeneous && v.is_number();
                }
                if (homogeneous) schema["type"] = type;
            }
        // The private Schema enum field accepts strings only. Keep other
        // scalar enum values as explicit model guidance in non-strict mode.
        if (auto values = schema.if_contains("enum"); values && values->is_array()) {
            bool strings = true;
            for (const auto &v : values->as_array()) strings = strings && v.is_string();
            if (!strings) {
                hint(schema, "Allowed values: " + json::serialize(*values));
                ignored("enum");
                schema.erase("enum");
            }
        }
        for (auto keyword : {"properties"})
            if (auto p = schema.if_contains(keyword)) {
                if (!p->is_object()) bad("properties must be an object");
                for (auto &kv : p->as_object()) {
                    if (kv.value().is_bool()) throw UnsupportedSchema("boolean property schema is not supported");
                    if (!kv.value().is_object()) bad("property schema must be an object");
                    std::optional<json::array> child;
                    if (path) (child = *path)->push_back(json::value(kv.key()));
                    kv.value() = lower(kv.value().as_object(), depth + 1, child ? &*child : nullptr, false);
                }
            }
        for (auto keyword : {"items", "not"})
            if (auto p = schema.if_contains(keyword)) {
                if (p->is_bool() || (std::string_view(keyword) == "items" && p->is_array()))
                    throw UnsupportedSchema("boolean or tuple schema is not supported");
                if (!p->is_object()) bad(std::string(keyword) + " must be a schema object");
                std::optional<json::array> child;
                if (path && std::string_view(keyword) == "items") (child = *path)->push_back(nullptr);
                *p = lower(p->as_object(), depth + 1, child ? &*child : nullptr, false);
            }
        for (auto keyword : {"oneOf", "anyOf", "allOf"})
            if (auto p = schema.if_contains(keyword)) {
                if (!p->is_array()) bad(std::string(keyword) + " must be an array");
                for (auto &branch : p->as_array()) {
                    if (branch.is_bool()) throw UnsupportedSchema("boolean union schema is not supported");
                    if (!branch.is_object()) bad("union branch must be a schema object");
                    branch = lower(branch.as_object(), depth + 1, nullptr, false);
                }
            }
        // Also validate schemas used only as textual additionalProperties
        // guidance; these do not pass through the native encoder themselves.
        for (const auto &kv : schema) {
            static constexpr std::string_view allowed[] = {
                "type", "format", "description", "nullable", "enum", "items", "properties", "required",
                "minProperties", "maxProperties", "minimum", "maximum", "minLength", "maxLength", "pattern",
                "example", "oneOf", "anyOf", "allOf", "not", "maxItems", "minItems", "propertyOrdering"};
            if (std::find(std::begin(allowed), std::end(allowed), std::string_view(kv.key())) == std::end(allowed))
                bad("unsupported field " + std::string(kv.key()));
        }
        if (auto type = schema.if_contains("type")) {
            if (type->is_array()) {
                unsigned concrete = 0;
                for (const auto &t : type->as_array()) {
                    if (!t.is_string()) bad("type array must contain strings");
                    std::string name(t.as_string());
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (name != "string" && name != "number" && name != "integer" && name != "boolean" && name != "array" && name != "object" && name != "null") bad("invalid schema type");
                    if (name != "null") ++concrete;
                }
                if (concrete != 1) throw UnsupportedSchema("multi-type schema is not supported");
            } else if (!type->is_string()) bad("type must be a string or array");
        } else if (!schema.if_contains("properties") && !schema.if_contains("items") && !schema.if_contains("oneOf") && !schema.if_contains("anyOf") && !schema.if_contains("allOf")) {
            // A nested schema that only describes accepts any JSON value; the
            // upstream has no such type, so the model writes it as JSON text.
            const bool any = std::all_of(schema.begin(), schema.end(), [](const auto &kv) {
                return kv.key() == "description" || kv.key() == "nullable" || kv.key() == "example";
            });
            if (root || !any) throw UnsupportedSchema("schema requires a concrete type");
            schema["type"] = "string";
            hint(schema, "Any JSON value, written as JSON text.");
            if (path && json_text_) json_text_->push_back(*path);
        }
        active_.erase(&source);
        return schema;
    }
};
}
json::object prepare_schema(const json::object &schema, json::array *ignored, std::string_view location, bool ignore_unsupported,
                            json::array *json_text) {
    if (json::serialize(schema).size() > 8 * 1024 * 1024) bad("schema expansion exceeds byte limit");
    std::size_t nodes = 0;
    validate(json::value(schema), schema, 0, nodes);
    const json::array top;
    return Lowerer(schema, ignored, location, ignore_unsupported, json_text).lower(schema, 0, &top, true);
}
}
