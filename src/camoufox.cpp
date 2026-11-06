// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/camoufox.hpp"
#include "../browser/json_text.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace ais2api {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
}

json::array browser_detail::bidi_cookies(const json::array& stored) {
    json::array result;
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& source = value.as_object();
        if (source.contains("partitionKey") || bool_or(source, "partitionKeyOpaque")) continue;
        const auto* name = source.if_contains("name");
        const auto* content = source.if_contains("value");
        const auto* domain = source.if_contains("domain");
        if (!name || !name->is_string() || name->as_string().empty() || !content || !content->is_string() ||
            !domain || !domain->is_string() || domain->as_string().empty()) continue;
        json::object cookie{{"name", *name}, {"value", {{"type", "string"}, {"value", *content}}}, {"domain", *domain}};
        if (auto* path = source.if_contains("path"); path && path->is_string() && !path->as_string().empty()) cookie["path"] = *path;
        for (auto key : {"httpOnly", "secure"}) if (auto* flag = source.if_contains(key); flag && flag->is_bool()) cookie[key] = *flag;
        if (auto* expires = source.if_contains("expires"); expires && expires->is_number()) {
            const auto seconds = json::value_to<double>(*expires);
            if (seconds > 0) cookie["expiry"] = static_cast<std::uint64_t>(std::floor(seconds));
        }
        // SameSite=None is only valid on secure cookies.
        const auto same_site = lower(string_or(source, "sameSite"));
        if (same_site == "strict" || same_site == "lax" || (same_site == "none" && bool_or(source, "secure"))) cookie["sameSite"] = same_site;
        result.push_back(std::move(cookie));
    }
    return result;
}

json::array browser_detail::stored_cookies(const json::array& bidi) {
    json::array result;
    for (const auto& value : bidi) {
        if (!value.is_object()) continue;
        const auto& source = value.as_object();
        const auto* content = source.if_contains("value");
        if (!content || !content->is_object() || string_or(content->as_object(), "type") != "string") continue;
        const auto* text = content->as_object().if_contains("value");
        const auto* name = source.if_contains("name");
        if (!text || !text->is_string() || !name || !name->is_string() || name->as_string().empty()) continue;
        json::object cookie{{"name", *name}, {"value", *text}, {"domain", string_or(source, "domain")}};
        if (auto* path = source.if_contains("path"); path && path->is_string()) cookie["path"] = *path;
        for (auto key : {"secure", "httpOnly"}) if (auto* flag = source.if_contains(key); flag && flag->is_bool()) cookie[key] = *flag;
        const auto same_site = string_or(source, "sameSite");
        if (same_site == "strict") cookie["sameSite"] = "Strict";
        else if (same_site == "lax") cookie["sameSite"] = "Lax";
        else if (same_site == "none") cookie["sameSite"] = "None";
        if (auto* expiry = source.if_contains("expiry"); expiry && expiry->is_number() && json::value_to<double>(*expiry) > 0) cookie["expires"] = *expiry;
        result.push_back(std::move(cookie));
    }
    return result;
}

json::array browser_detail::cookies_for_host(const json::array& stored, std::string_view host) {
    const auto target = lower(std::string(host));
    json::array result;
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& cookie = value.as_object();
        auto domain = lower(string_or(cookie, "domain"));
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        if (domain.empty() || (target != domain && !target.ends_with("." + domain))) continue;
        // Only cookies for the site root, the path the signing request uses.
        if (const auto path = string_or(cookie, "path"); !path.empty() && path != "/") continue;
        result.push_back(cookie);
    }
    return result;
}

std::optional<Error> browser_detail::firefox_navigation_failure(std::string_view message) {
    if (message.empty()) return std::nullopt;
    const auto has = [&](std::string_view part) { return message.find(part) != std::string_view::npos; };
    // NS_ERROR_PROXY_* (including a proxy's 407 Firefox could not answer) and
    // NS_ERROR_UNKNOWN_PROXY_HOST.
    if (has("NS_ERROR_PROXY_") || has("NS_ERROR_UNKNOWN_PROXY_HOST"))
        return Error(502, "proxy_failed", "The browser could not reach Google through the configured proxy (" + std::string(message) + ")");
    static constexpr std::string_view unreachable[] = {
        "NS_ERROR_UNKNOWN_HOST", "NS_ERROR_CONNECTION_REFUSED", "NS_ERROR_NET_TIMEOUT", "NS_ERROR_NET_RESET",
        "NS_ERROR_NET_INTERRUPT", "NS_ERROR_OFFLINE", "NS_ERROR_NET_EMPTY_RESPONSE"};
    if (std::any_of(std::begin(unreachable), std::end(unreachable), has) || has("SSL_ERROR_") || has("SEC_ERROR_"))
        return Error(502, "network_failed", "The browser could not reach Google (" + std::string(message) + ")");
    return std::nullopt;
}

