// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "config.hpp"
#include "logs.hpp"

namespace ais2api {
// What a diagnostics report is built from. Each part may hold anything its
// source returns; the report copies only allowlisted fields.
struct DiagnosticsSources {
    json::object status;   // the service status; empty when it is not running
    json::object browser;  // the gateway's diagnostics; empty when not running
    json::object runtime;  // doctor()
    Config config;
    json::array accounts;  // Store::account_status(), with "backend" and "firefox_identity"
    json::object events;   // Store::backend_events()
    json::array journal;   // journal records, oldest first
    json::object logs;     // log_sources()
    std::string platform;  // runtime_platform()
};
// A report users can attach to a bug report: versions, runtime and browser
// state, counters, account states and recent failure events. It never holds
// cookies, emails, account names or ids, request content, file paths, proxy
// hosts or credentials, keys, fingerprint seeds or hardware strings: fields
// are copied from an allowlist, and accounts become "account-1", ... in
// account order.
json::object safe_report(const DiagnosticsSources &sources);
// The journal's last `limit` failure records (account failures, session
// recoveries, browser and import failures, failed generations), oldest first.
json::array recent_failure_records(const logs::Journal &journal, std::size_t limit = 50);
} // namespace ais2api
