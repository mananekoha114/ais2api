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
// Browser output that repeats on every page without meaning anything: Juggler
// redeclaring SimpleChannel in each content process, and the WebDriver BiDi
// module's WeakMap error when a context closes. Kept out of the browser log.
bool camoufox_log_noise(std::string_view line);
// Camoufox's Linux fonts.conf for a launch: the bundled fonts it names
// relative to the working directory become `font_directories`, and its cache
// moves out of the launch's private XDG_CACHE_HOME to `cache_directory`, so
// the fonts are scanned once rather than at every launch. Both paths are
// absolute. 502 camoufox_layout_changed when the file names its fonts or its
// cache otherwise.
std::string camoufox_fontconfig(std::string_view conf, const std::vector<fs::path> &font_directories,
                                const fs::path &cache_directory);
// Xvfb's arguments, as Camoufox's launcher runs it; it reports the display it
// picked on standard output.
std::vector<std::string> xvfb_arguments();
// The display an Xvfb "-displayfd" line reports: digits only.
std::optional<unsigned> xvfb_display(std::string_view line);
// The proot-distro arguments that run `program` with `args` in the Termux
// container. Each of `binds` is bound at its own path. proot-distro builds the
// program's environment afresh, so `environment` is all it is given, but for
// the HOME and Android variables proot-distro sets itself. `shared_x11` binds
// Termux's X11 sockets, a Termux:X11 display's. 400 invalid_config for a path
// proot-distro cannot bind.
std::vector<std::string> proot_arguments(const fs::path &program, const std::vector<std::string> &args,
                                         const std::vector<fs::path> &binds,
                                         const std::vector<std::pair<std::string, std::string>> &environment,
                                         bool shared_x11 = false);
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
    std::string_view backend_for(const Account &) override { return "camoufox"; }
    void reconfigure(const Config &config) override;
    // Launches the browser without an identity and checks, on a local page,
    // what generation depends on: the bridge preload, navigator.webdriver,
    // BYOB stream readers and trusted key presses. No Google request.
    json::object verify_runtime(std::stop_token cancel = {});
    // Launches the browser with the identity of `account_id` (generated, and
    // saved for a stored account, when it has none) and reads on a local page
    // what the identity sets, beside what it should read. The page also looks
    // for the identity's fonts and for `probe_fonts`, measures text in the
    // generic families, and reads back one canvas and one WebGL drawing twice.
    // No cookies and no Google request.
    json::object verify_identity(std::string_view account_id, std::stop_token cancel = {},
                                 const std::vector<std::string> &probe_fonts = {});
    void close() override;
    void reset_session() override;
    void clear_template() override;
    bool release_idle() override;
    // For tools/waa_experiment only: generate() in its two halves. The page's
    // request ticket for the request's content, and the request sent by the
    // page with a ticket from anywhere.
    std::string experiment_ticket(const Request &request, const Account &account, std::stop_token cancel = {});
    void experiment_send(const Request &request, const Account &account, std::string_view ticket, const EventSink &sink,
                         std::stop_token cancel = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ais2api
