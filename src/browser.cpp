// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/browser.hpp"
#include <algorithm>
#include <array>
#include <cctype>

namespace ais2api {
browser_detail::IdentityVerdict browser_detail::identity_verdict(const json::object& identity, std::string_view expected_email) {
    if (bool_or(identity, "identityAmbiguous")) return IdentityVerdict::ambiguous;
    auto normalized = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const auto email = normalized(string_or(identity, "email"));
    if (!bool_or(identity, "identityReady") || email.empty()) return IdentityVerdict::pending;
    const auto expected = normalized(std::string(expected_email));
    return expected.empty() || expected == email ? IdentityVerdict::verified : IdentityVerdict::mismatch;
}
void browser_detail::check_identity(const json::object& identity, bool allow_login) {
    if (bool_or(identity, "login") && !allow_login) throw Error(401, "session_expired", "Google redirected the account to sign-in");
    if (bool_or(identity, "regionBlocked")) throw Error(403, "region_blocked", "AI Studio is unavailable in the browser region");
    if (bool_or(identity, "termsRequired"))
        throw Error(409, "terms_required", "AI Studio asks this account to accept its terms; sign in to AI Studio with this account in a normal browser and accept them");
}
void browser_detail::await_account_identity(const IdentityPage& page, std::string_view expected_email, bool require_editor,
                                            std::stop_token token) {
    using namespace std::chrono_literals;
    const auto deadline = Clock::now() + 60s;
    std::optional<Clock::time_point> login_since, mismatch_since;
    auto last = IdentityVerdict::pending;
    while (Clock::now() < deadline) {
        check_cancel(token);
        try {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
            if (remaining < 1s) break;
            if (page.bridge_ready(std::min<std::chrono::seconds>(5s, remaining))) {
                remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                if (remaining < 1s) break;
                const auto info = page.identity(std::min<std::chrono::seconds>(5s, remaining));
                if (bool_or(info, "login")) {
                    mismatch_since.reset();
                    if (!login_since) login_since = Clock::now();
                    if (Clock::now() - *login_since >= 10s)
                        throw Error(401, "session_expired", "Google remained on sign-in after the navigation grace period");
                } else {
                    login_since.reset();
                    last = identity_verdict(info, expected_email);
                    if (last == IdentityVerdict::verified && (!require_editor || bool_or(info, "ready"))) return;
                    if (last == IdentityVerdict::mismatch) {
                        if (!mismatch_since) mismatch_since = Clock::now();
                        if (Clock::now() - *mismatch_since >= 2s)
                            throw Error(401, "identity_mismatch", "The verified Google account differs from the selected account");
                    } else mismatch_since.reset();
                }
            } else { login_since.reset(); mismatch_since.reset(); last = IdentityVerdict::pending; }
        } catch (const Error& error) {
            // Redirects can replace the page realm while the account badge is
            // loading. Missing identity is never a proven mismatch.
            if (error.code != "browser_incompatible") throw;
            login_since.reset(); mismatch_since.reset(); last = IdentityVerdict::pending;
        }
        page.pause();
    }
    if (page.timed_out) page.timed_out();
    if (last == IdentityVerdict::verified)
        throw Error(502, "browser_incompatible", "AI Studio page did not expose a supported ready prompt editor");
    throw Error(409, "identity_unverified", last == IdentityVerdict::ambiguous
        ? "AI Studio exposed multiple account identities" : "AI Studio account identity did not finish loading");
}
json::array browser_detail::import_cookies(const json::array& cookies, std::size_t* skipped_partition_cookies) {
    json::array result;
    if (skipped_partition_cookies) *skipped_partition_cookies = 0;
    for (const auto& value : cookies) {
        if (!value.is_object()) continue;
        const auto& source = value.as_object();
        if (source.contains("partitionKey") || bool_or(source, "partitionKeyOpaque")) {
            if (skipped_partition_cookies) ++*skipped_partition_cookies;
            continue;
        }
        auto domain = string_or(source, "domain");
        std::transform(domain.begin(), domain.end(), domain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        if (domain != "google.com" && !domain.ends_with(".google.com")) continue;
        const auto name = source.if_contains("name"), content = source.if_contains("value");
        if (!name || !name->is_string() || name->as_string().empty() || !content || !content->is_string()) continue;
        json::object cookie{{"name", *name}, {"value", *content}, {"domain", source.at("domain")}};
        if (auto p = source.if_contains("path"); p && p->is_string()) cookie["path"] = *p;
        for (auto key : {"secure", "httpOnly"}) if (auto p = source.if_contains(key); p && p->is_bool()) cookie[key] = *p;
        if (auto p = source.if_contains("sameSite"); p && p->is_string() && (p->as_string() == "Strict" || p->as_string() == "Lax" || p->as_string() == "None")) cookie["sameSite"] = *p;
        if (auto p = source.if_contains("expires"); p && p->is_number() && json::value_to<double>(*p) > 0) cookie["expires"] = *p;
        result.push_back(std::move(cookie));
    }
    return result;
}
json::array browser_detail::injected_cookies(const json::array& stored) {
    // Short-lived security and origin-bound cookies from another browser
    // conflict with the fresh session; the page negotiates new ones.
    static constexpr std::array<std::string_view, 5> conflicting{
        "OSID", "__Secure-OSID", "SIDCC", "__Secure-1PSIDCC", "__Secure-3PSIDCC"};
    json::array all, kept;
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& original = value.as_object();
        json::object cookie;
        for (auto key : {"name", "value", "domain", "path", "secure", "httpOnly", "sameSite", "partitionKey"}) if (auto p = original.if_contains(key)) cookie[key] = *p;
        if (auto expires = original.if_contains("expires"); expires && expires->is_number() && json::value_to<double>(*expires) > 0) cookie["expires"] = *expires;
        const auto name = string_or(cookie, "name");
        const bool conflict = std::find(conflicting.begin(), conflicting.end(), name) != conflicting.end();
        if (!conflict) kept.push_back(cookie);
        all.push_back(std::move(cookie));
    }
    return kept.empty() ? all : kept;
}
json::array browser_detail::refreshed_cookies(const json::array& stored, const json::array& browser) {
    auto result = import_cookies(browser);
    // Never replace credentials with a signed-out or partial export.
    auto has = [&](std::initializer_list<std::string_view> names) {
        return std::any_of(result.begin(), result.end(), [&](const json::value& value) {
            const auto name = string_or(value.as_object(), "name");
            return std::find(names.begin(), names.end(), name) != names.end();
        });
    };
    if (!has({"SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID"}) || !has({"SID", "__Secure-1PSID", "__Secure-3PSID"}))
        return {};
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& cookie = value.as_object();
        auto domain = string_or(cookie, "domain");
        std::transform(domain.begin(), domain.end(), domain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        const bool google = domain == "google.com" || domain.ends_with(".google.com");
        // import_cookies exports neither other domains nor partitioned cookies.
        if (!google || cookie.contains("partitionKey") || bool_or(cookie, "partitionKeyOpaque"))
            result.push_back(cookie);
    }
    return result;
}
namespace {
#include "browser_bridge.inc"

std::string content_hash(const json::array& wire) {
    // Hashed in place: the parts can hold megabytes of inline data.
    Sha256 joined;
    bool first = true;
    if (wire.size() > 1 && wire[1].is_array()) for (const auto& content : wire[1].as_array()) {
        if (!content.is_array() || content.as_array().empty() || !content.as_array()[0].is_array()) continue;
        for (const auto& part : content.as_array()[0].as_array()) {
            if (!first) joined.update(" ");
            first = false;
            if (!part.is_array()) continue;
            const auto& p = part.as_array();
            if (p.size() > 1 && p[1].is_string()) joined.update(p[1].as_string());
            else if (p.size() > 2 && p[2].is_array()) {
                const auto& data = p[2].as_array();
                if (data.size() > 1 && data[1].is_string()) joined.update(data[1].as_string());
            }
            // Function calls/results and executable code/results contribute an
            // empty field, preserving the page's single-space join semantics.
        }
    }
    return joined.finish();
}
} // namespace

std::string browser_detail::wire_content_hash(const json::array& wire) { return content_hash(wire); }
std::string_view browser_detail::embedded_bridge_source() {
    return {reinterpret_cast<const char*>(embedded_bridge_bytes), sizeof(embedded_bridge_bytes)};
}
std::string_view browser_detail::embedded_bridge_sha256() { return embedded_bridge_digest; }
} // namespace ais2api
