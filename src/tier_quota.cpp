// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/tier_quota.hpp"
#include "../browser/rate_policy.hpp"
#include <algorithm>
#include <array>
#include <cctype>

namespace ais2api {
namespace {
constexpr std::array<std::string_view, 4> tiers{"free", "plus", "pro", "ultra"};
constexpr std::int64_t request_limit = 1000000, token_limit = 1000000000000LL;

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
bool model_id(std::string_view id) {
    return !id.empty() && id.size() <= 256 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") == std::string_view::npos;
}
// YYYY-MM-DDTHH:MM:SSZ, which orders as text.
bool utc_timestamp(std::string_view text) {
    if (text.size() != 20) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char expected = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' : i == 19 ? 'Z' : 0;
        if (expected ? text[i] != expected : text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}
std::optional<std::int64_t> bounded(const json::object &item, std::string_view key, std::int64_t limit) {
    auto value = item.if_contains(key);
    if (!value) return std::nullopt;
    std::int64_t number = 0;
    if (value->is_int64()) number = value->as_int64();
    else if (value->is_uint64() && value->as_uint64() <= static_cast<std::uint64_t>(limit)) number = static_cast<std::int64_t>(value->as_uint64());
    else return std::nullopt;
    if (number < 1 || number > limit) return std::nullopt;
    return number;
}
// {id: {tier: null | {requests, tokens}}}, or nothing when malformed.
std::optional<json::object> parse_models(const json::value &models) {
    if (!models.is_object()) return std::nullopt;
    json::object out;
    for (auto &entry : models.as_object()) {
        const std::string_view id(entry.key().data(), entry.key().size());
        if (!model_id(id) || !entry.value().is_object()) return std::nullopt;
        json::object row;
        for (auto &tier : entry.value().as_object()) {
            const std::string_view name(tier.key().data(), tier.key().size());
            if (!known_tier(name)) return std::nullopt;
            if (tier.value().is_null()) {
                row[name] = nullptr;
                continue;
            }
            if (!tier.value().is_object() || tier.value().as_object().size() != 2) return std::nullopt;
            auto requests = bounded(tier.value().as_object(), "requests", request_limit);
            auto tokens = bounded(tier.value().as_object(), "tokens", token_limit);
            if (!requests || !tokens) return std::nullopt;
            row[name] = json::object{{"requests", *requests}, {"tokens", *tokens}};
        }
        if (row.empty()) return std::nullopt;
        out[id] = std::move(row);
    }
    return out;
}
[[noreturn]] void invalid_catalog() {
    throw Error(502, "invalid_quota_catalog", "Quota source returned an invalid catalog");
}
} // namespace

bool known_tier(std::string_view tier) {
    return std::find(tiers.begin(), tiers.end(), tier) != tiers.end();
}
int tier_rank(std::string_view tier) {
    const auto found = std::find(tiers.begin(), tiers.end(), tier);
    return found == tiers.end() ? static_cast<int>(tiers.size()) : static_cast<int>(found - tiers.begin());
}

json::object parse_quota_catalog(const json::value &data, std::int64_t fetched_at) {
    if (!data.is_object()) invalid_catalog();
    const auto &root = data.as_object();
    auto version = root.if_contains("schema_version");
    if (!version || !version->is_int64()) invalid_catalog();
    if (version->as_int64() != 1)
        throw Error(502, "quota_schema_unsupported", "Quota catalog uses an unsupported schema version");
    for (auto &entry : root)
        if (entry.key() != "schema_version" && entry.key() != "published_at" && entry.key() != "note" &&
            entry.key() != "tier_fallback" && entry.key() != "models")
            invalid_catalog();
    auto published = root.if_contains("published_at");
    if (!published || !published->is_string() ||
        !utc_timestamp(std::string_view(published->as_string().data(), published->as_string().size())))
        invalid_catalog();
    if (auto note = root.if_contains("note"); note && (!note->is_string() || note->as_string().size() > 1024))
        invalid_catalog();
    json::object fallback;
    if (auto value = root.if_contains("tier_fallback")) {
        if (!value->is_object()) invalid_catalog();
        for (auto &entry : value->as_object()) {
            const std::string_view from(entry.key().data(), entry.key().size());
            if (!known_tier(from) || !entry.value().is_string()) invalid_catalog();
            const std::string_view to(entry.value().as_string().data(), entry.value().as_string().size());
            // A single step between different known tiers; never a chain.
            if (!known_tier(to) || to == from || value->as_object().if_contains(to)) invalid_catalog();
            fallback[from] = to;
        }
    }
    auto models_value = root.if_contains("models");
    if (!models_value) invalid_catalog();
    auto models = parse_models(*models_value);
    if (!models || models->empty()) invalid_catalog();
    return {{"published_at", *published},
            {"fetched_at", fetched_at},
            {"tier_fallback", std::move(fallback)},
            {"models", std::move(*models)}};
}

std::string quota_catalog_published(const json::object &catalog) {
    return string_or(catalog, "published_at");
}

json::object parse_quota_overrides(const json::value &models) {
    auto parsed = parse_models(models);
    if (!parsed)
        throw Error(400, "invalid_config",
                    "tier_quotas maps lowercase model ids to {free|plus|pro|ultra: null | {requests, tokens}}");
    return std::move(*parsed);
}

TierPolicy::TierPolicy(const json::object &catalog, const json::object &overrides) {
    if (auto models = catalog.if_contains("models"); models && models->is_object())
        models_ = models->as_object();
    if (auto fallback = catalog.if_contains("tier_fallback"); fallback && fallback->is_object())
        for (auto &entry : fallback->as_object())
            if (entry.value().is_string())
                fallback_.emplace(std::string(entry.key()), std::string(entry.value().as_string()));
    for (auto &entry : overrides) {
        models_[entry.key()] = entry.value();
        overridden_.emplace(entry.key());
    }
}

TierQuota TierPolicy::lookup(std::string_view model, std::string_view tier) const {
    TierQuota quota;
    const auto entry = models_.if_contains(lowercase(model));
    if (!entry || !entry->is_object()) return quota;
    quota.listed = true;
    if (!known_tier(tier)) return quota;
    const auto &row = entry->as_object();
    auto value = row.if_contains(tier);
    if (!value)
        if (auto fallback = fallback_.find(tier); fallback != fallback_.end())
            value = row.if_contains(fallback->second);
    if (!value) return quota;
    if (value->is_null()) {
        quota.available = false;
        return quota;
    }
    const auto &limits = value->as_object();
    quota.requests = limits.at("requests").to_number<std::int64_t>();
    quota.tokens = limits.at("tokens").to_number<std::int64_t>();
    return quota;
}

bool TierPolicy::excludes(std::string_view model, std::string_view tier) const {
    return !lookup(model, tier).available;
}

bool TierPolicy::exhausted(std::string_view model, std::string_view tier, std::int64_t requests,
                           std::int64_t tokens) const {
    const auto quota = lookup(model, tier);
    return (quota.requests > 0 && requests >= quota.requests) || (quota.tokens > 0 && tokens >= quota.tokens);
}

json::object TierPolicy::effective() const {
    json::object out;
    for (auto &entry : models_) {
        const std::string_view id(entry.key().data(), entry.key().size());
        json::object resolved;
        for (auto tier : tiers) {
            const auto quota = lookup(id, tier);
            if (!quota.available) resolved[tier] = nullptr;
            else if (quota.limited()) resolved[tier] = json::object{{"requests", quota.requests}, {"tokens", quota.tokens}};
        }
        out[id] = json::object{{"origin", overridden_.contains(id) ? "config" : "catalog"},
                               {"tiers", std::move(resolved)}};
    }
    return out;
}

std::int64_t next_quota_reset(std::int64_t now) {
    return browser_detail::next_pacific_reset(now);
}

void annotate_quota(json::array &accounts, const TierPolicy &policy) {
    const auto count = [](const json::object &row, std::string_view key) -> std::int64_t {
        const auto value = row.if_contains(key);
        return value && value->is_int64() ? std::max<std::int64_t>(value->as_int64(), 0) : 0;
    };
    const auto table = policy.effective();
    for (auto &item : accounts) {
        if (!item.is_object()) continue;
        auto &account = item.as_object();
        const auto tier = string_or(account, "benefit_tier");
        std::map<std::string, json::object, std::less<>> used;
        if (auto raw = account.if_contains("daily_usage"); raw && raw->is_array())
            for (auto &row : raw->as_array())
                if (row.is_object()) used.emplace(lowercase(string_or(row.as_object(), "model")), row.as_object());
        json::array view;
        const auto add = [&](std::string_view model, const json::object *row) {
            const auto quota = policy.lookup(model, tier);
            const auto requests = row ? count(*row, "requests") : 0;
            const auto tokens = row ? count(*row, "tokens") : 0;
            const bool flagged = row && bool_or(*row, "exhausted");
            view.push_back(json::object{
                {"model", model},
                {"requests", requests},
                {"tokens", tokens},
                {"request_limit", quota.requests > 0 ? json::value(quota.requests) : json::value(nullptr)},
                {"token_limit", quota.tokens > 0 ? json::value(quota.tokens) : json::value(nullptr)},
                {"exhausted", flagged || policy.exhausted(model, tier, requests, tokens)}});
        };
        // The models the tier has limits for, in table order, then any other
        // model used today.
        for (auto &entry : table) {
            const std::string_view model(entry.key().data(), entry.key().size());
            const auto quota = policy.lookup(model, tier);
            if (!quota.available || !quota.limited()) continue;
            const auto found = used.find(model);
            add(model, found == used.end() ? nullptr : &found->second);
            if (found != used.end()) used.erase(found);
        }
        for (auto &[key, row] : used) add(string_or(row, "model"), &row);
        account["daily_usage"] = std::move(view);
    }
}
} // namespace ais2api