std::optional<unsigned short> browser_detail::bidi_endpoint_port(std::string_view line) {
    for (std::string_view host : {"ws://127.0.0.1:", "ws://localhost:"}) {
        const auto start = line.find(host);
        if (start == std::string_view::npos) continue;
        auto digits = line.substr(start + host.size());
        std::size_t length = 0;
        while (length < digits.size() && length < 6 && std::isdigit(static_cast<unsigned char>(digits[length]))) ++length;
        if (!length || length > 5 || (length < digits.size() && digits[length] != '/' && !std::isspace(static_cast<unsigned char>(digits[length]))))
            return std::nullopt;
        const auto port = std::stoul(std::string(digits.substr(0, length)));
        if (!port || port > 65535) return std::nullopt;
        return static_cast<unsigned short>(port);
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> browser_detail::camoufox_environment(std::string_view config_json) {
    constexpr std::size_t piece = 2047;
    std::vector<std::pair<std::string, std::string>> result;
    while (!config_json.empty()) {
        const auto size = utf8_prefix(config_json, piece);
        result.emplace_back("CAMOU_CONFIG_" + std::to_string(result.size() + 1), std::string(config_json.substr(0, size)));
        config_json.remove_prefix(size);
    }
    return result;
}

std::string browser_detail::firefox_user_js(const json::object& prefs) {
    std::vector<std::string_view> names;
    for (const auto& item : prefs) names.emplace_back(item.key());
    std::sort(names.begin(), names.end());
    const auto quoted = [](std::string_view text, std::string_view name) {
        // The preference parser is not JSON: keep to text it reads the same way.
        if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20; }))
            throw Error(500, "browser_launch", "Firefox preference " + std::string(name) + " contains a control character");
        std::string result;
        append_quoted(result, text);
        return result;
    };
    std::string result;
    for (auto name : names) {
        const auto& value = prefs.at(name);
        std::string literal;
        if (value.is_string()) literal = quoted(value.as_string(), name);
        else if (value.is_bool()) literal = value.as_bool() ? "true" : "false";
        else if (value.is_int64() || value.is_uint64()) {
            const auto number = value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(std::min<std::uint64_t>(value.as_uint64(), INT64_MAX));
            if (number < std::numeric_limits<std::int32_t>::min() || number > std::numeric_limits<std::int32_t>::max())
                throw Error(500, "browser_launch", "Firefox preference " + std::string(name) + " is out of range");
            literal = std::to_string(number);
        } else throw Error(500, "browser_launch", "Firefox preference " + std::string(name) + " has an unsupported type");
        result += "user_pref(" + quoted(name, name) + ", " + literal + ");\n";
    }
    return result;
}

browser_detail::CamoufoxIdentityPool browser_detail::load_identity_pool(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw Error(503, "browser_not_configured", "Camoufox identity pool missing: run scripts/camoufox-dev.py identities");
    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    boost::system::error_code error;
    auto value = json::parse(text, error);
    const auto invalid = [] { return Error(503, "browser_not_configured", "Camoufox identity pool is invalid: regenerate it with scripts/camoufox-dev.py"); };
    if (error || !value.is_object()) throw invalid();
    const auto& object = value.as_object();
    const auto* schema = object.if_contains("schema");
    const auto* identities = object.if_contains("identities");
    if (!schema || !schema->is_int64() || schema->as_int64() != 1 || !identities || !identities->is_array() || identities->as_array().empty()) throw invalid();
    CamoufoxIdentityPool pool;
    if (auto* camoufox = object.if_contains("camoufox"); camoufox && camoufox->is_object()) pool.camoufox = camoufox->as_object();
    for (const auto& item : identities->as_array()) {
        if (!item.is_object()) throw invalid();
        const auto* config = item.as_object().if_contains("config");
        const auto* prefs = item.as_object().if_contains("prefs");
        if (!config || !config->is_object() || config->as_object().empty() || (prefs && !prefs->is_object())) throw invalid();
        pool.identities.push_back(item.as_object());
    }
    pool.sha256 = sha256(text);
    return pool;
}

json::object browser_detail::camoufox_launch_config(const json::object& identity_config, std::string_view locale, std::string_view timezone) {
    auto config = identity_config;
    if (!locale.empty()) {
        // BCP 47 subtags: language first, then an optional 4-letter script and
        // a 2-letter or 3-digit region.
        std::vector<std::string> parts;
        for (std::size_t start = 0; start <= locale.size();) {
            const auto end = std::min(locale.find('-', start), locale.size());
            parts.emplace_back(locale.substr(start, end - start));
            start = end + 1;
        }
        const auto language = lower(parts.front());
        std::string region;
        for (std::size_t i = 1; i < parts.size(); ++i)
            if (parts[i].size() == 2 || (parts[i].size() == 3 && std::all_of(parts[i].begin(), parts[i].end(), [](unsigned char c) { return std::isdigit(c); }))) {
                region = parts[i];
                std::transform(region.begin(), region.end(), region.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            }
        const std::string tag(locale);
        json::array languages{json::value(tag)};
        if (language != tag) languages.emplace_back(language);
        config["navigator.language"] = tag;
        config["navigator.languages"] = languages;
        config["headers.Accept-Language"] = language != tag ? tag + "," + language + ";q=0.9" : tag;
        config["locale:language"] = language;
        if (!region.empty()) config["locale:region"] = region;
        else config.erase("locale:region");
        config["locale:all"] = language != tag ? tag + ", " + language : tag;
    }
    if (!timezone.empty()) config["timezone"] = std::string(timezone);
    // The bridge runs in the page's own world, as AI Studio's code does.
    config["allowMainWorld"] = true;
    return config;
}
} // namespace ais2api
