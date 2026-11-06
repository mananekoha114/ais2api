// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <string>
#include <string_view>
namespace ais2api::browser_detail {
// Appends `text` as a JSON string literal, escaped exactly like json::serialize.
inline void append_quoted(std::string& out, std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    out += '"';
    std::size_t run = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        out.append(text.data() + run, i - run);
        run = i + 1;
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default: out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        }
    }
    out.append(text.data() + run, text.size() - run);
    out += '"';
}
// Length of the longest prefix of `text`, at most `limit` bytes, that does not
// split a UTF-8 sequence.
inline std::size_t utf8_prefix(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return text.size();
    auto end = limit;
    while (end > 0 && limit - end < 3 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    // Invalid input (a longer continuation run) is cut at the limit.
    return end && (static_cast<unsigned char>(text[end]) & 0xC0) != 0x80 ? end : limit;
}
} // namespace ais2api::browser_detail
