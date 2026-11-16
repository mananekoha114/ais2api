// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstddef>
#include <string_view>

namespace ais2api {
// Counts the tokens of UTF-8 text with the embedded Gemma 4 tokenizer, which
// Gemini shares (SentencePiece BPE: spaces become U+2581, no pre-split, byte
// fallback). Control tokens such as <eos> are not recognized; their text
// counts as ordinary characters. The table loads on first use and stays
// resident (about 10 MB). Thread-safe; CPU-bound for large inputs.
std::size_t count_tokens(std::string_view utf8);
} // namespace ais2api
