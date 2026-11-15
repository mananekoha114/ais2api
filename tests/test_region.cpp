// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/region.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;

TEST_CASE("host region detection always yields usable browser values", "[fingerprint]") {
    const auto timezone = host_timezone();
    const auto locale = host_locale();
    INFO(timezone << " " << locale);
    CHECK(valid_timezone(timezone));
    CHECK(valid_locale(locale));
#ifdef _WIN32
    CHECK(fingerprint_platform() == "windows");
#else
    CHECK(fingerprint_platform() == "linux");
#endif
}

TEST_CASE("system locale strings normalize to language tags", "[fingerprint]") {
    using region_detail::normalize_locale;
    CHECK(normalize_locale("zh_CN.UTF-8") == "zh-CN");
    CHECK(normalize_locale("en_us.utf8@euro") == "en-US");
    CHECK(normalize_locale("ja-JP") == "ja-JP");
    CHECK(normalize_locale("zh-hant-tw") == "zh-Hant-TW");
    CHECK(normalize_locale("de") == "de");
    CHECK(normalize_locale("C").empty());
    CHECK(normalize_locale("POSIX").empty());
    CHECK(normalize_locale("C.UTF-8").empty());
    CHECK(normalize_locale("").empty());
    CHECK(normalize_locale("x").empty());
    CHECK(normalize_locale("en_US; rm -rf").empty());
}

TEST_CASE("zoneinfo paths map to IANA names", "[fingerprint]") {
    using region_detail::zone_from_path;
    CHECK(zone_from_path("/usr/share/zoneinfo/Asia/Tokyo") == "Asia/Tokyo");
    CHECK(zone_from_path("../usr/share/zoneinfo/America/Argentina/Buenos_Aires") == "America/Argentina/Buenos_Aires");
    CHECK(zone_from_path("/usr/share/zoneinfo/posix/Europe/Berlin") == "Europe/Berlin");
    CHECK(zone_from_path("/etc/localtime").empty());
    CHECK(zone_from_path("/usr/share/zoneinfo/").empty());
}

TEST_CASE("timezone and locale validation rejects unsafe browser arguments", "[fingerprint]") {
    for (const auto* zone : {"UTC", "Asia/Shanghai", "America/Port-au-Prince", "Etc/GMT+8", "America/Argentina/Buenos_Aires"})
        CHECK(valid_timezone(zone));
    for (const auto* zone : {"", "/Asia/Tokyo", "Asia/", "Asia//Tokyo", "../etc/passwd", "Asia/Tokyo --flag", "8Asia",
                             "Asia/Tokyo\n", "Asia/Tōkyō"})
        CHECK_FALSE(valid_timezone(zone));
    CHECK_FALSE(valid_timezone(std::string(65, 'A')));
    for (const auto* locale : {"en", "zh-CN", "zh-Hans-CN", "es-419", "en-US-posix"})
        CHECK(valid_locale(locale));
    for (const auto* locale : {"", "e", "1n-US", "en_US", "en-", "en--US", "en-US x", "english-US-toolongsubtag"})
        CHECK_FALSE(valid_locale(locale));
}
