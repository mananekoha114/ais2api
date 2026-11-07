// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Small fpgen models written by hand, compressed the way a release is.
#pragma once
#include "ais2api/fpgen.hpp"
#include <catch2/catch_test_macros.hpp>
#include <zstd.h>
#include <cstdio>
#include <fstream>

namespace fpgen_fixture {
using namespace ais2api;
using fpgen::ValueId;
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-fpgen-test-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
// fpgen's index for a value: its position as minimal big-endian bytes in
// base85 without padding.
inline std::string code(ValueId value) {
    static constexpr std::string_view alphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";
    std::size_t length = 1;
    while (length < 4 && value >> (8 * length)) ++length;
    const std::uint64_t word = static_cast<std::uint64_t>(value) << (8 * (4 - length));
    std::string digits(5, '0');
    auto rest = word;
    for (int i = 4; i >= 0; --i) {
        digits[static_cast<std::size_t>(i)] = alphabet[rest % 85];
        rest /= 85;
    }
    return digits.substr(0, length + 1);
}
// A model directory: `values` are the JSON texts in index order, `nodes` the
// network with value indices written as positions.
struct Fixture {
    Directory directory;
    std::map<std::string, std::string> sha256s;
    std::vector<std::string> values;

    ValueId add(std::string text) {
        values.push_back(std::move(text));
        return static_cast<ValueId>(values.size() - 1);
    }
    void write(const std::string &name, std::string_view contents) {
        std::string compressed(ZSTD_compressBound(contents.size()), '\0');
        const auto size = ZSTD_compress(compressed.data(), compressed.size(), contents.data(), contents.size(), 3);
        REQUIRE_FALSE(ZSTD_isError(size));
        compressed.resize(size);
        std::ofstream(directory.path / name, std::ios::binary) << compressed;
        sha256s[name] = sha256(compressed);
    }
    fpgen::Model load(const json::array &nodes) {
        std::string dat;
        json::object spans;
        char offset[32];
        for (const auto &value : values) {
            std::snprintf(offset, sizeof offset, "%zx", dat.size());
            spans[offset] = value.size();
            dat += value;
        }
        write("values.json.zst", json::serialize(spans));
        write("values.dat.zst", dat);
        write("fingerprint-network.json.zst", json::serialize(json::object{{"nodes", nodes}}));
        return fpgen::Model::load(directory.path, sha256s);
    }
};
inline json::object table(std::initializer_list<std::pair<ValueId, double>> entries) {
    json::object result;
    for (const auto &[value, probability] : entries) result[code(value)] = probability;
    return result;
}
inline json::array codes(std::initializer_list<ValueId> values) {
    json::array result;
    for (const auto value : values) result.push_back(json::value(code(value)));
    return result;
}
inline json::object node(std::string name, json::array parents, json::array values, json::object probabilities) {
    return {{"name", std::move(name)}, {"parentNames", std::move(parents)}, {"possibleValues", std::move(values)},
            {"conditionalProbabilities", std::move(probabilities)}};
}
} // namespace fpgen_fixture
