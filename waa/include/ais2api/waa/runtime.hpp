// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Google's WAA (BotGuard) virtual machine in an embedded SpiderMonkey with a
// Firefox-shaped host, after AIStudio2API's internal/waa. The caller fetches
// the challenge and the interpreter and sends every request the program
// makes; this library depends on neither the server, the configuration nor
// the store.
#pragma once
#include "ais2api/common.hpp"
#include <chrono>
#include <map>
#include <memory>

namespace ais2api::waa {
// What Waa/Create returns for the client interpreter.
struct Challenge {
    std::string message_id;
    std::string interpreter_javascript; // inline source, when the response carries it
    std::string interpreter_url;
    std::string interpreter_hash;
    std::string program;
    std::string global_name;
    std::string client_experiments_state_blob;
};
// Parses a Waa/Create response body. 502 waa_challenge_invalid when it holds
// no usable challenge.
Challenge parse_challenge(std::string_view create_response);
// The digest a challenge names its interpreter by: SHA-256 of the source as
// unpadded URL-safe Base64.
std::string interpreter_hash(std::string_view source);

// What the account's identity and the page decide about the host.
struct Profile {
    std::string page_url;
    std::string locale;   // navigator.language, e.g. "en-US"
    std::string timezone; // IANA name; empty is UTC
    // Values by property name: window.*, navigator.*, screen.*, document.*
    // (cookie, lastModified) and page globals.
    json::object window, navigator, screen, document, globals;
    std::map<std::string, std::string> local_storage;
    bool has_focus = true;
    // The page's CSP requires Trusted Types at script sinks.
    bool trusted_types = false;
    // When the page's navigation began; unset, the host's installation.
    std::optional<std::chrono::system_clock::time_point> navigation_start;
};

struct Options {
    Challenge challenge;
    std::string interpreter;
    Profile profile;
    // Requests the program makes, sent by the caller the way Firefox would:
    // an image load, and an iframe navigation. They return whether a response
    // arrived and may run on any thread; the token is requested when the
    // runtime closes.
    std::function<bool(std::string_view url, std::stop_token)> load_image, load_frame;
};

// One BotGuard lifecycle, as the AI Studio page holds one. The constructor
// loads the interpreter and runs the challenge program; it returns once the
// program handed over its snapshot function. Calls are serialised. Errors are
// 502 waa_runtime_failed, or 499 cancelled when the token is requested.
class Runtime {
  public:
    explicit Runtime(Options options, std::stop_token cancel = {});
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    // Waits for the page requests the program started, and for those their
    // completion started.
    void settle(std::stop_token cancel = {});
    // A proof bound to `digest`, the lowercase hexadecimal SHA-256 of the
    // request's binding prompt. `prompt` is first typed into the page's prompt
    // box, as the page's own worker does; with `run` the Run button is
    // clicked too, the order in which the page asks for a GenerateContent
    // proof. The box is cleared afterwards.
    std::string proof(std::string_view digest, std::string_view prompt, bool run, std::stop_token cancel = {});
    // The unbound snapshot the page sends when it refreshes Waa/Create.
    std::string refresh_snapshot(std::stop_token cancel = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// For diagnostics: the SHA-256 of the embedded host files and the Firefox
// shape table, and the engine's version: {"dom","host","shape","spidermonkey"}.
// Only in a build with AIS2API_HAS_SPIDERMONKEY, like Runtime.
json::object host_versions();
} // namespace ais2api::waa
