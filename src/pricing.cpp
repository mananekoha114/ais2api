// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/pricing.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace ais2api {
namespace {
constexpr std::int64_t exact_limit = 9007199254740991LL;
[[noreturn]] void invalid_catalog() {
    throw Error(502, "invalid_price_catalog", "Price source returned an invalid catalog");
}
json::value rate(const json::object &item, std::string_view key, bool required) {
    auto value = item.if_contains(key);
    if (!value || value->is_null()) {
        if (required)
            invalid_catalog();
        return nullptr;
    }
    if (!value->is_number())
        invalid_catalog();
    // USD per million tokens equals picodollars per token.
    auto number = value->to_number<double>() * 1e6;
    if (!std::isfinite(number) || number < 0 || number > 1e12)
        invalid_catalog();
    auto rounded = std::round(number);
    if (std::abs(number - rounded) > 0.001)
        invalid_catalog();
    return static_cast<std::int64_t>(rounded);
}
bool iso_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9'))
            return false;
    return true;
}
bool model_id(std::string_view id) {
    return !id.empty() && id.size() <= 256 &&
           id.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
               std::string_view::npos;
}
std::optional<std::int64_t> count(const json::object &usage, std::string_view key,
                                  bool required = false) {
    auto p = usage.if_contains(key);
    if (!p)
        return required ? std::nullopt : std::optional<std::int64_t>(0);
    if (p->is_int64() && p->as_int64() >= 0 && p->as_int64() <= 1000000000000LL)
        return p->as_int64();
    if (p->is_uint64() && p->as_uint64() <= 1000000000000ULL)
        return static_cast<std::int64_t>(p->as_uint64());
    return std::nullopt;
}
bool nontext(const json::object &usage) {
    for (auto key : {"promptTokensDetails", "cacheTokensDetails", "candidatesTokensDetails",
                     "toolUsePromptTokensDetails"}) {
        auto p = usage.if_contains(key);
        if (!p)
            continue;
        if (!p->is_array())
            return true;
        for (auto &v : p->as_array())
            if (!v.is_object() || string_or(v.as_object(), "modality") != "TEXT")
                return true;
    }
    return false;
}
std::int64_t line_amount(std::int64_t tokens, std::int64_t price) {
    // Split before multiplying to avoid overflowing even with large usage.
    auto whole = price / 1000;
    if (whole && tokens > exact_limit / whole)
        throw Error(500, "cost_overflow", "Estimated cost exceeds supported precision");
    auto amount = tokens * whole;
    auto fraction = (tokens * (price % 1000) + 500) / 1000;
    if (fraction > exact_limit - amount)
        throw Error(500, "cost_overflow", "Estimated cost exceeds supported precision");
    return amount + fraction;
}
} // namespace

json::object parse_price_catalog(const json::value &data, std::int64_t fetched_at) {
    if (!data.is_object())
        invalid_catalog();
    auto &root = data.as_object();
    auto version = root.if_contains("schema_version");
    auto published = root.if_contains("published_at");
    auto models = root.if_contains("models");
    if (!version || !version->is_int64() || version->as_int64() != 1 ||
        string_or(root, "currency") != "USD" || !published || !published->is_string() ||
        published->as_string().empty() || published->as_string().size() > 64 || !models ||
        !models->is_object())
        invalid_catalog();
    json::object result;
    for (auto &entry : models->as_object()) {
        std::string id(entry.key());
        if (!model_id(id) || !entry.value().is_object())
            invalid_catalog();
        auto periods = entry.value().as_object().if_contains("periods");
        if (!periods || !periods->is_array() || periods->as_array().empty())
            invalid_catalog();
        json::array values;
        std::string previous;
        for (auto &period : periods->as_array()) {
            if (!period.is_object())
                invalid_catalog();
            auto &item = period.as_object();
            auto from = item.if_contains("from");
            // The first period has no start; later ones start on strictly
            // increasing dates.
            if (!from || (values.empty() ? !from->is_null()
                                         : !from->is_string() || !iso_date(from->as_string()) ||
                                               std::string_view(from->as_string()) <= previous))
                invalid_catalog();
            if (from->is_string())
                previous = from->as_string();
            auto tiers = item.if_contains("tiers");
            if (!tiers || !tiers->is_array() || tiers->as_array().empty())
                invalid_catalog();
            json::array converted;
            std::int64_t last = -1;
            for (auto &tier : tiers->as_array()) {
                if (!tier.is_object())
                    invalid_catalog();
                auto &t = tier.as_object();
                auto above = count(t, "above_tokens", true);
                if (!above || *above <= last || (converted.empty() && *above != 0))
                    invalid_catalog();
                last = *above;
                converted.emplace_back(json::object{{"above_tokens", *above},
                                                    {"input_pico", rate(t, "input", true)},
                                                    {"output_pico", rate(t, "output", true)},
                                                    {"cache_pico", rate(t, "cache_read", false)}});
            }
            values.emplace_back(json::object{{"from", *from}, {"tiers", std::move(converted)}});
        }
        result[id] = json::object{{"model", id},
                                  {"source", price_source},
                                  {"fetched_at", fetched_at},
                                  {"published_at", *published},
                                  {"currency", "USD"},
                                  {"supported", true},
                                  {"periods", std::move(values)}};
    }
    if (result.empty())
        invalid_catalog();
    return result;
}

