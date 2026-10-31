// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/pricing.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace ais2api {
namespace {
constexpr std::int64_t exact_limit = 9007199254740991LL;
[[noreturn]] void invalid_catalog() {
    throw Error(502, "invalid_price_catalog", "Price source returned an invalid catalog");
}
json::value rate(const json::object &item, std::string_view key, double scale) {
    auto value = item.if_contains(key);
    if (!value || value->is_null())
        return nullptr;
    if (!value->is_number())
        invalid_catalog();
    auto number = value->to_number<double>() * scale;
    if (!std::isfinite(number) || number < 0 || number > 1e12)
        invalid_catalog();
    auto rounded = std::round(number);
    if (std::abs(number - rounded) > 0.001)
        invalid_catalog();
    return static_cast<std::int64_t>(rounded);
}
json::object tier(const json::object &item, std::int64_t above, bool lite,
                  std::string suffix = {}) {
    return {{"above_tokens", above},
            {"input_pico",
             rate(item, lite ? "input_cost_per_token" + suffix : "input", lite ? 1e12 : 1e6)},
            {"output_pico",
             rate(item, lite ? "output_cost_per_token" + suffix : "output", lite ? 1e12 : 1e6)},
            {"cache_pico", rate(item, lite ? "cache_read_input_token_cost" + suffix : "cache_read",
                                lite ? 1e12 : 1e6)}};
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

json::object parse_price_catalog(std::string_view source, const json::value &data,
                                 std::int64_t fetched_at) {
    if (!data.is_object() || (source != "litellm" && source != "models.dev"))
        invalid_catalog();
    bool lite = source == "litellm";
    const json::object *models = &data.as_object();
    if (!lite) {
        auto google = models->if_contains("google");
        if (!google || !google->is_object())
            invalid_catalog();
        auto p = google->as_object().if_contains("models");
        if (!p || !p->is_object())
            invalid_catalog();
        models = &p->as_object();
    }
    json::object result;
    for (auto &entry : *models) {
        std::string id(entry.key());
        if (lite && !id.starts_with("gemini/"))
            continue;
        if (!entry.value().is_object())
            invalid_catalog();
        auto &item = entry.value().as_object();
        if (lite &&
            (string_or(item, "litellm_provider") != "gemini" || string_or(item, "mode") != "chat"))
            continue;
        if (lite)
            id.erase(0, 7);
        if (!model_id(id))
            continue;
        const json::object *cost = &item;
        if (!lite) {
            auto p = item.if_contains("cost");
            if (!p || !p->is_object())
                continue;
            cost = &p->as_object();
        }
        std::map<std::int64_t, json::object> tiers;
        tiers.emplace(0, tier(*cost, 0, lite));
        if (tiers.at(0).at("input_pico").is_null() || tiers.at(0).at("output_pico").is_null())
            continue;
        bool supported = true;
        if (lite) {
            for (auto &field : *cost) {
                std::string_view key = field.key();
                if (!key.starts_with("input_cost_per_token_above_") &&
                    !key.starts_with("output_cost_per_token_above_") &&
                    !key.starts_with("cache_read_input_token_cost_above_"))
                    continue;
                auto start = key.find("_above_");
                auto number = key.substr(start + 7);
                auto end = number.find("k_tokens");
                if (end == std::string_view::npos) {
                    supported = false;
                    continue;
                }
                // Alternate service tiers are not standard API rates.
                if (end + 8 != number.size())
                    continue;
                std::int64_t threshold = 0;
                auto parsed = std::from_chars(number.data(), number.data() + end, threshold);
                if (parsed.ec != std::errc{} || parsed.ptr != number.data() + end ||
                    threshold <= 0 || threshold > 1000000) {
                    supported = false;
                    continue;
                }
                threshold *= 1000;
                tiers[threshold] = tier(*cost, threshold, true, std::string(key.substr(start)));
            }
        } else if (auto ts = cost->if_contains("tiers")) {
            if (!ts->is_array())
                invalid_catalog();
            for (auto &t : ts->as_array()) {
                if (!t.is_object())
                    invalid_catalog();
                auto condition = t.as_object().if_contains("tier");
                if (!condition || !condition->is_object() ||
                    string_or(condition->as_object(), "type") != "context") {
                    supported = false;
                    continue;
                }
                auto size = count(condition->as_object(), "size", true);
                if (!size || *size <= 0)
                    invalid_catalog();
                if (tiers.contains(*size))
                    invalid_catalog();
                tiers[*size] = tier(t.as_object(), *size, false);
            }
        } else if (auto over = cost->if_contains("context_over_200k")) {
            if (!over->is_object())
                invalid_catalog();
            tiers[200000] = tier(over->as_object(), 200000, false);
        }
        json::array values;
        for (auto &[threshold, value] : tiers)
            values.emplace_back(std::move(value));
        result[id] = json::object{
            {"model", id},       {"source", source},       {"fetched_at", fetched_at},
            {"currency", "USD"}, {"supported", supported}, {"tiers", std::move(values)}};
    }
    if (result.empty())
        invalid_catalog();
    return result;
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
