// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend_router.hpp"
#include "ais2api/camoufox.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/spidermonkey.hpp"
#include <algorithm>
#include <memory>
#include <unordered_map>

namespace ais2api {
namespace {
struct TransitionRegistry {
    std::mutex guard;
    std::unordered_map<const BackendRouter *, std::shared_ptr<std::mutex>> by_router;
    bool alive = true;
    ~TransitionRegistry() { alive = false; }
};
TransitionRegistry &transition_registry() {
    static TransitionRegistry registry;
    return registry;
}
std::shared_ptr<std::mutex> transition_mutex_for(const BackendRouter *router) {
    auto &registry = transition_registry();
    std::lock_guard lock(registry.guard);
    auto &slot = registry.by_router[router];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
}
void erase_transition_mutex(const BackendRouter *router) noexcept {
    auto &registry = transition_registry();
    if (!registry.alive) return;
    std::lock_guard lock(registry.guard);
    registry.by_router.erase(router);
}
// "auto" runs on Camoufox on desktops and servers, as the Stage 4 experiment
// settled (docs/reports/spidermonkey-experiment-2026-11-15.md).
constexpr std::string_view default_desktop_firefox_backend = "camoufox";
bool default_readiness(std::string_view backend, const Config &config) {
    return firefox_backend_ready(backend, config);
}
} // namespace
bool valid_account_backend(std::string_view backend) {
    return backend == "auto" || backend == "camoufox" || backend == "spidermonkey";
}
std::string_view adopted_backend(std::string_view) {
    return "auto";
}
bool spidermonkey_available() {
#ifdef AIS2API_HAS_SPIDERMONKEY
    return true;
#else
    return false;
#endif
}
std::string_view default_firefox_backend(std::string_view platform) {
    if (platform == "termux-arm64" && spidermonkey_available()) return "spidermonkey";
    return default_desktop_firefox_backend;
}
std::string_view default_firefox_backend() {
    return default_firefox_backend(runtime_platform());
}
// Always one of the literals, never a view of the arguments.
std::string_view effective_backend(std::string_view stored, std::string_view,
                                   std::string_view platform) {
    if (stored == "spidermonkey") return spidermonkey_available() ? "spidermonkey" : "camoufox";
    if (stored == "camoufox") return "camoufox";
    return default_firefox_backend(platform);
}
std::string_view effective_backend(std::string_view stored, std::string_view default_backend) {
    return effective_backend(stored, default_backend, runtime_platform());
}
std::string_view identity_family(std::string_view) {
    return "firefox";
}
std::vector<std::string> backends_needed(std::string_view default_backend, const std::vector<std::string> &stored,
                                         std::string_view platform) {
    std::vector<std::string> result{std::string(effective_backend({}, default_backend, platform))};
    for (const auto &backend : stored) {
        std::string next(effective_backend(backend, default_backend, platform));
        if (std::find(result.begin(), result.end(), next) == result.end()) result.push_back(std::move(next));
    }
    return result;
}
std::vector<std::string> backends_needed(std::string_view default_backend, const std::vector<std::string> &stored) {
    return backends_needed(default_backend, stored, runtime_platform());
}
json::array backend_deprecations(std::string_view, const std::vector<std::string> &) {
    return {};
}
void adopt_backends(Store &store, const Config &config) {
    store.adopt_backends(adopted_backend(config.browser_backend));
}

namespace {
std::unique_ptr<GenerationBackend> browser_gateway(std::string_view backend, const Config &config, Store &store) {
#ifdef AIS2API_HAS_SPIDERMONKEY
    if (backend == "spidermonkey") return std::make_unique<SpiderMonkeyGateway>(config, store);
#else
    (void)backend;
#endif
    return std::make_unique<CamoufoxGateway>(config, store);
}
} // namespace

BackendRouter::BackendRouter(const Config &config, Store &store, Factory factory, Readiness readiness,
                             std::string platform)
    : store_(store),
      factory_(factory ? std::move(factory) : Factory(browser_gateway)),
      readiness_(readiness ? std::move(readiness) : Readiness(default_readiness)),
      platform_(platform.empty() ? runtime_platform() : std::move(platform)),
      config_(config) {
    (void)transition_mutex_for(this);
}
BackendRouter::~BackendRouter() { erase_transition_mutex(this); }

GenerationBackend &BackendRouter::gateway(std::string_view backend) {
    auto it = gateways_.find(backend);
    if (it == gateways_.end()) it = gateways_.emplace(std::string(backend), factory_(backend, config_, store_)).first;
    return *it->second;
}
GenerationBackend *BackendRouter::current() {
    std::lock_guard lock(mutex_);
    const auto it = gateways_.find(current_);
    return it == gateways_.end() ? nullptr : it->second.get();
}
GenerationBackend &BackendRouter::serve(const Account &account) {
    const auto transition_lock = transition_mutex_for(this);
    std::lock_guard transition(*transition_lock);
    const std::string backend(backend_for(account));
    GenerationBackend *next = nullptr, *previous = nullptr;
    {
        std::lock_guard lock(mutex_);
        next = &gateway(backend);
        if (const auto it = gateways_.find(current_); it != gateways_.end() && it->first != backend) {
            previous = it->second.get();
            current_.clear();
        }
    }
    // The other gateway's browser stops before this one starts its own.
    if (previous) previous->close();
    std::lock_guard lock(mutex_);
    current_ = backend;
    return *next;
}

void BackendRouter::generate(const Request &request, const Account &account, const EventSink &sink, std::stop_token token) {
    serve(account).generate(request, account, sink, std::move(token));
}
json::array BackendRouter::models(const Account &account, std::stop_token token) {
    return serve(account).models(account, std::move(token));
}
json::object BackendRouter::probe(const Account &account, std::stop_token token) {
    return serve(account).probe(account, std::move(token));
}
json::object BackendRouter::probe_import(Account &account, std::stop_token token) {
    return serve(account).probe_import(account, std::move(token));
}
std::string BackendRouter::benefit_tier(const Account &account) {
    // Only the running gateway can hold the account's page; asking never
    // switches backends.
    auto *target = current();
    return target ? target->benefit_tier(account) : std::string();
}
json::object BackendRouter::diagnostics() {
    GenerationBackend *target = nullptr;
    std::string backend, fallback;
    {
        std::lock_guard lock(mutex_);
        fallback = effective_backend({}, config_.browser_backend, platform_);
        backend = current_.empty() ? fallback : current_;
        target = &gateway(backend);
    }
    auto result = target->diagnostics();
    result["backend"] = backend;
    result["default_backend"] = fallback;
    return result;
}
std::string_view BackendRouter::backend_for(const Account &account) {
    const auto stored = account.id.empty() ? std::nullopt : store_.account_backend(account.id);
    if (stored && *stored == "auto") {
        if (const auto switched = store_.backend_switch(account.id)) {
            Config cfg;
            {
                std::lock_guard lock(mutex_);
                cfg = config_;
            }
            if (switched->backend == "spidermonkey" && spidermonkey_available() && readiness_("spidermonkey", cfg))
                return "spidermonkey";
            if (switched->backend == "camoufox" && readiness_("camoufox", cfg))
                return "camoufox";
        }
        return default_firefox_backend(platform_);
    }
    std::lock_guard lock(mutex_);
    return effective_backend(stored.value_or(std::string{}), config_.browser_backend, platform_);
}
std::optional<std::string_view> BackendRouter::alternate_firefox_backend(const Account &account) {
    if (account.id.empty()) return std::nullopt;
    const auto stored = store_.account_backend(account.id);
    if (!stored || *stored != "auto") return std::nullopt;
    const auto active = backend_for(account);
    if (active != "camoufox" && active != "spidermonkey") return std::nullopt;
    const std::string_view alternate = active == "camoufox" ? "spidermonkey" : "camoufox";
    if (alternate == "spidermonkey" && !spidermonkey_available()) return std::nullopt;
    Config cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
    }
    if (!readiness_(alternate, cfg)) return std::nullopt;
    return alternate;
}
void BackendRouter::reconfigure(const Config &config) {
    std::vector<GenerationBackend *> all;
    {
        std::lock_guard lock(mutex_);
        config_ = config;
        for (auto &[name, backend] : gateways_) all.push_back(backend.get());
    }
    for (auto *backend : all) backend->reconfigure(config);
}
void BackendRouter::close() {
    const auto transition_lock = transition_mutex_for(this);
    std::lock_guard transition(*transition_lock);
    std::vector<GenerationBackend *> all;
    {
        std::lock_guard lock(mutex_);
        current_.clear();
        for (auto &[name, backend] : gateways_) all.push_back(backend.get());
    }
    for (auto *backend : all) backend->close();
}
void BackendRouter::reset_session() {
    if (auto *backend = current()) backend->reset_session();
}
void BackendRouter::clear_template() {
    if (auto *backend = current()) backend->clear_template();
}
bool BackendRouter::release_idle() {
    std::vector<GenerationBackend *> all;
    {
        std::lock_guard lock(mutex_);
        for (auto &[name, backend] : gateways_) all.push_back(backend.get());
    }
    bool released = false;
    for (auto *backend : all) released = backend->release_idle() || released;
    return released;
}
} // namespace ais2api
