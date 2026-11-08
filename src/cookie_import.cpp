// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/cookie_import.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <limits>
namespace ais2api {
namespace {
std::string folded(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
void invalid() { throw Error(400, "invalid_credentials", "Invalid cookie import request"); }
// One unauthenticated auth_user index must not hide later accounts. Stop only
// after repeated failures so sparse account layouts can still be imported.
constexpr unsigned max_consecutive_probe_failures = 3;
std::string text(const json::object &o, std::string_view key, std::size_t limit) {
    auto p = o.if_contains(key);
    if (!p) return {};
    if (!p->is_string() || p->as_string().size() > limit) invalid();
    return std::string(p->as_string());
}
bool google(std::string domain) {
    domain = folded(domain);
    if (domain.starts_with('.')) domain.erase(0, 1);
    return domain == "google.com" || domain.ends_with(".google.com");
}
json::object checked_state(const json::value &v) {
    const json::value *container = &v;
    if (v.is_object()) container = v.as_object().if_contains("cookies");
    if (container && ((container->is_array() && container->as_array().size() > 512) ||
                      (container->is_object() && container->as_object().size() > 512)))
        throw Error(413, "cookie_import_limit", "Cookie import exceeds the credential count limit");
    auto result = Store::normalize_storage_state(v);
    // Verified imports only keep cookies. Local storage can execute application
    // state, and the extra metadata is not needed to restore the session.
    json::object cookies_only{{"cookies", result.at("cookies")}};
    if (cookies_only.at("cookies").as_array().size() > 512 ||
        json::serialize(cookies_only).size() > 1024 * 1024)
        throw Error(413, "cookie_import_limit", "Cookie import exceeds the credential size limit");
    for (const auto &entry : cookies_only.at("cookies").as_array()) {
        const auto &cookie = entry.as_object();
        if (!google(string_or(cookie, "domain")) || cookie.if_contains("partitionKey") ||
            bool_or(cookie, "partitionKeyOpaque") || bool_or(cookie, "partitioned") ||
            !string_or(cookie, "firstPartyDomain").empty()) invalid();
    }
    return cookies_only;
}
json::object checked_state(const json::object &v) {
    // Check refreshed browser state before converting/copying the JSON object.
    auto cookies = v.if_contains("cookies");
    if ((cookies && cookies->is_array() && cookies->as_array().size() > 512) ||
        json::serialize(v).size() > 1024 * 1024)
        throw Error(413, "cookie_import_limit", "Cookie import exceeds the credential size limit");
    return checked_state(json::value(v));
}
}
CookieImportResult probe_cookie_import(const json::object &request, unsigned max_accounts,
                                      GenerationBackend &backend, std::stop_token token,
                                      std::optional<Clock::time_point> deadline,
                                      const std::function<void(std::string_view)> &observe) {
    check_cancel(token);
    if (max_accounts < 1 || max_accounts > 32) invalid();
    if (json::serialize(request).size() > 1024 * 1024)
        throw Error(413, "cookie_import_limit", "Cookie import exceeds the credential size limit");
    for (const auto &field : request)
        if (field.key() != "cookies" && field.key() != "storage_state" && field.key() != "name_prefix" &&
            field.key() != "auth_user" && field.key() != "email") invalid();
    auto prefix = text(request, "name_prefix", 128);
    auto expected = text(request, "email", 320);
    auto auth = text(request, "auth_user", 10);
    bool explicit_auth = request.if_contains("auth_user") != nullptr;
    if (explicit_auth && (auth.empty() || auth.find_first_not_of("0123456789") != std::string::npos)) invalid();
    if (request.if_contains("cookies") && request.if_contains("storage_state")) invalid();
    const auto *input = request.if_contains("storage_state");
    if (!input) input = request.if_contains("cookies");
    if (!input || (!input->is_string() && !input->is_array() && !input->is_object())) invalid();
    unsigned skipped = 0;
    json::object storage;
    bool raw_header = false;
    auto parsed = Store::normalize_storage_state(*input, &raw_header, 512);
    // checked_state() keeps cookies only; report the local storage it drops.
    std::uint64_t discarded_origins = 0;
    if (auto origins = parsed.if_contains("origins"); origins && origins->is_array())
        discarded_origins = origins->as_array().size();
    if (raw_header) {
        // Header cookies have no host provenance. Host-prefixed cookies cannot be reconstructed.
        if (parsed.at("cookies").as_array().size() > 512)
            throw Error(413, "cookie_import_limit", "Cookie import exceeds the credential count limit");
        json::array cookies;
        for (auto value : parsed.at("cookies").as_array()) {
            auto &cookie = value.as_object();
            auto name = string_or(cookie, "name");
            if (name.starts_with("__Host-")) { ++skipped; continue; }
            // The legacy script sends a header, losing HttpOnly metadata.
            // Google's page must still read these cookies for its RPC auth.
            // Exact metadata from structured imports is never overridden.
            if (name == "SID" || name == "APISID" || name == "SAPISID" ||
                name == "__Secure-1PAPISID" || name == "__Secure-3PAPISID")
                cookie["httpOnly"] = false;
            if (name == "OSID" || name == "__Secure-OSID" || name == "__Secure-BUCKET")
                cookie["domain"] = "aistudio.google.com";
            cookies.push_back(std::move(value));
        }
        parsed["cookies"] = std::move(cookies);
        storage = checked_state(parsed);
    } else storage = checked_state(parsed);
    CookieImportResult result;
    unsigned attempted = 0;
    std::uint64_t skipped_partition = 0;
    bool stopped = false, deadline_reached = false;
    unsigned consecutive_failures = 0;
    Clock::duration longest_probe{};
    std::set<std::string> seen;
    unsigned limit = explicit_auth ? 1 : max_accounts;
    for (unsigned i = 0; i < limit; ++i) {
        check_cancel(token);
        // The request timeout discards every verified account, so keep them
        // rather than start a probe that may not finish before the deadline.
        if (deadline && !result.accounts.empty() && *deadline - Clock::now() < longest_probe) {
            deadline_reached = true;
            break;
        }
        Account a;
        a.id = "acc_" + random_hex(16);
        a.auth_user = explicit_auth ? auth : std::to_string(i);
        a.name = prefix.empty() ? "Imported account " + a.auth_user : prefix + " " + a.auth_user;
        a.storage_state = storage;
        json::object identity;
        ++attempted;
        const auto probe_started = Clock::now();
        const auto record_duration = [&] { longest_probe = std::max(longest_probe, Clock::now() - probe_started); };
        const auto report = [&](std::string_view event) { if (observe && attempted == 1) observe(event); };
        try {
            identity = backend.probe_import(a, token);
            record_duration();
            check_cancel(token);
        } catch (const Error &e) {
            record_duration();
            check_cancel(token);
            if (e.status == 499 || e.code == "cancelled") throw Error(499, "cancelled", "Request cancelled");
            if (e.code == "terms_required") {
                report("terms_required");
                throw Error(409, "cookie_terms_required", "AI Studio asks this account to accept its terms; accept them in a normal browser, then import again");
            }
            if (e.code == "session_expired") report("login_redirect");
            if (e.code == "identity_unverified" || e.code == "identity_mismatch")
                throw Error(409, "identity_unverified", "Cookie account identity could not be verified");
            if (e.code == "region_blocked" || e.code == "cookie_region_blocked")
                throw Error(403, "cookie_region_blocked", "AI Studio is unavailable in this region");
            if (e.status == 401) {
                if (++consecutive_failures < max_consecutive_probe_failures) continue;
                stopped = true;
                break;
            }
            if (e.code == "browser_not_configured")
                throw Error(503, "cookie_browser_not_configured", "Cookie import requires a configured browser; run ais2api doctor");
            if (e.code == "browser_launch")
                throw Error(502, "cookie_browser_launch", "Cookie import browser failed to start; run ais2api doctor");
            if (e.code == "browser_timeout")
                throw Error(504, "cookie_browser_timeout", "Cookie import browser communication timed out; check browser and network connectivity");
            if (e.code == "browser_disconnected")
                throw Error(502, "cookie_browser_disconnected", "Cookie import browser disconnected; check the local service log");
            if (e.code == "browser_incompatible")
                throw Error(502, "cookie_browser_incompatible", "Cookie import could not read a supported AI Studio page; check network access and page compatibility");
            throw Error(502, "cookie_probe_failed", "Cookie identity probe failed");
        } catch (...) {
            check_cancel(token);
            throw Error(502, "cookie_probe_failed", "Cookie identity probe failed");
        }
        if (auto count = identity.if_contains("skippedPartitionCookies")) {
            std::uint64_t n = 0;
            if (count->is_uint64()) n = count->as_uint64();
            else if (count->is_int64() && count->as_int64() >= 0) n = static_cast<std::uint64_t>(count->as_int64());
            skipped_partition += std::min(n, std::numeric_limits<std::uint64_t>::max() - skipped_partition);
        }
        if (bool_or(identity, "login")) {
            report("login_redirect");
            if (++consecutive_failures < max_consecutive_probe_failures) continue;
            stopped = true;
            break;
        }
        a.email = string_or(identity, "email");
        if (!identity.if_contains("login") || !identity.at("login").is_bool() ||
            !identity.if_contains("identityAmbiguous") || !identity.at("identityAmbiguous").is_bool() ||
            !bool_or(identity, "ready") || bool_or(identity, "identityAmbiguous") || a.email.empty() ||
            a.email.size() > 320 || a.email.find_first_of(" \t\r\n,;") != std::string::npos)
            throw Error(409, "identity_unverified", "Cookie account identity could not be verified");
        if (!expected.empty() && folded(expected) != folded(a.email))
            throw Error(409, "identity_unverified", "Cookie account identity did not match");
        if (!seen.insert(folded(a.email)).second) { stopped = true; break; }
        consecutive_failures = 0;
        a.storage_state = checked_state(a.storage_state);
        result.accounts.push_back(std::move(a));
    }
    check_cancel(token);
    if (result.accounts.empty()) throw Error(400, "cookie_not_authenticated", "Cookies are not authenticated");
    result.details = {{"attempted", attempted},
                      {"truncated", !explicit_auth && (deadline_reached || (!stopped && attempted == limit))},
                      {"deadline_reached", deadline_reached},
                      {"skipped_host_cookies", skipped}, {"skipped_partition_cookies", skipped_partition},
                      {"discarded_origins", discarded_origins},
                      {"verified", result.accounts.size()}};
    return result;
}
}
