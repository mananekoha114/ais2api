// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/usage.hpp"
#include "ais2api/pricing.hpp"
#include "ais2api/protocol.hpp"
#include <algorithm>

namespace ais2api {
namespace {
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::int64_t elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
std::string identifier(std::string_view value, std::size_t limit = 256) {
    if (value.size() > limit ||
        value.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/:") !=
            std::string_view::npos)
        return {};
    return std::string(value);
}
bool extra_dimensions(const json::object &gemini) {
    if (auto tools = gemini.if_contains("tools"); tools && tools->is_array())
        for (auto &tool : tools->as_array())
            if (tool.is_object())
                for (auto &field : tool.as_object())
                    if (field.key() != "functionDeclarations")
                        return true;
    if (auto contents = gemini.if_contains("contents"); contents && contents->is_array())
        for (auto &content : contents->as_array())
            if (content.is_object())
                if (auto parts = content.as_object().if_contains("parts");
                    parts && parts->is_array())
                    for (auto &part : parts->as_array())
                        if (part.is_object() && (part.as_object().contains("inlineData") ||
                                                 part.as_object().contains("fileData")))
                            return true;
    if (auto config = gemini.if_contains("generationConfig"); config && config->is_object())
        if (auto modalities = config->as_object().if_contains("responseModalities");
            modalities && modalities->is_array())
            for (auto &modality : modalities->as_array())
                if (modality != "TEXT")
                    return true;
    return false;
}
// Extract only numeric/boolean and known enum generation settings. Free text,
// messages, tools, signatures and credentials never enter request history.
json::object recorded_settings(const json::object &gemini) {
    json::object result;
    auto config = gemini.if_contains("generationConfig");
    if (!config || !config->is_object())
        return result;
    auto &o = config->as_object();
    for (auto key : {"temperature", "topP", "topK", "candidateCount", "maxOutputTokens", "seed",
                     "presencePenalty", "frequencyPenalty"})
        if (auto p = o.if_contains(key); p && p->is_number())
            result[key] = *p;
    if (auto p = o.if_contains("responseMimeType");
        p && p->is_string() &&
        (p->as_string() == "text/plain" || p->as_string() == "application/json"))
        result["responseMimeType"] = *p;
    if (auto p = o.if_contains("thinkingConfig"); p && p->is_object()) {
        json::object thinking;
        if (auto v = p->as_object().if_contains("thinkingBudget");
            v && (v->is_int64() || v->is_uint64()))
            thinking["thinkingBudget"] = *v;
        if (auto v = p->as_object().if_contains("includeThoughts"); v && v->is_bool())
            thinking["includeThoughts"] = *v;
        auto level = string_or(p->as_object(), "thinkingLevel");
        if (level == "LOW" || level == "HIGH" || level == "MEDIUM" || level == "MINIMAL")
            thinking["thinkingLevel"] = level;
        if (!thinking.empty())
            result["thinkingConfig"] = std::move(thinking);
    }
    return result;
}
json::object allowed_usage(const json::object &usage, bool &valid) {
    json::object result;
    valid = true;
    for (auto key : {"promptTokenCount", "candidatesTokenCount", "cachedContentTokenCount",
                     "thoughtsTokenCount", "toolUsePromptTokenCount", "totalTokenCount"})
        if (auto p = usage.if_contains(key)) {
            if ((p->is_int64() && p->as_int64() >= 0 && p->as_int64() <= 1000000000000LL) ||
                (p->is_uint64() && p->as_uint64() <= 1000000000000ULL))
                result[key] = *p;
            else
                valid = false;
        }
    for (auto key : {"promptTokensDetails", "cacheTokensDetails", "candidatesTokensDetails",
                     "toolUsePromptTokensDetails"})
        if (auto p = usage.if_contains(key)) {
            json::array items;
            if (!p->is_array() || p->as_array().size() > 16) {
                valid = false;
                continue;
            }
            for (auto &v : p->as_array()) {
                if (!v.is_object()) {
                    valid = false;
                    continue;
                }
                auto modality = string_or(v.as_object(), "modality");
                if (modality != "TEXT" && modality != "IMAGE" && modality != "AUDIO" &&
                    modality != "VIDEO" && modality != "DOCUMENT")
                    modality = "UNSPECIFIED";
                items.push_back(json::object{{"modality", modality}});
            }
            result[key] = std::move(items);
        }
    if (!result.contains("totalTokenCount") && !result.empty()) {
        std::int64_t total = 0;
        for (auto key : {"promptTokenCount", "candidatesTokenCount", "toolUsePromptTokenCount"})
            if (auto p = result.if_contains(key))
                total += p->to_number<std::int64_t>();
        result["totalTokenCount"] = total;
    }
    return result;
}
json::object unknown_cost(std::string_view reason) {
    return {
        {"status", "unpriced"}, {"reason", reason}, {"amount_nano", nullptr}, {"currency", "USD"}};
}
} // namespace
void summarize_request(json::object &record, bool complete) {
    json::object usage;
    std::int64_t amount = 0;
    std::int64_t unpriced = 0, priced_attempts = 0;
    for (auto &a : record.at("attempts").as_array()) {
        auto &attempt = a.as_object();
        for (auto &field : attempt.at("usage").as_object()) {
            if (!field.value().is_number())
                continue;
            auto n = field.value().to_number<std::int64_t>();
            auto old = usage.if_contains(field.key());
            usage[field.key()] = n + (old ? old->to_number<std::int64_t>() : 0);
        }
        auto &cost = attempt.at("cost").as_object();
        if (string_or(cost, "status") != "priced")
            ++unpriced;
        else {
            ++priced_attempts;
            auto n = cost.at("amount_nano").as_int64();
            if (n > 9007199254740991LL - amount)
                throw Error(500, "cost_overflow", "Estimated cost exceeds supported precision");
            amount += n;
        }
    }
    record["usage"] = std::move(usage);
    const bool interrupted = string_or(record, "status") == "interrupted";
    const bool priced = complete && !unpriced && !interrupted;
    auto cost = priced ? json::object{{"status", "priced"},
                                      {"reason", ""},
                                      {"amount_nano", amount},
                                      {"currency", "USD"}}
                       : unknown_cost(!complete     ? "pending"
                                      : interrupted ? "service_interrupted"
                                                    : "incomplete_attempts");
    cost["known_amount_nano"] =
        priced_attempts || (complete && !interrupted && record.at("attempts").as_array().empty())
            ? json::value(amount)
            : json::value(nullptr);
    cost["unpriced_attempts"] = unpriced;
    record["cost"] = std::move(cost);
    auto &attempts = record.at("attempts").as_array();
    const auto finish =
        attempts.empty() ? std::string{} : string_or(attempts.back().as_object(), "finish_reason");
    record["finish_reason"] = finish;
    if (complete && !interrupted) {
        auto error = string_or(record, "error");
        if (content_filtered(finish)) {
            record["generation_completed"] = false;
            if (error.empty())
                record["error"] = error = "content_filter";
        }
        if (error.empty() && bool_or(record, "generation_completed") &&
            bool_or(record, "delivery_completed"))
            record["status"] = "succeeded";
        else if (error == "cancelled" || error == "client_disconnected" ||
                 !bool_or(record, "delivery_completed"))
            record["status"] = "cancelled";
        else
            record["status"] = "failed";
    }
}
RequestUsage::RequestUsage(Store &store, std::string id, std::string endpoint)
    : store_(store), id_(std::move(id)), record_{{"id", id_},
                                                 {"endpoint", std::move(endpoint)},
                                                 {"created_at", now_ms()},
                                                 {"ended_at", nullptr},
                                                 {"model", ""},
                                                 {"requested_model", ""},
                                                 {"account", ""},
                                                 {"stream", false},
                                                 {"status", "queued"},
                                                 {"http_status", nullptr},
                                                 {"generation_completed", false},
                                                 {"delivery_completed", false},
                                                 {"error", ""},
                                                 {"queue_ms", nullptr},
                                                 {"first_token_ms", nullptr},
                                                 {"generation_ms", nullptr},
                                                 {"duration_ms", nullptr},
                                                 {"settings", json::object{}},
                                                 {"attempts", json::array{}},
                                                 {"usage", json::object{}},
                                                 {"cost", unknown_cost("pending")}} {
    summarize_request(record_, false);
    store_.begin_request(record_);
}
void RequestUsage::configure(std::string_view model, bool stream) {
    std::lock_guard lock(mutex_);
    record_["requested_model"] = identifier(model);
    record_["model"] = identifier(model);
    record_["stream"] = stream;
    save();
}
bool RequestUsage::start() {
    std::lock_guard lock(mutex_);
    if (terminal_ || producer_done_)
        return false;
    started_ = true;
    started_at_ = Clock::now();
    record_["queue_ms"] = elapsed(created_);
    record_["status"] = "running";
    save();
    return true;
}
void RequestUsage::normalized(std::string_view model, const json::object &gemini) {
    std::lock_guard lock(mutex_);
    record_["model"] = identifier(model);
    record_["settings"] = recorded_settings(gemini);
    quote_ = store_.price_quote(model);
    additional_ = extra_dimensions(gemini);
    save();
}
void RequestUsage::begin_attempt(std::string_view account) {
    std::lock_guard lock(mutex_);
    record_["account"] = identifier(account);
    usage_valid_ = true;
    attempt_open_ = true;
    record_.at("attempts")
        .as_array()
        .push_back(json::object{{"account", identifier(account)},
                                {"started_at", now_ms()},
                                {"ended_at", nullptr},
                                {"completed", false},
                                {"finish_reason", ""},
                                {"error", ""},
                                {"usage", json::object{}},
                                {"cost", unknown_cost("pending")}});
    save();
}
void RequestUsage::event(const json::object &event, bool content) {
    std::lock_guard lock(mutex_);
    auto &attempt = record_.at("attempts").as_array().back().as_object();
    if (content && record_.at("first_token_ms").is_null())
        record_["first_token_ms"] = elapsed(created_);
    if (auto p = event.if_contains("usageMetadata"); p && p->is_object())
        attempt["usage"] = allowed_usage(p->as_object(), usage_valid_);
    if (auto candidates = event.if_contains("candidates"); candidates && candidates->is_array())
        for (auto &candidate : candidates->as_array())
            if (candidate.is_object()) {
                auto reason = string_or(candidate.as_object(), "finishReason");
                if (!reason.empty() && reason != "FINISH_REASON_UNSPECIFIED") {
                    attempt["completed"] = true;
                    constexpr std::string_view reasons[] = {"STOP",
                                                            "MAX_TOKENS",
                                                            "SAFETY",
                                                            "RECITATION",
                                                            "LANGUAGE",
                                                            "OTHER",
                                                            "BLOCKLIST",
                                                            "PROHIBITED_CONTENT",
                                                            "SPII",
                                                            "MALFORMED_FUNCTION_CALL",
                                                            "IMAGE_SAFETY",
                                                            "IMAGE_PROHIBITED_CONTENT",
                                                            "IMAGE_RECITATION",
                                                            "IMAGE_OTHER",
                                                            "NO_IMAGE",
                                                            "UNEXPECTED_TOOL_CALL",
                                                            "TOO_MANY_TOOL_CALLS",
                                                            "MISSING_THOUGHT_SIGNATURE"};
                    attempt["finish_reason"] = std::find(std::begin(reasons), std::end(reasons),
                                                         reason) != std::end(reasons)
                                                   ? reason
                                                   : "OTHER";
                }
                if (candidate.as_object().contains("groundingMetadata"))
                    additional_ = true;
            }
}
void RequestUsage::end_attempt_locked(std::string_view error) {
    if (!attempt_open_)
        return;
    auto &attempt = record_.at("attempts").as_array().back().as_object();
    attempt["ended_at"] = now_ms();
    attempt["error"] = identifier(error, 128);
    if (!usage_valid_)
        attempt["cost"] = unknown_cost("invalid_usage");
    else if (!bool_or(attempt, "completed"))
        attempt["cost"] = unknown_cost("incomplete_usage");
    else
        attempt["cost"] = estimate_tokens(attempt.at("usage").as_object(), quote_, additional_);
    attempt.at("cost").as_object()["quote"] = quote_;
    attempt_open_ = false;
}
void RequestUsage::end_attempt(std::string_view error) {
    std::lock_guard lock(mutex_);
    end_attempt_locked(error);
    save();
}
void RequestUsage::producer_done(std::string_view error) {
    std::lock_guard lock(mutex_);
    if (producer_done_)
        return;
    end_attempt_locked(error);
    producer_done_ = true;
    record_["generation_ms"] = started_ ? elapsed(started_at_) : 0;
    record_["generation_completed"] = error.empty();
    if (!error.empty())
        record_["error"] = identifier(error, 128);
    record_["status"] = "finishing";
    save();
}
void RequestUsage::http_done(unsigned status, std::string_view error, bool delivered) {
    std::lock_guard lock(mutex_);
    if (http_done_)
        return;
    http_done_ = true;
    record_["http_status"] = status;
    record_["delivery_completed"] = delivered;
    if (!error.empty())
        record_["error"] = identifier(error, 128);
    if (!started_)
        producer_done_ = true;
    save();
}
void RequestUsage::save() {
    if (terminal_)
        return;
    bool complete = producer_done_ && http_done_;
    summarize_request(record_, complete);
    if (complete) {
        record_["ended_at"] = now_ms();
        record_["duration_ms"] = elapsed(created_);
    }
    store_.save_request(record_, complete);
    terminal_ = complete;
}
} // namespace ais2api
