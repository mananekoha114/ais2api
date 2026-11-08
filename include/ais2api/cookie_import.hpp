// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
namespace ais2api {
struct CookieImportResult {
    std::vector<Account> accounts;
    json::object details;
};
// Once at least one account is verified, no probe starts that is unlikely to
// finish before `deadline`; the scan returns what it has verified instead.
// `observe` receives the diagnostic events the first probe proves about the
// cookies themselves ("login_redirect", "terms_required"); later probes end
// the multi-login scan and are not counted.
CookieImportResult probe_cookie_import(const json::object &request, unsigned max_accounts,
                                      GenerationBackend &backend, std::stop_token token = {},
                                      std::optional<Clock::time_point> deadline = std::nullopt,
                                      const std::function<void(std::string_view)> &observe = {});
}
