// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend_router.hpp"
#include "ais2api/browser.hpp"
#include "ais2api/camoufox.hpp"
#include <algorithm>

namespace ais2api {
bool valid_account_backend(std::string_view backend) {
    return backend == "cloakbrowser" || backend == "auto" || backend == "camoufox" || backend == "spidermonkey";
}
std::string_view adopted_backend(std::string_view default_backend) {
    return default_backend == "camoufox" ? "auto" : "cloakbrowser";
}
// Always one of the literals, never a view of the arguments.
std::string_view effective_backend(std::string_view stored, std::string_view default_backend) {
    if (stored == "cloakbrowser") return "cloakbrowser";
    if (stored == "auto" || stored == "camoufox" || stored == "spidermonkey") return "camoufox";
    return default_backend == "camoufox" ? "camoufox" : "cloakbrowser";
}
std::string_view identity_family(std::string_view backend) {
    return backend == "cloakbrowser" ? "chrome" : "firefox";
}
std::vector<std::string> backends_needed(std::string_view default_backend, const std::vector<std::string> &stored) {
    std::vector<std::string> result{std::string(effective_backend({}, default_backend))};
    for (const auto &backend : stored) {
        std::string next(effective_backend(backend, default_backend));
        if (std::find(result.begin(), result.end(), next) == result.end()) result.push_back(std::move(next));
    }
    return result;
}
json::array backend_deprecations(std::string_view default_backend, const std::vector<std::string> &stored) {
    const bool by_default = effective_backend({}, default_backend) == "cloakbrowser";
    const bool accounts = std::any_of(stored.begin(), stored.end(), [&](const std::string &backend) {
        return effective_backend(backend, default_backend) == "cloakbrowser";
    });
    if (!by_default && !accounts) return {};
    return {json::object{{"code", "cloakbrowser_backend"}, {"default_backend", by_default}, {"accounts", accounts}}};
}
void adopt_backends(Store &store, const Config &config) {
    store.adopt_backends(adopted_backend(config.browser_backend));
}

namespace {
std::unique_ptr<GenerationBackend> browser_gateway(std::string_view backend, const Config &config, Store &store) {
    if (backend == "camoufox") return std::make_unique<CamoufoxGateway>(config, store);
    return std::make_unique<BrowserGateway>(config, store);
}
} // namespace

BackendRouter::BackendRouter(const Config &config, Store &store, Factory factory)
    : store_(store), factory_(factory ? std::move(factory) : Factory(browser_gateway)), config_(config) {}
BackendRouter::~BackendRouter() = default;

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
    const std::string backend(backend_for(account));
    GenerationBackend *next = nullptr, *previous = nullptr;
    {
        std::lock_guard lock(mutex_);
        next = &gateway(backend);
        if (const auto it = gateways_.find(current_); it != gateways_.end() && it->first != backend)
            previous = it->second.get();
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
json::object BackendRouter::diagnostics() {
    GenerationBackend *target = nullptr;
    std::string backend, fallback;
    {
        std::lock_guard lock(mutex_);
        fallback = effective_backend({}, config_.browser_backend);
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
    std::lock_guard lock(mutex_);
    return effective_backend(stored.value_or(std::string{}), config_.browser_backend);
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
    std::vector<GenerationBackend *> all;
    {
        std::lock_guard lock(mutex_);
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
