// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
#include <memory>
#include <optional>
#include <utility>
#include <vector>
namespace ais2api {
namespace browser_detail {
// Stored cookies (the CDP shape used by the account store) as BiDi
// storage.setCookie cookies. Partitioned cookies and entries without a name
// or string value are left out.
json::array bidi_cookies(const json::array &stored);
// Cookies from BiDi storage.getCookies in the stored (CDP) shape, as read by
// import_cookies and refreshed_cookies. Binary values are left out.
json::array stored_cookies(const json::array &bidi);
// The cookies a request to https://<host>/ carries.
json::array cookies_for_host(const json::array &stored, std::string_view host);
// The error for a failed Firefox navigation (the BiDi error message, which
// names an NS_ERROR_* or NSS code): 502 proxy_failed when the proxy is at
// fault, 502 network_failed when Google is unreachable, nothing otherwise.
std::optional<Error> firefox_navigation_failure(std::string_view message);
// The loopback port of a "WebDriver BiDi listening on ws://..." line.
std::optional<unsigned short> bidi_endpoint_port(std::string_view line);
// Camoufox reads its configuration from CAMOU_CONFIG_1..N. Each piece holds
// at most 2047 bytes and ends on a UTF-8 character boundary, since Windows
// converts each variable to UTF-16 on its own.
std::vector<std::pair<std::string, std::string>> camoufox_environment(std::string_view config_json);
// user.js contents setting `prefs` (strings, booleans and 32-bit integers),
// sorted by name. Other values are rejected.
std::string firefox_user_js(const json::object &prefs);
// Pre-generated Camoufox identities (scripts/camoufox-dev.py identities).
struct CamoufoxIdentityPool {
    json::object camoufox;
    std::vector<json::object> identities;  // {config, prefs}
    std::string sha256;
};
CamoufoxIdentityPool load_identity_pool(const fs::path &file);
// The launch configuration for `identity_config`: the identity with the
// account's locale and timezone, and page-world script evaluation allowed.
json::object camoufox_launch_config(const json::object &identity_config, std::string_view locale, std::string_view timezone);
} // namespace browser_detail
} // namespace ais2api
