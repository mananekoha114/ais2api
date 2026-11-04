// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "../browser/generation.hpp"
#include "../browser/rpc_error.hpp"
#include "ais2api/continuation.hpp"
#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace ais2api::browser_detail {
namespace {
using namespace std::chrono_literals;
[[noreturn]] void incompatible(std::string message) {
    throw Error(502, "browser_incompatible", std::move(message));
}
std::string decode64(std::string_view encoded) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (encoded.size() > 4 * ((32768 + 2) / 3) || encoded.size() % 4)
        incompatible("Invalid browser relay chunk");
    std::string bytes;
    bytes.reserve(encoded.size() * 3 / 4);
    unsigned bits = 0, count = 0;
    bool padding = false;
    for (char c : encoded) {
        if (c == '=') {
            padding = true;
            continue;
        }
        const auto n = alphabet.find(c);
        if (padding || n == std::string_view::npos)
            incompatible("Invalid base64 browser chunk");
        bits = (bits << 6) | static_cast<unsigned>(n);
        count += 6;
        if (count >= 8) {
            count -= 8;
            bytes.push_back(static_cast<char>((bits >> count) & 255));
        }
    }
    if (bytes.size() > 32768)
        incompatible("Browser exceeded byte credit");
    return bytes;
}
bool known_benefit(const json::object &state) {
    const auto tier = string_or(state, "benefitTier");
    return tier == "free" || tier == "plus" || tier == "pro" || tier == "ultra";
}
} // namespace

void run_generation(const Request &request, const Limits &limits, const GenerationIO &io,
                    const EventSink &sink, std::stop_token cancel) {
    const auto deadline = Clock::now() + std::chrono::seconds(limits.generation_seconds);
    NativeContinuation continuation(request, deadline, cancel);
    bool retried = false;
    for (;;) {
        continuation.check();
        if (!continuation.token().empty())
            sink(GenerationEvent(GenerationEvent::Kind::continuation_started));
        // Encode once per checkpoint: an empty denial reuses these exact bytes.
        auto body = io.encode(continuation.token());
        std::optional<Clock::time_point> activity;
        const auto idle = std::chrono::seconds(limits.stream_idle_seconds);
        const auto check = [&] {
            continuation.check();
            if (activity && Clock::now() - *activity > idle)
                throw Error(504, "stream_idle_timeout",
                            "No upstream stream bytes before idle deadline");
        };
        for (;;) {
            check();
            bool empty_http_denial = false;
            try {
                WireStreamParser parser(limits.object_bytes);
                std::uint64_t sequence = 0, unacknowledged = 0;
                bool received_bytes = false;
                std::string error_prefix;
                constexpr std::size_t error_prefix_limit = 16384;
                std::optional<Error> parse_failure;
                io.start(body, check);
                if (!activity)
                    activity = Clock::now();
                for (;;) {
                    check();
                    auto value = io.next(unacknowledged);
                    unacknowledged = 0;
                    check_cancel(cancel);
                    if (!value.is_object())
                        incompatible("Browser relay state was lost");
                    const auto &state = value.as_object();
                    auto classified = [&](Error error) {
                        if (error.status == 403 && !known_benefit(state))
                            error.code = "benefit_tier_unconfirmed";
                        return error;
                    };
                    const auto retry_value = state.if_contains("retryAfter");
                    const auto retry_after =
                        retry_value && retry_value->is_int64() && retry_value->as_int64() > 0
                            ? static_cast<unsigned>(
                                  std::min<std::int64_t>(86400, retry_value->as_int64()))
                            : 0u;
                    const auto pending = state.if_contains("pending");
                    if (pending && pending->is_object()) {
                        // Any stream bytes forbid replay, so a large input no
                        // longer needs to stay resident for the rest of the round.
                        if (!received_bytes)
                            std::string().swap(body);
                        received_bytes = true;
                        auto number = pending->as_object().if_contains("sequence");
                        if (!number || !number->is_int64() ||
                            number->as_int64() != static_cast<std::int64_t>(sequence + 1))
                            incompatible("Browser relay sequence mismatch");
                        const auto bytes = decode64(string_or(pending->as_object(), "bytes"));
                        if (bytes.empty())
                            incompatible("Empty browser relay credit");
                        if (error_prefix.size() < error_prefix_limit)
                            error_prefix.append(
                                bytes.substr(0, error_prefix_limit - error_prefix.size()));
                        if (auto error = rpc_text_error(error_prefix, retry_after))
                            throw classified(*error);
                        try {
                            if (!parse_failure)
                                parser.feed(bytes, [&](json::value chunk) {
                                    check_cancel(cancel);
                                    if (auto error = rpc_error(chunk, retry_after))
                                        throw classified(*error);
                                    auto event = decode_chunk(chunk);
                                    if (!event.empty())
                                        continuation.push(std::move(event), sink);
                                    check_cancel(cancel);
                                });
                        } catch (const Error &error) {
                            if (error.code != "invalid_upstream" &&
                                error.code != "truncated_upstream")
                                throw;
                            parse_failure = error;
                        }
                        if (parse_failure && error_prefix.size() >= error_prefix_limit)
                            throw *parse_failure;
                        continuation.check();
                        // The sink has consumed the event before the next read
                        // acknowledges this credit and permits another chunk.
                        unacknowledged = ++sequence;
                        activity = Clock::now();
                    } else if (bool_or(state, "done")) {
                        const auto error = string_or(state, "error");
                        const auto status_value = state.if_contains("status");
                        const auto status =
                            status_value && status_value->is_int64() ? status_value->as_int64() : 0;
                        if (status == 204)
                            throw Error(502, "upstream_no_content",
                                        "Google returned an empty response");
                        if (!error.empty()) {
                            empty_http_denial = status == 403 && !received_bytes && pending &&
                                                pending->is_null() && known_benefit(state);
                            const auto error_body = string_or(state, "errorBody");
                            if (auto rpc = rpc_text_error(error_body, retry_after))
                                throw classified(*rpc);
                            if (auto http = rpc_status_error(status, retry_after, error_body))
                                throw classified(*http);
                            throw Error(502,
                                        error.starts_with("browser_incompatible")
                                            ? "browser_incompatible"
                                            : "upstream_error",
                                        "Browser generation transport failed");
                        }
                        if (auto rpc = rpc_text_error(error_prefix, retry_after))
                            throw classified(*rpc);
                        if (parse_failure)
                            throw *parse_failure;
                        parser.finish();
                        if (io.completed)
                            io.completed();
                        check_cancel(cancel);
                        break;
                    }
                }
            } catch (const Error &error) {
                if (!empty_http_denial || retried || continuation.token().empty() ||
                    error.status != 403 || error.code != "permission_denied")
                    throw;
                continuation.check();
                const auto delay = std::max(2s, std::chrono::seconds(error.retry_after));
                const auto ready = Clock::now() + delay;
                if (ready >= *activity + idle || (!request.stream && ready >= deadline))
                    throw;
                retried = true;
                sink(GenerationEvent(GenerationEvent::Kind::continuation_failed,
                                     {{"error", error.code}}));
                std::mutex wait_mutex;
                std::condition_variable_any wake;
                std::unique_lock lock(wait_mutex);
                wake.wait_for(lock, cancel, delay, [] { return false; });
                check();
                sink(GenerationEvent(GenerationEvent::Kind::continuation_retry_started));
                continue;
            }
            break;
        }
        if (!continuation.finish(sink))
            return;
    }
}
} // namespace ais2api::browser_detail
