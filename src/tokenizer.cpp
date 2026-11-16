// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/tokenizer.hpp"
#include "ais2api/common.hpp"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <queue>
#include <tuple>
#include <vector>

namespace ais2api {
namespace {
#include "gemma4_tokenizer.inc"

[[noreturn]] void corrupt() {
    throw Error(500, "tokenizer_corrupt", "The embedded tokenizer table is invalid");
}

// The table tools/tokenizer/derive_gemma4.py writes: single-character pieces,
// the byte-fallback base and the merges in rank order.
class Table {
  public:
    Table() {
        const unsigned char *source = embedded_gemma4_bpe;
        const auto size = ZSTD_getFrameContentSize(source, sizeof(embedded_gemma4_bpe));
        if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > 64u * 1024 * 1024)
            corrupt();
        std::vector<unsigned char> payload(static_cast<std::size_t>(size));
        const auto written = ZSTD_decompress(payload.data(), payload.size(), source, sizeof(embedded_gemma4_bpe));
        if (ZSTD_isError(written) || written != payload.size())
            corrupt();
        std::size_t at = 0;
        auto read = [&]() -> std::uint64_t {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 64; shift += 7) {
                if (at >= payload.size())
                    corrupt();
                const auto byte = payload[at++];
                value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
                if (!(byte & 0x80))
                    return value;
            }
            corrupt();
        };
        if (payload.size() < 5 || std::string_view(reinterpret_cast<const char *>(payload.data()), 4) != "A2TK" ||
            payload[4] != 1)
            corrupt();
        at = 5;
        const auto vocabulary = read();
        byte_base_ = static_cast<std::uint32_t>(read());
        if (vocabulary == 0 || vocabulary > 0xFFFFFFu || byte_base_ + 256 > vocabulary)
            corrupt();
        auto id = [&] {
            const auto value = read();
            if (value >= vocabulary)
                corrupt();
            return static_cast<std::uint32_t>(value);
        };
        const auto characters = read();
        if (characters > vocabulary)
            corrupt();
        codepoints_.resize(static_cast<std::size_t>(characters));
        char_ids_.resize(codepoints_.size());
        std::uint64_t codepoint = 0;
        for (auto &value : codepoints_) {
            codepoint += read();
            if (codepoint > 0x10FFFF || (&value != codepoints_.data() && codepoint <= *(&value - 1)))
                corrupt();
            value = static_cast<char32_t>(codepoint);
        }
        for (auto &value : char_ids_)
            value = id();
        ascii_.fill(none);
        for (std::size_t i = 0; i < codepoints_.size() && codepoints_[i] < 128; ++i)
            ascii_[codepoints_[i]] = char_ids_[i];
        const auto merges = read();
        if (merges == 0 || merges >= 0xFFFFFFFFu)
            corrupt();
        left_.resize(static_cast<std::size_t>(merges));
        right_.resize(left_.size());
        merged_.resize(left_.size());
        for (auto &value : left_)
            value = id();
        for (auto &value : right_)
            value = id();
        std::int64_t previous = 0;
        for (auto &value : merged_) {
            const auto zigzag = read();
            previous += (zigzag & 1) ? -static_cast<std::int64_t>((zigzag + 1) >> 1)
                                     : static_cast<std::int64_t>(zigzag >> 1);
            if (previous < 0 || static_cast<std::uint64_t>(previous) >= vocabulary)
                corrupt();
            value = static_cast<std::uint32_t>(previous);
        }
        if (at != payload.size())
            corrupt();
        // Open addressing from (left, right) to rank + 1, at most half full.
        std::size_t capacity = 1;
        while (capacity < left_.size() * 2)
            capacity <<= 1;
        slots_.assign(capacity, 0);
        mask_ = capacity - 1;
        for (std::uint32_t rank = 0; rank < left_.size(); ++rank) {
            auto slot = hash(left_[rank], right_[rank]) & mask_;
            while (slots_[slot]) {
                const auto other = slots_[slot] - 1;
                // derive_gemma4.py refuses a table that repeats a pair.
                if (left_[other] == left_[rank] && right_[other] == right_[rank])
                    corrupt();
                slot = (slot + 1) & mask_;
            }
            slots_[slot] = rank + 1;
        }
    }

