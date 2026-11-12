// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "text.hpp"
#include <algorithm>
#include <array>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace ais2api::waa {
namespace {
constexpr std::string_view replacement = "\xEF\xBF\xBD"; // U+FFFD

void append_utf8(std::string &out, char32_t point) {
    if (point < 0x80) {
        out += static_cast<char>(point);
    } else if (point < 0x800) {
        out += static_cast<char>(0xC0 | (point >> 6));
        out += static_cast<char>(0x80 | (point & 0x3F));
    } else if (point < 0x10000) {
        out += static_cast<char>(0xE0 | (point >> 12));
        out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (point >> 18));
        out += static_cast<char>(0x80 | ((point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (point & 0x3F));
    }
}
// The WHATWG UTF-8 decoder: each maximal invalid prefix becomes one U+FFFD.
std::string decode_utf8(std::string_view data, bool &invalid) {
    std::string out;
    out.reserve(data.size());
    std::size_t i = 0;
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(data[at]); };
    while (i < data.size()) {
        const unsigned char lead = byte(i);
        if (lead < 0x80) {
            out += static_cast<char>(lead);
            ++i;
            continue;
        }
        unsigned needed = 0;
        unsigned char lower = 0x80, upper = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) needed = 1;
        else if (lead >= 0xE0 && lead <= 0xEF) {
            needed = 2;
            if (lead == 0xE0) lower = 0xA0;
            if (lead == 0xED) upper = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            needed = 3;
            if (lead == 0xF0) lower = 0x90;
            if (lead == 0xF4) upper = 0x8F;
        }
        std::size_t length = 1;
        bool complete = needed != 0;
        for (unsigned k = 0; k < needed; ++k) {
            if (i + length >= data.size() || byte(i + length) < lower || byte(i + length) > upper) {
                complete = false;
                break;
            }
            ++length;
            lower = 0x80;
            upper = 0xBF;
        }
        if (complete) {
            out.append(data.substr(i, length));
        } else {
            out += replacement;
            invalid = true;
        }
        i += length;
    }
    return out;
}
std::string decode_utf16(std::string_view data, bool big_endian, bool &invalid) {
    std::string out;
    out.reserve(data.size());
    const auto unit = [&](std::size_t at) {
        const auto first = static_cast<unsigned char>(data[at]), second = static_cast<unsigned char>(data[at + 1]);
        return static_cast<char32_t>(big_endian ? (first << 8) | second : (second << 8) | first);
    };
    std::size_t i = 0;
    for (; i + 1 < data.size(); i += 2) {
        const char32_t code = unit(i);
        if (code >= 0xD800 && code <= 0xDBFF && i + 3 < data.size()) {
            const char32_t low = unit(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                append_utf8(out, 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (code >= 0xD800 && code <= 0xDFFF) {
            out += replacement;
            invalid = true;
        } else {
            append_utf8(out, code);
        }
    }
    if (i < data.size()) {
        out += replacement;
        invalid = true;
    }
    return out;
}
std::string decode_windows_1252(std::string_view data) {
    // 0x80-0x9F, as the WHATWG index maps them.
    static constexpr std::array<char16_t, 32> high{
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    std::string out;
    out.reserve(data.size());
    for (const char character : data) {
        const auto byte = static_cast<unsigned char>(character);
        append_utf8(out, byte >= 0x80 && byte <= 0x9F ? high[byte - 0x80] : byte);
    }
    return out;
}
struct Label {
    std::string_view label, name;
};
constexpr Label labels[] = {
    {"unicode-1-1-utf-8", "utf-8"}, {"unicode11utf8", "utf-8"}, {"unicode20utf8", "utf-8"}, {"utf-8", "utf-8"},
    {"utf8", "utf-8"}, {"x-unicode20utf8", "utf-8"},
    {"csunicode", "utf-16le"}, {"iso-10646-ucs-2", "utf-16le"}, {"ucs-2", "utf-16le"}, {"unicode", "utf-16le"},
    {"unicodefeff", "utf-16le"}, {"utf-16", "utf-16le"}, {"utf-16le", "utf-16le"},
    {"unicodefffe", "utf-16be"}, {"utf-16be", "utf-16be"},
    {"ansi_x3.4-1968", "windows-1252"}, {"ascii", "windows-1252"}, {"cp1252", "windows-1252"},
    {"cp819", "windows-1252"}, {"csisolatin1", "windows-1252"}, {"ibm819", "windows-1252"},
    {"iso-8859-1", "windows-1252"}, {"iso-ir-100", "windows-1252"}, {"iso8859-1", "windows-1252"},
    {"iso88591", "windows-1252"}, {"iso_8859-1", "windows-1252"}, {"iso_8859-1:1987", "windows-1252"},
    {"l1", "windows-1252"}, {"latin1", "windows-1252"}, {"us-ascii", "windows-1252"},
    {"windows-1252", "windows-1252"}, {"x-cp1252", "windows-1252"}};

void append_sorted(std::string &out, const json::value &value) {
    if (value.is_object()) {
        std::vector<const json::key_value_pair *> members;
        members.reserve(value.as_object().size());
        for (const auto &member : value.as_object()) members.push_back(&member);
        std::sort(members.begin(), members.end(), [](const auto *a, const auto *b) { return a->key() < b->key(); });
        out += '{';
        bool first = true;
        for (const auto *member : members) {
            if (!first) out += ',';
            first = false;
            out += json::serialize(json::string_view(member->key()));
            out += ':';
            append_sorted(out, member->value());
        }
        out += '}';
    } else if (value.is_array()) {
        out += '[';
        bool first = true;
        for (const auto &item : value.as_array()) {
            if (!first) out += ',';
            first = false;
            append_sorted(out, item);
        }
        out += ']';
    } else {
        out += json::serialize(value);
    }
}
} // namespace

std::string digest_bytes(std::string_view algorithm, std::string_view data) {
    const EVP_MD *method = algorithm == "SHA-1" ? EVP_sha1() : algorithm == "SHA-384" ? EVP_sha384()
                           : algorithm == "SHA-512" ? EVP_sha512() : EVP_sha256();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned size = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &size, method, nullptr) != 1)
        throw Error(500, "waa_runtime_failed", "Could not compute a digest");
    return {reinterpret_cast<const char *>(digest.data()), size};
}

std::string encoding_name(std::string_view label) {
    constexpr std::string_view whitespace = "\t\n\f\r ";
    const auto begin = label.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) return {};
    std::string trimmed(label.substr(begin, label.find_last_not_of(whitespace) - begin + 1));
    for (auto &character : trimmed)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    for (const auto &entry : labels)
        if (entry.label == trimmed) return std::string(entry.name);
    return {};
}

std::optional<std::string> decode_text(std::string_view name, std::string_view data, bool fatal) {
    bool invalid = false;
    std::string decoded;
    if (name == "utf-8") decoded = decode_utf8(data, invalid);
    else if (name == "utf-16le") decoded = decode_utf16(data, false, invalid);
    else if (name == "utf-16be") decoded = decode_utf16(data, true, invalid);
    else if (name == "windows-1252") decoded = decode_windows_1252(data);
    else return std::nullopt;
    if (fatal && invalid) return std::nullopt;
    return decoded;
}

std::string random_bytes(std::size_t count) {
    std::string bytes(count, '\0');
    if (count && RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), static_cast<int>(count)) != 1)
        throw Error(500, "waa_runtime_failed", "Could not generate random bytes");
    return bytes;
}

std::string random_uuid() {
    auto bytes = random_bytes(16);
    bytes[6] = static_cast<char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<char>((bytes[8] & 0x3F) | 0x80);
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        const auto byte = static_cast<unsigned char>(bytes[i]);
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}

std::string sorted_json(const json::value &value) {
    std::string out;
    append_sorted(out, value);
    return out;
}
} // namespace ais2api::waa
