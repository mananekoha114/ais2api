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
#include <mutex>

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
    // The top page's PerformanceNavigationTiming, in milliseconds from the
    // navigation's start by field name, and the legacy timing's domLoading.
    // Empty, the shape table's entry stands.
    json::object navigation;
    int dom_loading = 0;
};

// One trusted pointer action on the page and the wait before it: move the
// pointer to a spot ("run" the Run button's label, "turn" the last turn's
// text, "rerun" its Rerun icon), press the button under it, release it (which
// clicks), or the page replaced the element under the pointer.
struct PointerStep {
    enum class Action { move, press, release, replace };
    std::chrono::milliseconds delay{0};
    Action action = Action::move;
    std::string to;
};
// The events Firefox dispatches for a step, in order, and the spots they
// involve: the pointer's spot before and after, and where the focus leaves and
// goes ("prompt", "run", "rerun"; empty when not on an element). `rounds`
// places the last turn and its Rerun button.
struct PointerEvents {
    std::string from, to, blurred, focused;
    int rounds = 0;
    std::vector<std::string> types;
};
// Where the trusted pointer and the focus are on one page, shared by the
// virtual machines the page creates in turn. A page just loaded has the
// pointer outside and the focus in the prompt box. Thread-safe. After
// AIStudio2API's internal/waa Pointer (MIT).
class Pointer {
  public:
    // The events of `step` from where the pointer and the focus are now, which
    // move to where they are after it: entering another element dispatches
    // leave and enter events first; pressing moves the focus to the button
    // under the pointer, and only focusin follows when it was on no element;
    // after a replace the pointer is over the new Run (or Stop) button, or over
    // the conversation once Rerun removed the last turn, which takes the focus
    // from the removed button without focusout.
    PointerEvents step(const PointerStep &step);
    // The number of turns the next Rerun click finds.
    void set_rounds(int rounds);
    // Whether a trusted click on Run or Rerun was dispatched on this page.
    bool clicked() const;

  private:
    mutable std::mutex mutex_;
    std::string at_, focus_ = "prompt";
    int rounds_ = 0;
    bool clicked_ = false;
};
// A trusted click on Run, and on the last turn's Rerun after the pointer
// rested on the turn, with the intervals at which Firefox dispatched each
// group of events when the Camoufox backend clicked them.
const std::vector<PointerStep> &run_click();
const std::vector<PointerStep> &rerun_click();

// A client streamz metric the page's BotGuard wrapper records: for "ec" and
// "cec" the value is a count to add, otherwise a sample.
struct Metric {
    std::string metric, label;
    double value = 0;
};

struct Options {
    Challenge challenge;
    std::string interpreter;
    Profile profile;
    // The page's pointer, shared by the machines the same page creates; null
    // stands for a page just loaded.
    std::shared_ptr<Pointer> pointer;
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
    // request's binding prompt, once `prompt` is typed into the page's prompt
    // box as the page's own worker types it. The box is cleared afterwards.
    std::string proof(std::string_view digest, std::string_view prompt, std::stop_token cancel = {});
    // The same with a trusted mouse click on Run between the typing and the
    // proof, timed as the page asks for a GenerateContent proof after the
    // click.
    std::string run_proof(std::string_view digest, std::string_view prompt, std::stop_token cancel = {});
    // A proof after a trusted click on the Rerun button of the last of
    // `rounds` turns; like the page's Rerun, nothing is typed.
    std::string rerun_proof(std::string_view digest, int rounds, std::stop_token cancel = {});
    // The page replaced the element under the pointer: the Run button turned
    // into Stop or back, or Rerun removed the last turn. Queued; returns at
    // once, and does nothing when the pointer is elsewhere.
    void replaced();
    // Records a user click or key press: navigator.userActivation becomes
    // active for 5 seconds and has been active since.
    void activate(std::stop_token cancel = {});
    // The unbound snapshot the page sends when it refreshes Waa/Create.
    std::string refresh_snapshot(std::stop_token cancel = {});
    // The metrics the page's BotGuard wrapper recorded since the last call,
    // taken after the work already queued, as the wrapper reports them from a
    // zero-delay timer.
    std::vector<Metric> take_metrics(std::stop_token cancel = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// For diagnostics: the SHA-256 of the embedded host files and the Firefox
// shape table, and the engine's version: {"dom","host","shape","spidermonkey"}.
// Only in a build with AIS2API_HAS_SPIDERMONKEY, like Runtime.
json::object host_versions();
} // namespace ais2api::waa