    // The rank of merging `left` and `right`, or none.
    std::uint32_t rank(std::uint32_t left, std::uint32_t right) const {
        for (auto slot = hash(left, right) & mask_; slots_[slot]; slot = (slot + 1) & mask_) {
            const auto rank = slots_[slot] - 1;
            if (left_[rank] == left && right_[rank] == right)
                return rank;
        }
        return none;
    }
    std::uint32_t merged(std::uint32_t rank) const { return merged_[rank]; }
    std::uint32_t character(char32_t codepoint) const {
        if (codepoint < 128)
            return ascii_[codepoint];
        const auto it = std::lower_bound(codepoints_.begin(), codepoints_.end(), codepoint);
        return it != codepoints_.end() && *it == codepoint
                   ? char_ids_[static_cast<std::size_t>(it - codepoints_.begin())]
                   : none;
    }
    std::uint32_t byte(unsigned char value) const { return byte_base_ + value; }
    static constexpr std::uint32_t none = 0xFFFFFFFFu;

  private:
    static std::size_t hash(std::uint32_t left, std::uint32_t right) {
        auto value = (static_cast<std::uint64_t>(left) << 32 | right) * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(value ^ (value >> 29));
    }
    std::uint32_t byte_base_ = 0;
    std::array<std::uint32_t, 128> ascii_{};
    std::vector<char32_t> codepoints_;
    std::vector<std::uint32_t> char_ids_, left_, right_, merged_, slots_;
    std::size_t mask_ = 0;
};

const Table &table() {
    static const Table instance;
    return instance;
}

// Decodes one UTF-8 scalar value; returns 0 bytes consumed when invalid.
std::size_t decode(std::string_view text, std::size_t at, char32_t &out) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80) {
        out = lead;
        return 1;
    }
    const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 0;
    if (!length || lead > 0xF4 || at + length > text.size())
        return 0;
    char32_t value = lead & (0xFF >> (length + 1));
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0) != 0x80)
            return 0;
        value = (value << 6) | (next & 0x3F);
    }
    static constexpr char32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < minimum[length] || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return 0;
    out = value;
    return length;
}
} // namespace

std::size_t count_tokens(std::string_view utf8) {
    if (utf8.empty())
        return 0;
    const auto &t = table();
    // One symbol per character, or one per UTF-8 byte for characters outside
    // the vocabulary (byte fallback). The normalizer turns spaces into U+2581.
    std::vector<std::uint32_t> ids;
    ids.reserve(utf8.size());
    for (std::size_t at = 0; at < utf8.size();) {
        char32_t codepoint = 0;
        const auto length = decode(utf8, at, codepoint);
        if (!length) {
            ids.push_back(t.byte(static_cast<unsigned char>(utf8[at++])));
            continue;
        }
        const auto id = t.character(codepoint == U' ' ? U'▁' : codepoint);
        if (id != Table::none)
            ids.push_back(id);
        else if (codepoint == U' ') {
            for (unsigned char byte : std::string_view("\xE2\x96\x81"))
                ids.push_back(t.byte(byte));
        } else
            for (std::size_t i = 0; i < length; ++i)
                ids.push_back(t.byte(static_cast<unsigned char>(utf8[at + i])));
        at += length;
    }
    // BPE over the whole text as one word: repeatedly merge the adjacent pair
    // with the lowest rank, leftmost first, as HF tokenizers' Word::merge_all.
    const auto count = ids.size();
    std::vector<std::uint32_t> next(count), previous(count);
    for (std::size_t i = 0; i < count; ++i) {
        next[i] = static_cast<std::uint32_t>(i + 1);
        previous[i] = i ? static_cast<std::uint32_t>(i - 1) : Table::none;
    }
    using Entry = std::pair<std::uint32_t, std::uint32_t>; // rank, position
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    auto consider = [&](std::uint32_t position) {
        if (position == Table::none || next[position] >= count)
            return;
        const auto rank = t.rank(ids[position], ids[next[position]]);
        if (rank != Table::none)
            queue.emplace(rank, position);
    };
    for (std::uint32_t i = 0; i + 1 < count; ++i)
        consider(i);
    std::size_t remaining = count;
    while (!queue.empty()) {
        const auto [rank, position] = queue.top();
        queue.pop();
        // Skip entries made stale by an earlier merge.
        if (ids[position] == Table::none || next[position] >= count ||
            t.rank(ids[position], ids[next[position]]) != rank)
            continue;
        const auto absorbed = next[position];
        ids[position] = t.merged(rank);
        ids[absorbed] = Table::none;
        next[position] = next[absorbed];
        if (next[position] < count)
            previous[next[position]] = position;
        --remaining;
        consider(previous[position]);
        consider(position);
    }
    return remaining;
}
} // namespace ais2api
