// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <chrono>
#include <cstdint>

namespace ais2api::browser_detail {
// Modern US DST rules, independent of the host's optional timezone database.
inline std::int64_t pacific_quota_day(std::int64_t timestamp) {
    using namespace std::chrono;
    const sys_seconds instant{seconds{timestamp}};
    const auto current_year = year_month_day{floor<days>(instant)}.year();
    const auto spring = sys_days{current_year / March / Sunday[2]} + hours{10};
    const auto autumn = sys_days{current_year / November / Sunday[1]} + hours{9};
    const auto offset = instant >= spring && instant < autumn ? hours{7} : hours{8};
    return floor<days>(instant - offset).time_since_epoch().count();
}
// Unix seconds of the next midnight America/Los_Angeles after `timestamp`,
// when Playground daily quotas reset. DST changes at 02:00 local, so the
// midnight that starts a changeover day still has the previous offset.
inline std::int64_t next_pacific_reset(std::int64_t timestamp) {
    using namespace std::chrono;
    const auto day = pacific_quota_day(timestamp) + 1;
    const sys_seconds daylight = sys_days{days{day}} + hours{7};
    const auto midnight = pacific_quota_day(daylight.time_since_epoch().count()) == day ? daylight : daylight + hours{1};
    return midnight.time_since_epoch().count();
}
} // namespace ais2api::browser_detail
