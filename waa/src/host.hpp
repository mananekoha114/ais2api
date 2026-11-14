// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/waa/runtime.hpp"
#include "js.hpp"
#include "loop.hpp"
#include <initializer_list>
#include <list>
#include <thread>

namespace ais2api::waa {
// The embedded host files: dom.js, smhost.js and the gzip-compressed Firefox
// shape table.
std::string_view dom_script();
std::string_view host_interface_script();
std::string_view shape_table_gzip();

// The handle of a value a host script must return as an object.
inline js::Handle handle_of(const js::Value &value) {
    if (value.kind != js::Kind::handle) throw Error(502, "waa_runtime_failed", "A WAA host script returned no object");
    return value.handle;
}

// One realm's global with the host functions that run its tasks, report its
// exceptions and order its global's keys.
struct RealmHost {
    std::string name;
    js::Handle global = 0, dispatch = 0, report = 0, order = 0;
};

// What the realms of one context share: the top page and the iframe the
// program creates. Everything here runs on the event loop's thread. After
// AIStudio2API's internal/waa/host.go (MIT).
class Agent {
  public:
    using Loader = std::function<bool(std::string_view url, std::stop_token)>;
    Agent(EventLoop &loop, Profile profile, Loader load_image, Loader load_frame, std::stop_token closing);
    ~Agent();

    js::Context &context() { return *context_; }
    // Calls a function, or evaluates a script in a realm. When the outermost
    // call returns, a microtask checkpoint follows, also after an exception.
    js::Value invoke(js::Handle function, js::Handle receiver, std::initializer_list<js::Value> arguments = {});
    js::Value evaluate(js::Handle global, std::string_view source, std::string_view name);
    // A new array of the top realm.
    js::Value new_array(std::initializer_list<js::Value> items);
    // A new realm with the Firefox host installed; with `same`, in that
    // global's compartment.
    RealmHost &install_realm(std::string name, js::Handle same);
    // Releases the value an exception carries.
    void release(const js::Exception &exception);
    // After each task: unhandledrejection and rejectionhandled for the
    // promises rejected in it.
    void after_task();
    // Runs `done` once no page request the program made is in flight.
    void when_settled(std::function<void()> done);
    // Stops the running script from another thread; nothing once the context
    // is gone.
    void interrupt();
    // Destroys the context. On the loop's thread, as its cleanup.
    void close();

    RealmHost *top = nullptr;
    js::Handle array = 0;

  private:
    struct Native {
        unsigned arity;
        js::Native call;
    };
    std::vector<std::pair<std::string, Native>> natives(RealmHost &realm);
    void install_agent(js::Handle result);
    void dispatch(RealmHost &realm, double id, const js::Value &argument);
    void report(RealmHost &realm, const js::Exception &exception);
    void load(RealmHost &realm, const Loader &loader, std::string address, double id);
    void loaded();
    json::object realm_profile(std::string_view realm) const;

    EventLoop &loop_;
    Profile profile_;
    Loader load_image_, load_frame_;
    std::stop_token closing_;
    std::string timezone_;
    std::mutex context_mutex_;
    std::unique_ptr<js::Context> context_;
    int depth_ = 0, pending_ = 0;
    std::vector<std::function<void()>> settled_;
    js::Handle shared_ = 0, track_ = 0, notify_ = 0;
    std::list<RealmHost> realms_;
    // The threads of requests in flight, joined before the agent goes.
    std::mutex loaders_mutex_;
    std::vector<std::jthread> loaders_;
};
} // namespace ais2api::waa
