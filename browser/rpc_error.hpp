// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/common.hpp"
#include <algorithm>
#include <cctype>

namespace ais2api::browser_detail {
inline std::optional<Error> rpc_status_error(std::int64_t code, unsigned retry_after = 0,
                                             std::string_view message = {}) {
    int http = 502;
    switch (code) {
        case 1: http = 499; break; case 2: http = 500; break;
        case 3: http = 400; break; case 4: http = 504; break;
        case 5: http = 404; break; case 6: http = 409; break;
        case 7: http = 403; break; case 8: http = 429; break;
        case 9: http = 400; break; case 10: http = 409; break;
        case 11: http = 400; break; case 12: http = 501; break;
        case 13: http = 500; break; case 14: http = 503; break;
        case 15: http = 500; break; case 16: http = 401; break;
        default: if (code >= 400 && code <= 599) http = static_cast<int>(code); else return {};
    }
    Error error(http, http == 429 ? "rate_limit" : http == 401 ? "upstream_auth" : http == 403 ? "permission_denied" : "upstream_error", "Google returned an upstream RPC error");
    error.retry_after = std::min(retry_after, 86400u);
    std::string lower(message);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (http == 404 && lower.find("ambiguous request for service") != std::string::npos &&
        lower.find("generatecontent") != std::string::npos)
        error.code = "transient_rpc_route";
    // AI Studio's answer to output past its per-request limit (and possibly to
    // other internal faults); the service decides which one it was.
    if (code == 13 && lower.find("internal error encountered") != std::string::npos)
        error.code = "upstream_internal";
    if (http == 429) {
        if (lower.find("per minute") != std::string::npos || lower.find("perminute") != std::string::npos)
            error.quota_scope = "minute";
        else if (lower.find("per day") != std::string::npos || lower.find("perday") != std::string::npos)
            error.quota_scope = "day";
    }
    return error;
}
inline std::optional<Error> rpc_error(const json::value& value, unsigned retry_after = 0, unsigned depth = 0) {
    if (depth > 16) return {};
    if (value.is_object()) {
        auto error = value.as_object().if_contains("error");
        if (!error || !error->is_object()) return {};
        const auto& object = error->as_object();
        const auto message = string_or(object, "message");
        auto classify = [&](std::int64_t code) {
            auto result = rpc_status_error(code, retry_after, message);
            if (result && result->status == 429 && result->quota_scope == "unknown")
                if (auto details = object.if_contains("details"))
                    result->quota_scope = rpc_status_error(429, retry_after, json::serialize(*details))->quota_scope;
            return result;
        };
        if (auto code = object.if_contains("code"); code && code->is_int64()) return classify(code->as_int64());
        const auto status = string_or(object, "status");
        if (status == "RESOURCE_EXHAUSTED") return classify(8);
        if (status == "PERMISSION_DENIED") return classify(7);
        if (status == "UNAUTHENTICATED") return classify(16);
        if (status == "NOT_FOUND") return classify(5);
        return {};
    }
    if (!value.is_array()) return {};
    const auto& array = value.as_array();
    // A candidate container is payload, not a status envelope: never inspect
    // generated tool arguments or grounding data for status-looking values.
    // Candidates begin with null or their [parts, role] content tuple, while
    // a wrapped status begins with its integer code.
    auto content_tuple = [](const json::value& content) {
        if (content.is_null()) return true;
        if (!content.is_array()) return false;
        const auto& tuple = content.as_array();
        return tuple.empty() || tuple[0].is_null() || tuple[0].is_array();
    };
    if (!array.empty() && array[0].is_array() && !array[0].as_array().empty() &&
        std::all_of(array[0].as_array().begin(), array[0].as_array().end(), [&](const json::value& candidate) {
            return candidate.is_array() && !candidate.as_array().empty() && content_tuple(candidate.as_array()[0]);
        }))
        return {};
    // google.rpc.Status in JSON+protobuf: [code, message, details?].
    if (array.size() >= 2 && array.size() <= 3 && array[0].is_int64() && array[1].is_string() && !array[1].as_string().empty()) {
        if (auto error = rpc_status_error(array[0].as_int64(), retry_after, array[1].as_string())) {
            if (error->status == 429 && error->quota_scope == "unknown" && array.size() == 3)
                error->quota_scope = rpc_status_error(429, retry_after, json::serialize(array[2]))->quota_scope;
            return error;
        }
    }
    for (const auto& child : array) if (auto error = rpc_error(child, retry_after, depth + 1)) return error;
    return {};
}
inline std::optional<Error> rpc_text_error(std::string_view text, unsigned retry_after = 0) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    if (text.starts_with(")]}'")) text.remove_prefix(4);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    std::string normalized;
    if (text.starts_with("[,")) { normalized = "[null,"; normalized += text.substr(2); text = normalized; }
    boost::system::error_code error;
    auto value = json::parse(text, error);
    return error ? std::optional<Error>{} : rpc_error(value, retry_after);
}
} // namespace ais2api::browser_detail
