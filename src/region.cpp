// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/region.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ais2api {
namespace {
std::string environment(const char *name) {
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string{};
}

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

#ifndef _WIN32
// Android exposes region settings as system properties, not files or env.
std::string android_property(const char *name) {
    if (!fs::exists("/system/bin/getprop")) return {};
    const std::string command = std::string("/system/bin/getprop ") + name + " 2>/dev/null";
    FILE *pipe = ::popen(command.c_str(), "r");
    if (!pipe) return {};
    char buffer[128]{};
    std::string output;
    while (std::fgets(buffer, sizeof buffer, pipe)) output += buffer;
    ::pclose(pipe);
    return trimmed(output);
}
#endif
} // namespace

std::string region_detail::normalize_locale(std::string_view value) {
    std::string text(value.substr(0, value.find_first_of(".@")));
    std::replace(text.begin(), text.end(), '_', '-');
    if (text.empty() || text == "C" || text == "POSIX") return {};
    // Canonical case: language lower, 2-letter region upper, 4-letter script title.
    std::string result;
    std::size_t start = 0;
    for (bool first = true; start <= text.size(); first = false) {
        auto end = text.find('-', start);
        if (end == std::string::npos) end = text.size();
        auto part = text.substr(start, end - start);
        std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!first && part.size() == 2) std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (!first && part.size() == 4) part[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
        if (!result.empty()) result += '-';
        result += part;
        start = end + 1;
    }
    return valid_locale(result) ? result : std::string{};
}

std::string region_detail::zone_from_path(std::string_view path) {
    constexpr std::string_view marker = "zoneinfo/";
    const auto at = path.rfind(marker);
    if (at == std::string_view::npos) return {};
    std::string zone(path.substr(at + marker.size()));
    // Some distributions keep a "posix/" or "right/" variant tree.
    for (std::string_view prefix : {"posix/", "right/"})
        if (zone.starts_with(prefix)) zone.erase(0, prefix.size());
    return valid_timezone(zone) ? zone : std::string{};
}

bool valid_timezone(std::string_view value) {
    if (value.empty() || value.size() > 64 || value.front() == '/' || value.back() == '/' ||
        value.find("//") != std::string_view::npos || value.find("..") != std::string_view::npos)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(value.front()))) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '/' || c == '_' || c == '-' || c == '+';
    });
}

bool valid_locale(std::string_view value) {
    if (value.size() < 2 || value.size() > 35) return false;
    std::size_t start = 0;
    for (bool first = true; start <= value.size(); first = false) {
        auto end = value.find('-', start);
        if (end == std::string_view::npos) end = value.size();
        const auto part = value.substr(start, end - start);
        if (first ? (part.size() < 2 || part.size() > 3 || !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isalpha(c); }))
                  : (part.size() < 2 || part.size() > 8 || !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isalnum(c); })))
            return false;
        start = end + 1;
    }
    return true;
}

std::string_view fingerprint_platform() {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

std::string host_timezone() {
    // An explicit IANA TZ wins everywhere (POSIX rule strings are ignored).
    if (auto tz = environment("TZ"); !tz.empty()) {
        if (tz.front() == ':') tz.erase(0, 1);
        if (auto zone = region_detail::zone_from_path(tz); !zone.empty()) return zone;
        if (valid_timezone(tz) && tz.find('/') != std::string::npos) return tz;
    }
#ifdef _WIN32
    try {
        // MSVC maps the Windows zone to its IANA name through ICU.
        const auto name = std::string(std::chrono::current_zone()->name());
        if (valid_timezone(name)) return name;
    } catch (...) {
    }
#else
    if (auto zone = android_property("persist.sys.timezone"); valid_timezone(zone)) return zone;
    std::error_code ec;
    if (auto target = fs::read_symlink("/etc/localtime", ec); !ec)
        if (auto zone = region_detail::zone_from_path(target.string()); !zone.empty()) return zone;
    if (std::ifstream file("/etc/timezone"); file) {
        std::string line;
        std::getline(file, line);
        if (line = trimmed(line); valid_timezone(line)) return line;
    }
#endif
    return "UTC";
}

std::string host_locale() {
#ifdef _WIN32
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH]{};
    if (const int size = GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH); size > 1) {
        std::string ascii;
        for (int i = 0; i + 1 < size; ++i)
            if (buffer[i] < 128) ascii += static_cast<char>(buffer[i]);
        if (auto locale = region_detail::normalize_locale(ascii); !locale.empty()) return locale;
    }
#else
    for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (auto locale = region_detail::normalize_locale(environment(name)); !locale.empty()) return locale;
    for (const char *property : {"persist.sys.locale", "ro.product.locale"})
        if (auto locale = region_detail::normalize_locale(android_property(property)); !locale.empty()) return locale;
#endif
    return "en-US";
}
} // namespace ais2api
