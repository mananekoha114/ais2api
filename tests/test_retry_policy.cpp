// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/retry_policy.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
namespace {
FailureDecision decide(unsigned status, std::string code, unsigned attempt = 0, bool recovered = false,
                       std::string quota_scope = "unknown") {
    Error error(status, std::move(code), "synthetic");
    error.quota_scope = std::move(quota_scope);
    return classify_failure(error, attempt, recovered);
}
}

TEST_CASE("Caller and environment failures never touch account state", "[upstream-policy][recovery]") {
    for (const auto& [status, code] : std::vector<std::pair<unsigned, std::string>>{
             {400, "invalid_argument"}, {413, "request_limit"}, {404, "upstream_error"},
             {409, "upstream_error"}, {403, "region_blocked"}, {504, "generation_timeout"},
             {504, "stream_idle_timeout"}, {503, "browser_not_configured"}, {400, "invalid_auth_user"},
             {500, "consumer_timeout"}}) {
        INFO(code << " " << status);
        CHECK(decide(status, code).action == FailureAction::fail);
    }
}

TEST_CASE("Route glitches retry the same session and the first empty reply refreshes its template", "[upstream-policy][recovery]") {
    CHECK(decide(404, "transient_rpc_route", 2).action == FailureAction::retry_same);
    CHECK(decide(502, "upstream_no_content", 0).action == FailureAction::refresh_template);
    CHECK(decide(502, "upstream_no_content", 1).action == FailureAction::fail);
    // A template refresh is not the request's session recovery.
    CHECK(decide(502, "upstream_no_content", 0, true).action == FailureAction::refresh_template);
}

TEST_CASE("Quota errors cool the model by scope", "[upstream-policy][recovery]") {
    CHECK(decide(429, "rate_limit", 0, false, "minute").cooldown == "rate_limit_minute");
    CHECK(decide(429, "rate_limit", 0, false, "day").cooldown == "rate_limit_day");
    auto unknown = decide(429, "rate_limit");
    CHECK(unknown.action == FailureAction::cool_and_switch);
    CHECK(unknown.cooldown == "rate_limit_model");
}

TEST_CASE("Permission errors rebuild the BotGuard session before any penalty", "[upstream-policy][recovery]") {
    CHECK(decide(403, "permission_denied").action == FailureAction::rebuild_session);
    auto repeated = decide(403, "permission_denied", 1, true);
    CHECK(repeated.action == FailureAction::cool_and_switch);
    CHECK(repeated.cooldown == "permission_temporary");
    CHECK(decide(403, "benefit_tier_unconfirmed").action == FailureAction::rebuild_session);
    CHECK(decide(403, "benefit_tier_unconfirmed", 1, true).action == FailureAction::fail);
}

TEST_CASE("Only proven sign-in loss expires an account", "[upstream-policy][recovery]") {
    CHECK(decide(401, "session_expired").action == FailureAction::rebuild_session);
    CHECK(decide(401, "session_expired", 1, true).cooldown == "login_expired");
    CHECK(decide(401, "missing_cookies").cooldown == "login_expired");
    for (const auto* code : {"upstream_auth", "identity_unverified", "authorization_hash_failed"}) {
        INFO(code);
        CHECK(decide(401, code).action == FailureAction::rebuild_session);
        auto repeated = decide(401, code, 1, true);
        CHECK(repeated.action == FailureAction::cool_and_switch);
        CHECK(repeated.cooldown == "auth");
    }
    // A different Google account behind the cookies does not change on rebuild.
    auto mismatch = decide(401, "identity_mismatch");
    CHECK(mismatch.action == FailureAction::cool_and_switch);
    CHECK(mismatch.cooldown == "auth");
}

TEST_CASE("An AI Studio terms dialog rebuilds once and then waits for the account's owner", "[upstream-policy][recovery]") {
    CHECK(decide(409, "terms_required").action == FailureAction::rebuild_session);
    auto repeated = decide(409, "terms_required", 1, true);
    CHECK(repeated.action == FailureAction::cool_and_switch);
    CHECK(repeated.cooldown == "terms_required");
}

TEST_CASE("Browser failures restart the browser once without penalty", "[upstream-policy][recovery]") {
    for (const auto& [status, code] : std::vector<std::pair<unsigned, std::string>>{
             {502, "browser_incompatible"}, {502, "browser_disconnected"}, {504, "browser_timeout"},
             {500, "browser_launch"}}) {
        INFO(code);
        CHECK(decide(status, code).action == FailureAction::restart_browser);
        CHECK(decide(status, code, 1, true).action == FailureAction::fail);
    }
}

TEST_CASE("Upstream server errors retry the same account with growing pauses", "[upstream-policy][recovery]") {
    for (const auto& [status, code] : std::vector<std::pair<unsigned, std::string>>{
             {503, "upstream_error"}, {500, "upstream_error"}, {502, "invalid_upstream"},
             {502, "truncated_upstream"}, {502, "incomplete_upstream"}, {502, "empty_upstream"}}) {
        INFO(code);
        auto first = decide(status, code, 0);
        CHECK(first.action == FailureAction::retry_same);
        CHECK(first.cooldown.empty());
        CHECK(decide(status, code, 1).delay > first.delay);
        // Server trouble stays account-neutral even after a recovery.
        CHECK(decide(status, code, 1, true).action == FailureAction::retry_same);
    }
}
