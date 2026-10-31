// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "protocol.hpp"

namespace ais2api {
// A generation session boundary. Production uses BrowserGateway; deterministic
// transports can exercise scheduling and HTTP cancellation without Google.
class GenerationBackend {
  public:
    virtual ~GenerationBackend() = default;
    virtual void generate(const Request &, const Account &, const EventSink &, std::stop_token) = 0;
    // A fresh discovery for this exact account. Errors propagate; configured
    // or previously saved lists are not discovery results.
    virtual json::array models(const Account &, std::stop_token = {}) = 0;
    virtual json::object probe(const Account &, std::stop_token = {}) = 0;
    virtual json::object probe_import(Account &account, std::stop_token token = {}) { return probe(account, token); }
    virtual json::object diagnostics() = 0;
    virtual void reconfigure(const Config &) {}
    virtual void close() = 0;
    // Discard the resident account's page and BotGuard session so the next
    // request rebuilds it in place, keeping the browser process when possible.
    virtual void reset_session() { close(); }
    // Discard the captured GenerateContent template so the next request
    // captures a new one on the resident page.
    virtual void clear_template() { reset_session(); }
    // Stops an idle browser to free its memory; the next request starts it
    // again. Returns whether a running browser was stopped.
    virtual bool release_idle() { return false; }
};
} // namespace ais2api
