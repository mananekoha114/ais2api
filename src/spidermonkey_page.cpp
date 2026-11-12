// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The parts of the SpiderMonkey backend that need neither the engine nor the
// network. After AIStudio2API's internal/aistudio/runtime_go.go (MIT).
#include "ais2api/spidermonkey.hpp"
#include "../browser/rpc_error.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace ais2api::spidermonkey_detail {
namespace {
// A Firefox window's content area is this much lower than the window, as a
// Camoufox page of the same identity reports.
constexpr double firefox_chrome_height = 57;

[[noreturn]] void incompatible(std::string message) { throw Error(502, "browser_incompatible", std::move(message)); }
// A JSON+protobuf response body as JSON: without the XSSI guard, and with the
// leading hole some responses start with filled in.
std::optional<json::value> parse_response(std::string_view body) {
    constexpr std::string_view blank = " \t\r\n";
    const auto trim = [&](std::string_view text) {
        const auto begin = text.find_first_not_of(blank);
        return begin == std::string_view::npos ? std::string_view{} : text.substr(begin, text.find_last_not_of(blank) - begin + 1);
    };
    body = trim(body);
    if (body.starts_with(")]}'")) body = trim(body.substr(4));
    std::string filled;
    if (body.starts_with("[,")) {
        filled = "[null," + std::string(body.substr(2));
        body = filled;
    }
    boost::system::error_code failed;
    auto value = json::parse(body, failed);
    if (failed) return std::nullopt;
    return value;
}
void append_varint(std::string &out, std::uint64_t value) {
    while (value >= 0x80) {
        out += static_cast<char>(static_cast<unsigned char>(value) | 0x80);
        value >>= 7;
    }
    out += static_cast<char>(value);
}
std::string base64(std::string_view bytes) {
    std::string encoded((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                     reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}
std::string lowercase(std::string_view text) {
    std::string out(text);
    for (auto &character : out)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return out;
}
constexpr std::string_view origin = "https://aistudio.google.com";
std::optional<double> number_of(const json::object &object, std::string_view name) {
    const auto value = object.if_contains(name);
    if (!value || !value->is_number()) return std::nullopt;
    return value->to_number<double>();
}
} // namespace

std::string page_api_key(std::string_view html) {
    constexpr std::string_view key = "\"WIu0Nc\":\"";
    const auto start = html.find(key);
    if (start == std::string_view::npos) return {};
    const auto value = html.substr(start + key.size());
    const auto end = value.find('"');
    return end == std::string_view::npos ? std::string() : std::string(value.substr(0, end));
}

json::object page_globals(std::string_view html) {
    json::object globals;
    constexpr std::string_view toggles = "window[\"_F_toggles_default_MakerSuite\"] = [";
    if (const auto start = html.find(toggles); start != std::string_view::npos) {
        auto list = html.substr(start + toggles.size());
        if (const auto end = list.find(']'); end != std::string_view::npos) {
            list = list.substr(0, end);
            json::array values;
            while (!list.empty()) {
                const auto comma = list.find(',');
                auto item = list.substr(0, comma);
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
                while (!item.empty() && (item.front() == ' ' || item.front() == '\n' || item.front() == '\t')) item.remove_prefix(1);
                while (!item.empty() && (item.back() == ' ' || item.back() == '\n' || item.back() == '\t')) item.remove_suffix(1);
                // Decimal, or hexadecimal with 0x, as the script writes them.
                int base = 10;
                bool negative = false;
                if (item.starts_with('-')) {
                    negative = true;
                    item.remove_prefix(1);
                }
                if (item.starts_with("0x") || item.starts_with("0X")) {
                    base = 16;
                    item.remove_prefix(2);
                }
                std::int64_t value = 0;
                const auto parsed = std::from_chars(item.data(), item.data() + item.size(), value, base);
                if (item.empty() || parsed.ec != std::errc() || parsed.ptr != item.data() + item.size()) continue;
                values.emplace_back(negative ? -value : value);
            }
            globals["_F_toggles_default_MakerSuite"] = std::move(values);
        }
    }
    for (const std::string_view name : {"_F_cssRowKey", "_F_combinedSignature"}) {
        const auto declaration = "var " + std::string(name) + " = '";
        const auto start = html.find(declaration);
        if (start == std::string_view::npos) continue;
        const auto value = html.substr(start + declaration.size());
        if (const auto end = value.find('\''); end != std::string_view::npos) globals[name] = value.substr(0, end);
    }
    return globals;
}

bool requires_trusted_types(const std::vector<std::string> &policies) {
    for (const std::string_view policy : policies) {
        std::size_t start = 0;
        while (start <= policy.size()) {
            const auto end = std::min(policy.find_first_of(";,", start), policy.size());
            // The directive's name, then its values, separated by spaces.
            std::vector<std::string> fields;
            std::string field;
            for (const char character : policy.substr(start, end - start)) {
                if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                    if (!field.empty()) fields.push_back(std::move(field));
                    field.clear();
                } else {
                    field += character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
                }
            }
            if (!field.empty()) fields.push_back(std::move(field));
            if (fields.size() > 1 && fields.front() == "require-trusted-types-for" &&
                std::find(fields.begin() + 1, fields.end(), "'script'") != fields.end())
                return true;
            start = end + 1;
        }
    }
    return false;
}

std::string new_visit_id() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1) throw Error(500, "random_failed", "Could not generate a visit id");
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    for (std::size_t index = 0; index < sizeof bytes; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) uuid += '-';
        uuid += hex[bytes[index] >> 4];
        uuid += hex[bytes[index] & 15];
    }
    return "v1_" + base64(uuid);
}