std::string price_catalog_published(const json::object &catalog) {
    for (auto &entry : catalog)
        if (entry.value().is_object())
            return string_or(entry.value().as_object(), "published_at");
    return {};
}

json::object effective_quote(const json::object &entry, std::string_view date) {
    auto quote = entry;
    auto periods = quote.if_contains("periods");
    if (!periods || !periods->is_array())
        return {};
    const json::object *selected = nullptr;
    for (auto &period : periods->as_array()) {
        auto &item = period.as_object();
        auto &from = item.at("from");
        if (from.is_null() || std::string_view(from.as_string()) <= date)
            selected = &item;
    }
    if (!selected)
        return {};
    auto from = selected->at("from");
    auto tiers = selected->at("tiers");
    quote.erase("periods");
    quote["effective_from"] = std::move(from);
    quote["tiers"] = std::move(tiers);
    return quote;
}

std::string utc_date() {
    const auto day = std::chrono::year_month_day(
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(day.year()),
                  static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
    return text;
}

json::object estimate_tokens(const json::object &usage, const json::object &quote,
                             bool additional_charges) {
    json::object result{{"status", "unpriced"},   {"reason", "missing_usage"},
                        {"amount_nano", nullptr}, {"currency", "USD"},
                        {"lines", json::array{}}, {"quote", quote}};
    auto prompt = count(usage, "promptTokenCount", true),
         output = count(usage, "candidatesTokenCount", true);
    auto cached = count(usage, "cachedContentTokenCount"),
         thoughts = count(usage, "thoughtsTokenCount");
    auto tool = count(usage, "toolUsePromptTokenCount"), total = count(usage, "totalTokenCount");
    if (!prompt || !output || !cached || !thoughts || !tool || !total || *cached > *prompt ||
        *thoughts > *output)
        return result;
    // decode_chunk() already includes thinking tokens in candidatesTokenCount.
    // Split them for display, but charge the combined output exactly once.
    auto visible = *output - *thoughts;
    auto sum = *prompt + *output + *tool;
    result["tokens"] =
        json::object{{"input", *prompt - *cached}, {"cache_read", *cached}, {"output", visible},
                     {"reasoning", *thoughts},     {"tool_input", *tool},   {"total", sum}};
    if (usage.contains("totalTokenCount") && *total != sum) {
        result["reason"] = "inconsistent_usage";
        return result;
    }
    if (additional_charges || *tool > 0 || nontext(usage)) {
        result["reason"] = "unsupported_charge_dimensions";
        return result;
    }
    auto values = quote.if_contains("tiers");
    if (!values || !values->is_array() || values->as_array().empty()) {
        result["reason"] = "missing_price";
        return result;
    }
    if (!bool_or(quote, "supported")) {
        result["reason"] = "unsupported_price_tier";
        return result;
    }
    auto &tiers = values->as_array();
    const json::object *selected = &tiers.front().as_object();
    for (auto &value : tiers) {
        auto &t = value.as_object();
        if (*prompt > t.at("above_tokens").as_int64())
            selected = &t;
    }
    result["above_tokens"] = selected->at("above_tokens");
    std::int64_t amount = 0;
    auto add = [&](std::string_view name, std::int64_t tokens, std::string_view key) {
        auto p = selected->if_contains(key);
        if (tokens > 0 && (!p || !p->is_int64()))
            return false;
        auto price = p && p->is_int64() ? p->as_int64() : 0;
        auto cost = line_amount(tokens, price);
        if (cost > exact_limit - amount)
            throw Error(500, "cost_overflow", "Estimated cost exceeds supported precision");
        amount += cost;
        result.at("lines").as_array().emplace_back(
            json::object{{"kind", name},
                         {"tokens", tokens},
                         {"rate_pico", p ? *p : json::value(nullptr)},
                         {"amount_nano", cost}});
        return true;
    };
    if (!add("input", *prompt - *cached, "input_pico") ||
        !add("cache_read", *cached, "cache_pico") || !add("output", visible, "output_pico") ||
        !add("reasoning", *thoughts, "output_pico")) {
        result["reason"] = "missing_price";
        return result;
    }
    result["amount_nano"] = amount;
    result["status"] = "priced";
    result["reason"] = "";
    return result;
}
} // namespace ais2api
