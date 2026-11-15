// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
namespace ais2api {
namespace browser_detail {
std::string_view embedded_bridge_source();
std::string_view embedded_bridge_sha256();
std::string wire_content_hash(const json::array &wire);
enum class IdentityVerdict { pending, verified, mismatch, ambiguous };
IdentityVerdict identity_verdict(const json::object &identity, std::string_view expected_email);
// What the page identity probe proves at once: a sign-in page (unless the
// caller tolerates one while navigating), a region block, and an AI Studio
// terms dialog that only the account's owner may accept. The bridge continues
// the welcome dialog itself and does not report it.
void check_identity(const json::object &identity, bool allow_login);
// How a gateway reads the account page. `bridge_ready` and `identity` take a
// timeout; `identity` returns the probe already passed through check_identity
// with sign-in allowed. `timed_out` runs before the final error.
struct IdentityPage {
    std::function<bool(std::chrono::seconds)> bridge_ready;
    std::function<json::object(std::chrono::seconds)> identity;
    std::function<void()> pause;
    std::function<void()> timed_out = {};
};
// Waits up to 60 s until the page shows exactly the account's identity and,
// with `require_editor`, the prompt editor.
void await_account_identity(const IdentityPage &page, std::string_view expected_email, bool require_editor,
                            std::stop_token token);
json::array import_cookies(const json::array &cookies, std::size_t *skipped_partition_cookies = nullptr);
// Stored cookies prepared for browser injection: supported fields only, and
// without the volatile cookies that conflict with a new session (unless
// nothing else remains). The stored state itself keeps them.
json::array injected_cookies(const json::array &stored);
// Cookies to persist after a verified session: the browser's current Google
// cookies replace the stored Google ones; stored cookies for other domains and
// partitioned cookies the browser cannot export are kept unchanged.
json::array refreshed_cookies(const json::array &stored, const json::array &browser);
}
} // namespace ais2api
