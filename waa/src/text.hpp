// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What the host's SubtleCrypto.digest, TextDecoder, URL and crypto natives
// compute outside the script. After AIStudio2API's internal/waa/realm.go and
// url.go (MIT).
#pragma once
#include "ais2api/common.hpp"

namespace ais2api::waa {
// The digest SubtleCrypto.digest names "SHA-1", "SHA-384" or "SHA-512";
// anything else is SHA-256. Raw bytes.
std::string digest_bytes(std::string_view algorithm, std::string_view data);
// The WHATWG Encoding name of a label ("latin1" is "windows-1252"); empty
// when the label is unknown here. UTF-8, UTF-16LE/BE and windows-1252 are
// supported; a TextDecoder for any other encoding reports a RangeError as it
// does for an unknown label.
std::string encoding_name(std::string_view label);
// `data` in the named encoding as UTF-8, invalid sequences replaced by
// U+FFFD; nothing when `fatal` and a sequence is invalid, or the name is not
// one encoding_name returns.
std::optional<std::string> decode_text(std::string_view name, std::string_view data, bool fatal);

// The URL interface's attributes after parsing `input` against the optional
// base, as a JSON object; nothing when it does not parse.
std::optional<std::string> parse_url(std::string_view input, const std::string *base);
// The attributes after one of the URL setters ("href", "protocol", ...,
// "hash") ran on `href`; nothing when `href`, or a new href, does not parse.
std::optional<std::string> update_url(std::string_view href, std::string_view field, std::string_view value);
// A version 4 UUID.
std::string random_uuid();
std::string random_bytes(std::size_t count);

// JSON text with every object's keys in byte order, the way the reference
// implementation serialises its maps: the host's scripts see properties in
// the order this text lists them.
std::string sorted_json(const json::value &value);
} // namespace ais2api::waa
