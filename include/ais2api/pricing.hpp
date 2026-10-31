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
//
// The only source is the catalog scripts/scrape-gemini-pricing.py builds from
// Google's official pricing page, published on this repository's pricing-data
// branch and embedded from pricing/gemini.json at build time.
inline constexpr std::string_view price_source = "google";
// Validates a catalog and returns its entries by model id, each with the
// announced price periods converted to picodollars per token.
json::object parse_price_catalog(const json::value &data, std::int64_t fetched_at);
// The catalog's publication time (ISO 8601 UTC), or empty for an empty catalog.
std::string price_catalog_published(const json::object &catalog);
// The quote one catalog entry gives on `date` (YYYY-MM-DD): the latest period
// starting on or before it, in the form estimate_tokens expects.
json::object effective_quote(const json::object &entry, std::string_view date);
// Today's UTC date as YYYY-MM-DD.
std::string utc_date();
// Accepts ais2api's normalized usage: candidatesTokenCount includes thoughts.
json::object estimate_tokens(const json::object &usage, const json::object &quote,
                             bool additional_charges = false);
} // namespace ais2api
