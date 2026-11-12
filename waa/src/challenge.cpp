// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// After AIStudio2API's internal/waa/challenge.go (MIT).
#include "ais2api/waa/runtime.hpp"
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace ais2api::waa {
namespace {
[[noreturn]] void invalid(std::string_view what) {
    throw Error(502, "waa_challenge_invalid", "Waa/Create response " + std::string(what));
}
// Standard Base64 with padding; nothing when the text is not Base64.
std::optional<std::string> base64_decode(std::string_view text) {
    if (text.size() % 4) return std::nullopt;
    std::string decoded(text.size() / 4 * 3, '\0');
    const int written = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()),
                                        reinterpret_cast<const unsigned char *>(text.data()), static_cast<int>(text.size()));
    if (written < 0) return std::nullopt;
    std::size_t padding = 0;
    while (padding < 2 && padding < text.size() && text[text.size() - 1 - padding] == '=') ++padding;
    decoded.resize(static_cast<std::size_t>(written) - padding);
    return decoded;
}
std::string string_at(const json::array &fields, std::size_t index) {
    return index < fields.size() && fields[index].is_string() ? std::string(fields[index].as_string()) : std::string();
}
// The first non-empty string of the array at `index`.
std::string first_string_at(const json::array &fields, std::size_t index) {
    if (index >= fields.size() || !fields[index].is_array()) return {};
    for (const auto &item : fields[index].as_array())
        if (item.is_string() && !item.as_string().empty()) return std::string(item.as_string());
    return {};
}
} // namespace

Challenge parse_challenge(std::string_view create_response) {
    boost::system::error_code error;
    const auto parsed = json::parse(create_response, error);
    if (error || !parsed.is_array()) invalid("is not a JSON array");
    const auto &outer = parsed.as_array();
    // The second slot holds the challenge obfuscated: Base64, each byte
    // lowered by 97. Without it the first slot holds it in the clear.
    json::value decoded;
    const json::array *fields = nullptr;
    if (outer.size() > 1 && outer[1].is_string() && !outer[1].as_string().empty()) {
        auto bytes = base64_decode(outer[1].as_string());
        if (!bytes) invalid("has a challenge that is not Base64");
        for (auto &byte : *bytes) byte = static_cast<char>(static_cast<unsigned char>(byte) + 97);
        decoded = json::parse(*bytes, error);
        if (error || !decoded.is_array()) invalid("has a challenge that is not a JSON array");
        fields = &decoded.as_array();
    } else if (!outer.empty() && outer[0].is_array() && !outer[0].as_array().empty()) {
        fields = &outer[0].as_array();
    } else {
        invalid("holds no challenge");
    }
    if (fields->size() < 8) invalid("has a challenge of " + std::to_string(fields->size()) + " fields");
    Challenge challenge;
    challenge.message_id = string_at(*fields, 0);
    challenge.interpreter_javascript = first_string_at(*fields, 1);
    challenge.interpreter_hash = string_at(*fields, 3);
    challenge.program = string_at(*fields, 4);
    challenge.global_name = string_at(*fields, 5);
    challenge.client_experiments_state_blob = string_at(*fields, 7);
    // The interpreter's address is protocol-relative.
    if (auto path = first_string_at(*fields, 2); !path.empty()) {
        if (path.starts_with("https:")) path.erase(0, 6);
        challenge.interpreter_url = "https:" + path;
    }
    if (challenge.interpreter_hash.empty() || challenge.program.empty() || challenge.global_name.empty())
        invalid("names no interpreter or program");
    return challenge;
}

std::string interpreter_hash(std::string_view source) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(source.data()), source.size(), digest);
    // Unpadded URL-safe Base64 of 32 bytes is 43 characters.
    unsigned char encoded[48];
    EVP_EncodeBlock(encoded, digest, SHA256_DIGEST_LENGTH);
    std::string result(reinterpret_cast<const char *>(encoded), 43);
    for (auto &character : result) {
        if (character == '+') character = '-';
        else if (character == '/') character = '_';
    }
    return result;
}
} // namespace ais2api::waa
