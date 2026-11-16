// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "common.hpp"

namespace ais2api {
// Region settings of the computer running ais2api, used as the default browser
// fingerprint timezone and locale. Detection never fails: it falls back to
// UTC and en-US.
std::string host_timezone();
std::string host_locale();

// Firefox identity host OS label: "windows" on Windows, "linux" elsewhere.
std::string_view fingerprint_platform();

// IANA zone names such as "Asia/Tokyo", "America/Argentina/Buenos_Aires" or "UTC".
bool valid_timezone(std::string_view value);
// BCP 47 language tags such as "zh-CN", "en-US" or "zh-Hant-TW".
bool valid_locale(std::string_view value);

namespace region_detail {
// "zh_CN.UTF-8@pinyin" -> "zh-CN"; returns empty for C/POSIX or invalid input.
std::string normalize_locale(std::string_view value);
// "/usr/share/zoneinfo/Asia/Tokyo" -> "Asia/Tokyo"; empty when not a zoneinfo path.
std::string zone_from_path(std::string_view path);
} // namespace region_detail
} // namespace ais2api
