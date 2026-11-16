// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/common.hpp"
#include "ais2api/tokenizer.hpp"
#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>

using namespace ais2api;

namespace {
struct Sample {
    const char *text;
    std::size_t count;
};

// Counts from Hugging Face tokenizers on the pinned tokenizer.json
// (tools/tokenizer/reference_counts.py, add_special_tokens=False).
constexpr Sample samples[] = {
    {"Hello, world!", 4},
    {" leading and trailing spaces  ", 5},
    {"The quick brown fox jumps over the lazy dog.\n", 11},
    {"你好，世界！今天的天气很好。", 9},
    {"日本語のテキストとカタカナ、ひらがなを数えます。", 14},
    {"def add(a, b):\n    return a + b\n", 14},
    {"{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}", 14},
    {"\U0001F600\U0001F468‍\U0001F469‍\U0001F467‍\U0001F466\U0001F3F3️‍\U0001F308", 12},
    {"1234567890 3.14159 2026-11-16", 29},
    {"\n\n\n\t\t    \n", 4},
    // Neither character is in the vocabulary: four byte pieces each.
    {"\U0002A6D6\U000E0001", 8},
    {"a▁b ▁ c", 4},
    {"Ünïcödé façade naïve café", 9},
};
} // namespace

TEST_CASE("token counts match the reference Gemma 4 tokenizer", "[tokenizer]") {
    for (const auto &sample : samples) {
        INFO(sample.text);
        CHECK(count_tokens(sample.text) == sample.count);
    }
    CHECK(count_tokens("") == 0);
    // Counting is per call; repeating the text repeats its merges.
    CHECK(count_tokens("Hello, world!Hello, world!") > count_tokens("Hello, world!"));
}

TEST_CASE("invalid UTF-8 counts as byte pieces", "[tokenizer]") {
    CHECK(count_tokens("\xFF") == 1);
    CHECK(count_tokens("\xC3") == 1);
    CHECK(count_tokens("\xE4\xBD") == 2);
    CHECK(count_tokens("\xED\xA0\x80") == 3); // an encoded surrogate
    CHECK(count_tokens("\xC0\xAF") == 2);     // an overlong encoding
    CHECK(count_tokens(std::string("a\0b", 3)) == count_tokens("a") + count_tokens(std::string(1, '\0')) +
                                                      count_tokens("b"));
}

// Hidden by default: AIS2API_TOKENIZER_CORPUS names the output of
// tools/tokenizer/reference_counts.py, and every text must match exactly.
TEST_CASE("token counts match a reference corpus", "[.tokenizer-corpus]") {
    const auto path = std::getenv("AIS2API_TOKENIZER_CORPUS");
    REQUIRE(path != nullptr);
    const auto corpus = boost::json::parse(read_file(fs::u8path(path), 256 * 1024 * 1024));
    std::size_t mismatches = 0, total = 0;
    for (const auto &entry : corpus.as_array()) {
        const auto &object = entry.as_object();
        const auto text = std::string(object.at("text").as_string());
        const auto expected = static_cast<std::size_t>(object.at("count").to_number<std::uint64_t>());
        const auto actual = count_tokens(text);
        ++total;
        if (actual != expected && ++mismatches <= 5) {
            INFO(text.substr(0, 200));
            CHECK(actual == expected);
        }
    }
    INFO(mismatches << " of " << total << " texts differ");
    CHECK(mismatches == 0);
    CHECK(total > 0);
}
