// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/common.hpp"
#include <charconv>
#include <chrono>
#include <cstdint>
#include <string_view>
namespace ais2api::download_detail {
struct Options {
    unsigned connections = 4;
    fs::path ca_file;
    // Before the attempt after an interrupted one, doubled for each further
    // failure without progress.
    std::chrono::milliseconds retry_delay{1000};
};
void download_with_options(std::string_view url, const fs::path &destination,
                           std::string_view digest, std::string_view proxy,
                           std::uint64_t expected_size, const Options &options);
inline bool content_range(std::string_view value, std::uint64_t first, std::uint64_t last,
                          std::uint64_t total) {
    if (!value.starts_with("bytes ")) return false;
    value.remove_prefix(6);
    auto number = [&](char separator, std::uint64_t expected) {
        std::uint64_t actual{};
        auto [end, error] = std::from_chars(value.data(), value.data()+value.size(), actual);
        if (error != std::errc{} || actual != expected) return false;
        auto used = static_cast<std::size_t>(end-value.data());
        value.remove_prefix(used);
        if (separator) {
            if (value.empty() || value.front()!=separator) return false;
            value.remove_prefix(1);
        }
        return true;
    };
    return first <= last && last < total && number('-', first) && number('/', last) &&
           number(0, total) && value.empty();
}
}
