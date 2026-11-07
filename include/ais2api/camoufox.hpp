// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
#include <cstdint>
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
// The salt an account's Firefox identity is generated from: the first 64 bits
// of SHA-256 over this installation's secret, the account id and its
// fingerprint seed. Cookie import and the account's first launch draw the
// same identity.
std::uint64_t identity_salt(std::string_view secret, std::string_view account_id, unsigned seed);
} // namespace browser_detail
// The experimental Camoufox backend (browser_backend "camoufox"): the same
// account flow as BrowserGateway over WebDriver BiDi. Each account keeps a
// Firefox identity generated on this machine from the fpgen model setup
// installs, stored with its fingerprint seed.
class CamoufoxGateway final : public GenerationBackend {
  public:
    CamoufoxGateway(const Config &config, Store &store);
    ~CamoufoxGateway() override;
    void generate(const Request &request, const Account &account, const EventSink &sink,
                  std::stop_token cancel) override;
    json::array models(const Account &account, std::stop_token cancel = {}) override;
    json::object probe(const Account &account, std::stop_token cancel = {}) override;
    json::object probe_import(Account &account, std::stop_token cancel = {}) override;
    json::object diagnostics() override;
    void reconfigure(const Config &config) override;
    // Launches the browser without an identity and checks, on a local page,
    // what generation depends on: the bridge preload, navigator.webdriver,
    // BYOB stream readers and trusted key presses. No Google request.
    json::object verify_runtime(std::stop_token cancel = {});
    // Launches the browser with the identity of `account_id` (generated, and
    // saved for a stored account, when it has none) and reads on a local page
    // what the identity sets, beside what it should read. No cookies and no
    // Google request.
    json::object verify_identity(std::string_view account_id, std::stop_token cancel = {});
    void close() override;
    void reset_session() override;
    void clear_template() override;
    bool release_idle() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ais2api
