// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/continuation.hpp"

namespace ais2api {
void NativeContinuation::check() const {
    check_cancel(cancel_);
    if (deadline_ && Clock::now() >= *deadline_)
        throw Error(504, "generation_timeout", "Generation deadline exceeded");
}
void NativeContinuation::push(json::object event, const EventSink &sink) {
    check();
    if (auto candidates = event.if_contains("candidates"); candidates && candidates->is_array()) {
        if (budget_ && candidates->as_array().size() > 1)
            throw Error(502, "invalid_upstream", "Continuation requires a single candidate");
        for (auto &value : candidates->as_array()) {
            auto &candidate = value.as_object();
            if (auto checkpoint = candidate.if_contains(continuation_token_key)) {
                if (!checkpoint->is_string())
                    throw Error(502, "invalid_upstream", "Invalid continuation checkpoint");
                checkpoint_ = std::string(checkpoint->as_string());
                candidate.erase(continuation_token_key);
            }
            if (budget_) {
                auto reason = string_or(candidate, "finishReason");
                if (!reason.empty() && reason != "FINISH_REASON_UNSPECIFIED") {
                    terminal_ = {{"index", 0}, {"finishReason", reason}};
                }
                if (auto message = candidate.if_contains("finishMessage");
                    message && !terminal_.empty())
                    terminal_["finishMessage"] = *message;
                candidate.erase("finishReason");
                candidate.erase("finishMessage");
            }
        }
    }
    if (budget_) {
        if (auto usage = event.if_contains("usageMetadata"); usage && usage->is_object())
            usage_ = usage->as_object();
        if (auto id = event.if_contains("responseId"); id && id->is_string()) {
            if (response_id_.empty())
                response_id_ = std::string(id->as_string());
            *id = response_id_;
        }
    }
    if (!event.empty())
        sink(std::move(event));
    check();
}

bool NativeContinuation::finish(const EventSink &sink) {
    check();
    if (!budget_)
        return false;
    if (terminal_.empty())
        throw Error(502, "incomplete_upstream", "Upstream response ended without a finish reason");
    ++rounds_;
    const auto count = usage_.if_contains("candidatesTokenCount");
    const bool counted = count && ((count->is_int64() && count->as_int64() >= 0) ||
                                   (count->is_uint64() && count->as_uint64() <= INT64_MAX));
    const auto produced = counted ? count->to_number<std::int64_t>() : 0;
    const bool limited = string_or(terminal_, "finishReason") == "MAX_TOKENS";
    const bool resume =
        limited && budget_ > studio_round_tokens && (!counted || produced < budget_ - output_);
    json::object event{{"candidates", json::array{terminal_}}};
    if (!usage_.empty())
        event["usageMetadata"] = usage_;
    if (!response_id_.empty())
        event["responseId"] = response_id_;
    if (!resume) {
        // The answer has already been delivered. A reported overrun cannot be
        // undone, so the service records it instead of failing the answer.
        sink(std::move(event));
        check();
        return false;
    }
    // This completed round can be priced even when its checkpoint is unusable.
    sink(GenerationEvent(GenerationEvent::Kind::round_finished, std::move(event)));
    if (!counted)
        throw Error(502, "continuation_missing_usage", "Cannot continue without output usage");
    if (!produced)
        throw Error(502, "continuation_no_progress", "Continuation produced no output tokens");
    if (checkpoint_.empty())
        throw Error(502, "continuation_missing_token",
                    "Upstream stopped before the total budget without a checkpoint");
    if (!checkpoints_.insert(sha256(checkpoint_)).second)
        throw Error(502, "continuation_repeated_token",
                    "Upstream repeated a continuation checkpoint");
    // The verified context ceiling contains at most 32 full 32K rounds. A
    // malformed upstream returning tiny checkpoints must not loop indefinitely.
    if (rounds_ >= 32)
        throw Error(502, "continuation_limit", "Continuation round limit exceeded");
    output_ += produced;
    token_ = std::move(checkpoint_);
    checkpoint_.clear();
    terminal_.clear();
    usage_.clear();
    return true;
}
} // namespace ais2api
