// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
#include "store.hpp"
#include <functional>
#include <map>
#include <mutex>

namespace ais2api {
// An account's stored browser backend is "cloakbrowser", "auto", "camoufox"
// or "spidermonkey"; empty means not settled yet. Accounts and new ones
// follow config.browser_backend only until adopt_backends() settles them, so
// changing the configuration never moves an account to another identity.
bool valid_account_backend(std::string_view backend);
// What adopt_backends() stores for config.browser_backend: "cloakbrowser"
// keeps the Chrome identity, "camoufox" becomes "auto".
std::string_view adopted_backend(std::string_view default_backend);
// The gateway that serves a stored backend: "auto" and, until it exists,
// "spidermonkey" run on Camoufox; an empty or unknown value follows
// `default_backend`.
std::string_view effective_backend(std::string_view stored, std::string_view default_backend);
// "chrome" for CloakBrowser, "firefox" for every Firefox backend. Moving an
// account between them gives Google a new device, so it needs confirmation.
std::string_view identity_family(std::string_view backend);
// The gateways new accounts and the stored ones need, the default first.
std::vector<std::string> backends_needed(std::string_view default_backend, const std::vector<std::string> &stored);
// Settles every unsettled account on adopted_backend(config.browser_backend).
void adopt_backends(Store &store, const Config &config);

// Routes each account to the gateway its backend names, creating gateways on
// first use. Only one gateway keeps a browser: switching closes the other, so
// one account stays resident as before.
class BackendRouter final : public GenerationBackend {
  public:
    using Factory = std::function<std::unique_ptr<GenerationBackend>(std::string_view backend, const Config &, Store &)>;
    BackendRouter(const Config &config, Store &store, Factory factory = {});
    ~BackendRouter() override;
    void generate(const Request &, const Account &, const EventSink &, std::stop_token) override;
    json::array models(const Account &, std::stop_token = {}) override;
    json::object probe(const Account &, std::stop_token = {}) override;
    json::object probe_import(Account &account, std::stop_token token = {}) override;
    // The current gateway's, or the default backend's before any ran, with
    // "backend" and "default_backend".
    json::object diagnostics() override;
    // An account the store does not know (one being imported) uses the
    // default backend.
    std::string_view backend_for(const Account &) override;
    void reconfigure(const Config &) override;
    void close() override;
    void reset_session() override;
    void clear_template() override;
    bool release_idle() override;

  private:
    GenerationBackend &gateway(std::string_view backend);
    GenerationBackend &serve(const Account &account);
    GenerationBackend *current();
    Store &store_;
    Factory factory_;
    std::mutex mutex_;
    Config config_;
    std::map<std::string, std::unique_ptr<GenerationBackend>, std::less<>> gateways_;
    std::string current_;
};
} // namespace ais2api
