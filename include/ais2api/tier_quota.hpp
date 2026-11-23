// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "common.hpp"
#include <map>
#include <set>

namespace ais2api {
// AI Studio Playground daily quotas per subscription ("benefit") tier. The
// catalog is quotas/aistudio.json: embedded at build time and downloaded from
// this repository's main branch (see quota_service.hpp). Its values are
// community observations, so they only order accounts; nothing is refused on
// a local estimate alone.
inline constexpr std::string_view quota_source = "aistudio_quotas";

// The tiers GetAiStudioBenefitTier names. Anything else is unknown.
bool known_tier(std::string_view tier);
// Scheduling preference among accounts that can serve a model: lower tiers
// first, so higher tiers keep their allowance for the models only they can
// use. free 0, plus 1, pro 2, ultra 3, unknown 4.
int tier_rank(std::string_view tier);

// Validates a published catalog and returns it normalized:
// {published_at, fetched_at, tier_fallback: {tier: tier}, models: {id: {tier:
// null | {requests, tokens}}}}. Throws 502 invalid_quota_catalog, or
// quota_schema_unsupported for another schema_version.
json::object parse_quota_catalog(const json::value &data, std::int64_t fetched_at);
// The catalog's publication time (ISO 8601 UTC), empty for an empty catalog.
std::string quota_catalog_published(const json::object &catalog);
// Validates the configuration's tier_quotas, which has the form of a catalog's
// "models". Throws 400 invalid_config.
json::object parse_quota_overrides(const json::value &models);

struct TierQuota {
    // The model has an entry; unlisted models are never filtered or limited.
    bool listed = false;
    // False only when the tier's entry is null.
    bool available = true;
    // Daily limits; 0 means no known limit.
    std::int64_t requests = 0, tokens = 0;
    bool limited() const { return requests > 0 || tokens > 0; }
};

// The effective table: the catalog with the configuration's entries replacing
// whole models.
class TierPolicy {
  public:
    TierPolicy() = default;
    TierPolicy(const json::object &catalog, const json::object &overrides);
    // `model` is the normalized request model (alias resolved, suffixes
    // removed); matching ignores case. An unknown tier is never filtered or
    // limited. A tier missing from an entry uses the catalog's tier_fallback
    // (plus -> free), and otherwise has no known limit.
    TierQuota lookup(std::string_view model, std::string_view tier) const;
    // A known tier whose entry for `model` is null.
    bool excludes(std::string_view model, std::string_view tier) const;
    // Whether `requests`/`tokens` used today reach the tier's limit.
    bool exhausted(std::string_view model, std::string_view tier, std::int64_t requests,
                   std::int64_t tokens) const;
    // {model: {origin: "catalog" | "config", tiers: {tier: null | {requests, tokens}}}}
    // with every known tier resolved through the fallback.
    json::object effective() const;
    bool empty() const { return models_.empty(); }

  private:
    json::object models_;
    std::map<std::string, std::string, std::less<>> fallback_;
    std::set<std::string, std::less<>> overridden_;
};

// Unix seconds when the Pacific quota day containing `now` ends and the daily
// allowances reset (midnight America/Los_Angeles).
std::int64_t next_quota_reset(std::int64_t now);
// Turns each account's raw "daily_usage" (Store::account_status()) into its
// quota view: the models its tier has daily limits for, in table order, then
// any other model it used today, each {model, requests, tokens, request_limit,
// token_limit, exhausted}. Limits are null when unknown; exhausted when a
// daily 429 marked the model or the estimate reaches a limit.
void annotate_quota(json::array &accounts, const TierPolicy &policy);
} // namespace ais2api