std::string encode_jspb_fields(std::string_view text) {
    const auto parsed = parse_response(text);
    if (!parsed || !parsed->is_array()) incompatible("GetLoggingContext returned an unsupported response");
    std::string out;
    std::uint64_t number = 0;
    for (const auto &field : parsed->as_array()) {
        ++number;
        if (field.is_null()) continue;
        if (field.is_string()) {
            append_varint(out, number << 3 | 2);
            append_varint(out, field.as_string().size());
            out.append(field.as_string().data(), field.as_string().size());
        } else if (field.is_bool()) {
            append_varint(out, number << 3);
            append_varint(out, field.as_bool() ? 1 : 0);
        } else if (field.is_int64()) {
            append_varint(out, number << 3);
            append_varint(out, static_cast<std::uint64_t>(field.as_int64()));
        } else if (field.is_uint64()) {
            append_varint(out, number << 3);
            append_varint(out, field.as_uint64());
        } else {
            incompatible("GetLoggingContext field " + std::to_string(number) + " is not a scalar");
        }
    }
    return out;
}

std::string document_cookie(const json::array &stored, std::int64_t now) {
    constexpr std::string_view host = "aistudio.google.com";
    std::string result;
    for (const auto &value : stored) {
        if (!value.is_object()) continue;
        const auto &cookie = value.as_object();
        if (bool_or(cookie, "httpOnly")) continue;
        auto domain = string_or(cookie, "domain");
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        for (auto &character : domain)
            if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        if (domain.empty() || (host != domain && !host.ends_with("." + domain))) continue;
        if (const auto path = string_or(cookie, "path"); !path.empty() && path != "/") continue;
        if (const auto expires = number_of(cookie, "expires"); expires && *expires > 0 && *expires <= static_cast<double>(now)) continue;
        const auto name = string_or(cookie, "name");
        if (name.empty()) continue;
        if (!result.empty()) result += "; ";
        result += name + "=" + string_or(cookie, "value");
    }
    return result;
}

std::string benefit_tier(std::string_view body) {
    const auto parsed = parse_response(body);
    if (!parsed || !parsed->is_array()) return "unknown";
    const auto &fields = parsed->as_array();
    if (fields.empty() || fields[0].is_null()) return "free";
    if (!fields[0].is_int64()) return "unknown";
    switch (fields[0].as_int64()) {
    case 0: return "free";
    case 1: return "pro";
    case 2: return "ultra";
    case 3: return "plus";
    default: return "unknown";
    }
}

