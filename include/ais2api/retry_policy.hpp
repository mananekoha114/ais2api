// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "common.hpp"

namespace ais2api {
// How the scheduler reacts to one failed generation attempt that has not yet
// delivered content. Account penalties are reserved for evidence that the
// account itself is unusable; session, browser and upstream-server failures
// never cool down or disable a healthy account.
enum class FailureAction {
    fail,            // Return the error; account state is unchanged.
    retry_same,      // Retry the same account and browser session.
    refresh_template, // Discard the captured request template, then retry the same session.
    rebuild_session, // Recreate the account's page and BotGuard session, then retry it.
    restart_browser, // Restart the browser process, then retry the same account.
    cool_and_switch, // Record `cooldown` for the account and try another one.
};

struct FailureDecision {
    FailureAction action = FailureAction::fail;
    // Store::failure kind for cool_and_switch.
    std::string_view cooldown;
    // Pause before retry_same, so an overloaded upstream is not hammered.
    std::chrono::milliseconds delay{0};
};

// `attempt` is zero-based. `recovered` is true once this request already used
// its single in-place session rebuild or browser restart; a repeated failure
// after that recovery is treated as evidence about the account.
inline FailureDecision classify_failure(const Error& error, unsigned attempt, bool recovered) {
    using enum FailureAction;
    const auto& code = error.code;
    // Caller, configuration and environment problems are not account faults.
    if (error.status == 400 || error.status == 413 || code == "unsupported_adapter" ||
        code == "consumer_timeout" || code == "invalid_auth_user" || code == "invalid_config" ||
        code == "browser_not_configured" || code == "region_blocked" ||
        code == "proxy_failed" || code == "proxy_auth_unsupported" ||
        code == "generation_timeout" || code == "stream_idle_timeout")
        return {fail};
    // Recognized upstream routing glitches are retried as-is. A first empty
    // reply may come from a stale request template: capture a new one.
    if (code == "transient_rpc_route") return {retry_same};
    if (code == "upstream_no_content") return {attempt == 0 ? refresh_template : fail};
    if (error.status == 429) {
        return {cool_and_switch, error.quota_scope == "minute" ? "rate_limit_minute"
                                 : error.quota_scope == "day"  ? "rate_limit_day"
                                                               : "rate_limit_model"};
    }
    // Expired or mismatched BotGuard state is reported as PERMISSION_DENIED, so
    // a 403 first rebuilds the page session. An unknown benefit tier may also be
    // resolved by the new page's tier query, but never cools the account.
    if (error.status == 403) {
        if (!recovered) return {rebuild_session};
        if (code == "benefit_tier_unconfirmed") return {fail};
        return {cool_and_switch, "permission_temporary"};
    }
    // Credentials that cannot exist or a persistent sign-in redirect (after a
    // fresh session) are expiry; RPC authentication errors and identity
    // confusion are short account-wide cooldowns, never permanent expiry.
    // AI Studio's terms dialog has been seen to stay closed after a reload; a
    // repeated one waits for the account's owner to accept the terms.
    if (code == "terms_required")
        return recovered ? FailureDecision{cool_and_switch, "terms_required"} : FailureDecision{rebuild_session};
    if (code == "missing_cookies") return {cool_and_switch, "login_expired"};
    if (code == "session_expired") return recovered ? FailureDecision{cool_and_switch, "login_expired"} : FailureDecision{rebuild_session};
    if (code == "identity_mismatch") return {cool_and_switch, "auth"};
    if (code == "upstream_auth" || code == "identity_unverified" || code == "authorization_hash_failed" ||
        error.status == 401)
        return recovered ? FailureDecision{cool_and_switch, "auth"} : FailureDecision{rebuild_session};
    // A hung, crashed or incompatible page is a browser problem.
    if (code == "browser_incompatible" || code == "browser_disconnected" || code == "browser_timeout" ||
        code == "browser_launch")
        return {recovered ? fail : restart_browser};
    // Upstream server errors and incomplete transport say nothing about the
    // account: retry the same session after a short pause. Other client-side
    // statuses (404 model missing, 409, ...) are returned unchanged.
    if (error.status >= 500 && error.status <= 599)
        return {retry_same, {}, std::chrono::milliseconds(500 * (attempt + 1))};
    return {fail};
}
} // namespace ais2api
