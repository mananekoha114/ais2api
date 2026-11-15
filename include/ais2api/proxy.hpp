// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace ais2api {
// One upstream proxy: scheme://[user[:password]@]host[:port][/].
struct ProxyEndpoint {
    std::string scheme;  // Lowercase; "http" when the text has no scheme.
    std::string host;    // Without IPv6 brackets.
    unsigned short port = 0;
    std::string username, password;  // Percent-decoded.
    bool credentials = false;
};

// Parses a single upstream proxy URL (http, https, socks4, socks5). Returns
// nothing for unsupported forms or multi-rule lists.
inline std::optional<ProxyEndpoint> parse_proxy(std::string_view text) {
    auto decode = [](std::string_view value) -> std::optional<std::string> {
        auto hex = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] != '%') { out += value[i]; continue; }
            if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0) return std::nullopt;
            out += static_cast<char>(hex(value[i + 1]) * 16 + hex(value[i + 2]));
            i += 2;
        }
        return out;
    };
    ProxyEndpoint result;
    result.scheme = "http";
    if (const auto separator = text.find("://"); separator != std::string_view::npos) {
        result.scheme = std::string(text.substr(0, separator));
        std::transform(result.scheme.begin(), result.scheme.end(), result.scheme.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        text.remove_prefix(separator + 3);
    }
    static constexpr std::string_view schemes[] = {"http", "https", "socks4", "socks4a", "socks5", "socks5h"};
    if (std::find(std::begin(schemes), std::end(schemes), result.scheme) == std::end(schemes)) return std::nullopt;
    auto authority = text.substr(0, text.find_first_of("/?#"));
    if (const auto rest = text.substr(authority.size()); !rest.empty() && rest != "/") return std::nullopt;
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        const auto userinfo = authority.substr(0, at);
        const auto colon = userinfo.find(':');
        auto user = decode(userinfo.substr(0, colon));
        auto password = decode(colon == std::string_view::npos ? std::string_view{} : userinfo.substr(colon + 1));
        if (!user || !password) return std::nullopt;
        result.username = std::move(*user);
        result.password = std::move(*password);
        result.credentials = true;
        authority.remove_prefix(at + 1);
    }
    std::string_view port;
    if (authority.starts_with('[')) {
        const auto end = authority.find(']');
        if (end == std::string_view::npos || end == 1) return std::nullopt;
        result.host = std::string(authority.substr(1, end - 1));
        if (!std::all_of(result.host.begin(), result.host.end(), [](unsigned char c) { return std::isxdigit(c) || c == ':' || c == '.'; }))
            return std::nullopt;
        const auto rest = authority.substr(end + 1);
        if (!rest.empty()) { if (rest.front() != ':') return std::nullopt; port = rest.substr(1); if (port.empty()) return std::nullopt; }
    } else {
        const auto colon = authority.find(':');
        result.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) { port = authority.substr(colon + 1); if (port.empty()) return std::nullopt; }
        if (result.host.empty() || result.host.size() > 253 ||
            !std::all_of(result.host.begin(), result.host.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == '_'; }))
            return std::nullopt;
    }
    if (port.empty()) {
        // Standard scheme defaults when the port is omitted.
        result.port = result.scheme == "http" ? 80 : result.scheme == "https" ? 443 : 1080;
    } else {
        unsigned value = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<unsigned>(c - '0');
            if (value > 65535) return std::nullopt;
        }
        if (!value) return std::nullopt;
        result.port = static_cast<unsigned short>(value);
    }
    return result;
}
// Upstream schemes the browser proxy relay can speak.
inline bool relay_supports(const ProxyEndpoint& endpoint) {
    if (endpoint.scheme == "http") return true;
    // RFC 1929 limits each credential to 255 bytes.
    return (endpoint.scheme == "socks5" || endpoint.scheme == "socks5h") &&
           endpoint.username.size() <= 255 && endpoint.password.size() <= 255 &&
           (!endpoint.credentials || !endpoint.username.empty());
}
} // namespace ais2api