json::array discovered_models(std::string_view body) {
    const auto parsed = parse_response(body);
    if (!parsed || !parsed->is_array() || parsed->as_array().empty() || !parsed->as_array()[0].is_array())
        incompatible("ListModels returned an unsupported response");
    json::array models;
    for (const auto &item : parsed->as_array()[0].as_array()) {
        if (!item.is_array() || item.as_array().empty() || !item.as_array()[0].is_string()) continue;
        const auto &row = item.as_array();
        const std::string name(row[0].as_string());
        if (!name.starts_with("models/")) continue;
        const auto id = name.substr(7);
        const auto text = [&](std::size_t index, std::string fallback) {
            return row.size() > index && row[index].is_string() ? std::string(row[index].as_string()) : std::move(fallback);
        };
        const auto limit = [&](std::size_t index) -> json::value {
            if (row.size() > index && row[index].is_number()) return row[index];
            return 0;
        };
        models.push_back(json::object{{"id", id}, {"name", name}, {"displayName", text(3, id)}, {"description", text(4, {})},
                                      {"inputTokenLimit", limit(5)}, {"outputTokenLimit", limit(6)},
                                      {"supportedGenerationMethods", row.size() > 7 && row[7].is_array() ? row[7] : json::value(json::array{})},
                                      {"source", "live"}});
    }
    return models;
}

std::string_view fetch_site(std::string_view url) {
    const auto scheme = url.find("://");
    auto host = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/?#:"));
    if (host == "aistudio.google.com") return "same-origin";
    return host == "google.com" || host.ends_with(".google.com") ? "same-site" : "cross-site";
}

std::string page_prompt(const Request &request) {
    const auto contents = request.gemini.if_contains("contents");
    if (!contents || !contents->is_array() || contents->as_array().empty()) return {};
    const auto &last = contents->as_array().back();
    if (!last.is_object()) return {};
    if (const auto role = string_or(last.as_object(), "role"); !role.empty() && role != "user") return {};
    const auto parts = last.as_object().if_contains("parts");
    if (!parts || !parts->is_array()) return {};
    std::string prompt;
    for (const auto &part : parts->as_array())
        if (part.is_object()) prompt += string_or(part.as_object(), "text");
    return prompt;
}

std::string last_modified(std::chrono::system_clock::time_point time, std::string_view timezone) {
    using namespace std::chrono;
    const auto instant = floor<seconds>(time);
    seconds offset{0};
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    try {
        offset = locate_zone(timezone)->get_info(instant).offset;
    } catch (const std::exception &) {
        // A zone this system does not know reads as UTC.
    }
#else
    (void)timezone;
#endif
    const auto local = instant + offset;
    const auto day = floor<days>(local);
    const year_month_day date{day};
    const hh_mm_ss clock{local - day};
    char text[32];
    std::snprintf(text, sizeof text, "%02u/%02u/%04d %02d:%02d:%02d", static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<int>(date.year()), static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
                  static_cast<int>(clock.seconds().count()));
    return text;
}

HostIdentity host_identity(const json::object &config) {
    HostIdentity result;
    auto &profile = result.profile;
    for (const auto &item : config) {
        const std::string_view key = item.key();
        const auto dot = key.find('.');
        if (dot == std::string_view::npos) continue;
        const auto scope = key.substr(0, dot), name = key.substr(dot + 1);
        if (scope == "navigator") profile.navigator[name] = item.value();
        else if (scope == "screen") profile.screen[name] = item.value();
        else if (scope == "window") profile.window[name] = item.value();
    }
    for (const std::string_view name : {"availLeft", "availTop"})
        if (!profile.screen.contains(name)) profile.screen[name] = 0;
    profile.navigator["doNotTrack"] = "unspecified";
    profile.navigator["globalPrivacyControl"] = false;
    // The page fills the window: no sidebar, and Firefox's own toolbars above.
    if (const auto width = number_of(profile.window, "outerWidth")) profile.window["innerWidth"] = *width;
    if (const auto height = number_of(profile.window, "outerHeight")) profile.window["innerHeight"] = *height - firefox_chrome_height;
    profile.locale = string_or(config, "navigator.language");
    if (profile.locale.empty()) profile.locale = "en-US";
    profile.timezone = string_or(config, "timezone");
    profile.has_focus = true;
    result.user_agent = string_or(config, "navigator.userAgent");
    result.accept_language = string_or(config, "headers.Accept-Language");
    if (result.accept_language.empty()) result.accept_language = "en-US,en;q=0.5";
    return result;
}
std::string page_email(std::string_view html) {
    constexpr std::string_view key = "\"oPEP7c\":\"";
    const auto start = html.find(key);
    if (start == std::string_view::npos) return {};
    // The JSON string, escapes and all: Google escapes "@" as backslash-u0040.
    const auto value = start + key.size() - 1;
    for (auto end = value + 1; end < html.size(); ++end) {
        if (html[end] == '\\') {
            ++end;
            continue;
        }
        if (html[end] != '"') continue;
        boost::system::error_code failed;
        const auto parsed = json::parse(html.substr(value, end - value + 1), failed);
        return failed || !parsed.is_string() ? std::string() : std::string(parsed.as_string());
    }
    return {};
}

