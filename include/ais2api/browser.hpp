// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "backend.hpp"
#include <memory>
#include <vector>
namespace ais2api {
namespace browser_detail {
std::string_view embedded_bridge_source();
std::string_view embedded_bridge_sha256();
std::string wire_content_hash(const json::array &wire);
enum class IdentityVerdict { pending, verified, mismatch, ambiguous };
IdentityVerdict identity_verdict(const json::object &identity, std::string_view expected_email);
json::array import_cookies(const json::array &cookies, std::size_t *skipped_partition_cookies = nullptr);
// Stored cookies as passed to Storage.setCookies: supported fields only, and
// without the volatile cookies that conflict with a new session (unless
// nothing else remains). The stored state itself keeps them.
json::array injected_cookies(const json::array &stored);
// Cookies to persist after a verified session: the browser's current Google
// cookies replace the stored Google ones; stored cookies for other domains and
// partitioned cookies the browser cannot export are kept unchanged.
json::array refreshed_cookies(const json::array &stored, const json::array &browser);
// The error for a failed page navigation (Page.navigate errorText): 502
// proxy_failed when the proxy is at fault, 502 network_failed when Google is
// unreachable. Nothing for other outcomes, such as a superseded navigation.
std::optional<Error> navigation_failure(std::string_view error_text, bool proxy_configured);
// Chromium switches that depend on the configuration rather than on the
// launch's own profile, proxy and fingerprint. `no_zygote` launches without
// the zygote, and therefore without the sandbox that depends on it.
std::vector<std::string> launch_switches(const Config &config, bool no_zygote);
}
class BrowserGateway final : public GenerationBackend {
  public:
    BrowserGateway(const Config &config, Store &store);
    ~BrowserGateway() override;
    void generate(const Request &request, const Account &account, const EventSink &sink,
                  std::stop_token cancel) override;
    json::array models(const Account &account, std::stop_token cancel = {}) override;
    json::object probe(const Account &account, std::stop_token cancel = {}) override;
    json::object probe_import(Account &account, std::stop_token cancel = {}) override;
    json::object diagnostics() override;
    void reconfigure(const Config &config) override;
    json::object verify_runtime(std::stop_token cancel = {});
    void close() override;
    void reset_session() override;
    void clear_template() override;
    bool release_idle() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ais2api
