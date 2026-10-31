// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "common.hpp"

namespace ais2api {
// Catalog rates are integer picodollars per token. Estimates and totals use
// integer nanodollars; each line is rounded once, half up.
json::object parse_price_catalog(std::string_view source, const json::value &data,
                                 std::int64_t fetched_at);
// Accepts ais2api's normalized usage: candidatesTokenCount includes thoughts.
json::object estimate_tokens(const json::object &usage, const json::object &quote,
                             bool additional_charges = false);
} // namespace ais2api