std::string resolve_location(std::string_view base, std::string_view location) {
    if (location.empty()) return {};
    // Absolute: a scheme and a colon.
    if (const auto colon = location.find(':'); colon != std::string_view::npos && colon > 0) {
        const auto scheme = location.substr(0, colon);
        if (std::isalpha(static_cast<unsigned char>(scheme.front())) &&
            std::all_of(scheme.begin(), scheme.end(), [](unsigned char c) { return std::isalnum(c) || c == '+' || c == '-' || c == '.'; }))
            return std::string(location);
    }
    const auto separator = base.find("://");
    if (separator == std::string_view::npos) return std::string(location);
    const auto scheme = base.substr(0, separator);
    const auto rest = base.substr(separator + 3);
    const auto authority = rest.substr(0, rest.find_first_of("/?#"));
    if (location.starts_with("//")) return std::string(scheme) + ":" + std::string(location);
    const auto prefix = std::string(scheme) + "://" + std::string(authority);
    if (location.starts_with('/')) return prefix + std::string(location);
    auto path = rest.substr(authority.size());
    path = path.substr(0, path.find_first_of("?#"));
    if (location.starts_with('?')) return prefix + std::string(path.empty() ? "/" : path) + std::string(location);
    const auto slash = path.rfind('/');
    const auto directory = slash == std::string_view::npos ? std::string_view("/") : path.substr(0, slash + 1);
    return prefix + std::string(directory) + std::string(location);
}

std::optional<Error> page_rpc_failure(std::string_view method, long status, std::string_view body) {
    if (const auto parsed = parse_response(body); parsed && parsed->is_array()) {
        const auto &fields = parsed->as_array();
        if (fields.size() >= 2 && fields[0].is_int64() && fields[1].is_string()) {
            const auto message = lowercase(fields[1].as_string());
            if (message.find("region not supported") != std::string::npos)
                return Error(403, "region_blocked", "AI Studio is unavailable in the account's region");
            if (message.find("account not supported") != std::string::npos)
                return Error(403, "account_unsupported", "AI Studio does not support this account");
        }
    }
    if (status == 200) return std::nullopt;
    if (auto rpc = browser_detail::rpc_text_error(body)) return rpc;
    if (auto http = browser_detail::rpc_status_error(status, 0, body)) return http;
    return Error(502, "upstream_error", std::string(method) + " returned HTTP " + std::to_string(status));
}

std::string count_tokens_body(std::string_view model, std::string_view prompt) {
    // Built level by level: a one-element initializer list would copy its
    // element instead of nesting it.
    json::array part{nullptr, prompt};
    json::array parts;
    parts.push_back(std::move(part));
    json::array content;
    content.push_back(std::move(parts));
    content.emplace_back("user");
    json::array contents;
    contents.push_back(std::move(content));
    json::array body;
    body.emplace_back(model);
    body.push_back(std::move(contents));
    return json::serialize(body);
}

std::string_view tier_header(std::string_view tier) {
    if (tier == "plus") return "TIER0";
    if (tier == "pro") return "TIER1";
    if (tier == "ultra") return "TIER2";
    return {};
}
json::object relay_state(const RelayState &state) {
    return {{"pending", nullptr},
            {"done", state.done},
            {"error", state.error},
            {"errorBody", state.error_body},
            {"status", static_cast<std::int64_t>(state.status)},
            {"retryAfter", static_cast<std::int64_t>(state.retry_after)},
            {"benefitTier", state.benefit}};
}
json::object relay_chunk(std::uint64_t sequence, std::string_view bytes) {
    return {{"sequence", static_cast<std::int64_t>(sequence)}, {"bytes", base64(bytes)}};
}

namespace {
Headers leading(const PageHeaders &page, std::string_view accept) {
    return {{"User-Agent", page.user_agent}, {"Accept", std::string(accept)}, {"Accept-Language", page.accept_language},
            {"Accept-Encoding", page.accept_encoding}};
}
constexpr std::string_view html_accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
std::string referer() { return std::string(origin) + "/"; }
} // namespace

Headers document_headers(const PageHeaders &page) {
    auto headers = leading(page, html_accept);
    headers.insert(headers.end(), {{"Cookie", ""}, {"Upgrade-Insecure-Requests", "1"}, {"Sec-Fetch-Dest", "document"},
                                   {"Sec-Fetch-Mode", "navigate"}, {"Sec-Fetch-Site", "none"}, {"Sec-Fetch-User", "?1"},
                                   {"Priority", "u=0, i"}, {"TE", "trailers"}});
    return headers;
}

Headers frame_headers(const PageHeaders &page, std::string_view url) {
    auto headers = leading(page, html_accept);
    headers.insert(headers.end(), {{"Referer", referer()}, {"Cookie", ""}, {"Upgrade-Insecure-Requests", "1"},
                                   {"Sec-Fetch-Dest", "iframe"}, {"Sec-Fetch-Mode", "navigate"}, {"Sec-Fetch-Site", std::string(fetch_site(url))},
                                   {"Priority", "u=4"}, {"TE", "trailers"}});
    return headers;
}

Headers image_headers(const PageHeaders &page, std::string_view url) {
    auto headers = leading(page, "image/avif,image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5");
    headers.insert(headers.end(), {{"Referer", referer()}, {"Cookie", ""}, {"Sec-Fetch-Dest", "image"}, {"Sec-Fetch-Mode", "no-cors"},
                                   {"Sec-Fetch-Site", std::string(fetch_site(url))}, {"Priority", "u=5, i"}, {"TE", "trailers"}});
    return headers;
}

Headers script_headers(const PageHeaders &page, std::string_view url) {
    auto headers = leading(page, "*/*");
    headers.insert(headers.end(), {{"Referer", referer()}, {"Cookie", ""}, {"Sec-Fetch-Dest", "script"}, {"Sec-Fetch-Mode", "no-cors"},
                                   {"Sec-Fetch-Site", std::string(fetch_site(url))}, {"TE", "trailers"}});
    return headers;
}

Headers rpc_headers(const PageHeaders &page, const RpcContext &rpc) {
    auto headers = leading(page, "*/*");
    headers.insert(headers.end(), {{"Referer", referer()}, {"Content-Type", "application/json+protobuf"}, {"X-Goog-Api-Key", rpc.api_key},
                                   {"X-Goog-AuthUser", rpc.auth_user}, {"X-User-Agent", "grpc-web-javascript/0.1"}});
    if (!rpc.authorization.empty()) headers.emplace_back("Authorization", rpc.authorization);
    if (!rpc.visit_id.empty()) headers.emplace_back("X-AIStudio-Visit-Id", rpc.visit_id);
    if (!rpc.extension.empty()) headers.emplace_back("X-Goog-Ext-519733851-Bin", rpc.extension);
    if (!rpc.tier.empty()) headers.emplace_back("X-AIStudio-G1-Tier", rpc.tier);
    headers.insert(headers.end(), {{"Origin", std::string(origin)}, {"Cookie", ""}, {"Sec-Fetch-Dest", "empty"}, {"Sec-Fetch-Mode", "cors"},
                                   {"Sec-Fetch-Site", "same-site"}, {"Priority", rpc.priority}, {"TE", "trailers"}});
    return headers;
}
} // namespace ais2api::spidermonkey_detail
