// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/quota_service.hpp"
#include "ais2api/download.hpp"
#include <utility>

namespace ais2api {
namespace {
#include "quota_snapshot.inc"
constexpr std::size_t download_limit = 1024 * 1024;
std::string fetch_catalog(std::string_view url, const std::string &proxy, std::stop_token token) {
    return fetch_small_https(url, proxy, token, download_limit, "quota");
}
// The stored row: {catalog, fetched_at, attempted_at, error}.
std::optional<json::object> stored(Store &store) {
    auto saved = store.price_sources();
    auto row = saved.if_contains(quota_source);
    if (!row || !row->is_object() || !row->as_object().at("catalog").is_object()) return std::nullopt;
    return row->as_object();
}
} // namespace

std::string_view embedded_quota_catalog() {
    return {reinterpret_cast<const char *>(embedded_quota_bytes), sizeof embedded_quota_bytes};
}

QuotaService::QuotaService(Store &store, std::function<Config()> config, bool automatic, Fetch fetch)
    : store_(store), config_(std::move(config)), fetch_(fetch ? std::move(fetch) : fetch_catalog),
      automatic_(automatic) {
    // Seed before the first policy() so scheduling never starts without the
    // embedded table.
    seed();
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
}
QuotaService::~QuotaService() { stop(); }
void QuotaService::stop() {
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
}
void QuotaService::refresh() {
    {
        std::lock_guard lock(mutex_);
        requested_ = true;
    }
    wake_.notify_all();
}
void QuotaService::load_catalog() {
    json::object catalog;
    if (auto row = stored(store_))
        catalog = row->at("catalog").as_object();
    std::lock_guard lock(mutex_);
    if (catalog != catalog_) {
        catalog_ = std::move(catalog);
        policy_.reset();
    }
}
void QuotaService::seed() {
    // Install the embedded snapshot unless a stored catalog is as recent.
    try {
        auto snapshot = parse_quota_catalog(json::parse(embedded_quota_catalog()), 0);
        auto row = stored(store_);
        if (!row || quota_catalog_published(row->at("catalog").as_object()) < quota_catalog_published(snapshot))
            store_.seed_price_source(quota_source, snapshot);
    } catch (const std::exception &) {
        std::lock_guard lock(mutex_);
        error_ = "quota_snapshot_invalid";
    }
    load_catalog();
}
json::object QuotaService::status() {
    json::object source{{"url", quota_catalog_url}, {"fetched_at", 0}, {"attempted_at", 0}, {"error", ""},
                        {"published_at", ""}, {"models", 0}, {"embedded", false}};
    if (auto row = stored(store_)) {
        for (auto key : {"fetched_at", "attempted_at", "error"})
            source[key] = row->at(key);
        const auto &catalog = row->at("catalog").as_object();
        if (auto models = catalog.if_contains("models"); models && models->is_object())
            source["models"] = models->as_object().size();
        source["published_at"] = quota_catalog_published(catalog);
        // Never downloaded: the table comes from the build's snapshot.
        source["embedded"] = !catalog.empty() && row->at("fetched_at").as_int64() == 0;
    }
    auto effective = policy(config_())->effective();
    std::lock_guard lock(mutex_);
    return {{"refreshing", refreshing_ || requested_},
            {"error", error_},
            {"source", std::move(source)},
            {"models", std::move(effective)}};
}
std::shared_ptr<const TierPolicy> QuotaService::policy(const Config &config) {
    std::lock_guard lock(mutex_);
    if (!policy_ || overrides_ != config.tier_quotas) {
        overrides_ = config.tier_quotas;
        json::object overrides;
        // Config::validate() already rejected malformed overrides.
        try {
            overrides = parse_quota_overrides(overrides_);
        } catch (const Error &) {
        }
        policy_ = std::make_shared<const TierPolicy>(catalog_, overrides);
    }
    return policy_;
}
void QuotaService::run(std::stop_token token) {
    while (!token.stop_requested()) {
        bool manual;
        {
            std::lock_guard lock(mutex_);
            manual = std::exchange(requested_, false);
            refreshing_ = manual;
        }
        try {
            auto cfg = config_();
            auto row = stored(store_);
            const auto now = unix_seconds();
            const bool due = !row || (now - row->at("fetched_at").as_int64() >= quota_refresh_seconds &&
                                      now - row->at("attempted_at").as_int64() >= quota_retry_seconds);
            if (manual || (automatic_ && due)) {
                {
                    std::lock_guard lock(mutex_);
                    refreshing_ = true;
                }
                try {
                    auto text = fetch_(quota_catalog_url, cfg.proxy, token);
                    boost::system::error_code ec;
                    auto data = json::parse(text, ec);
                    if (ec)
                        throw Error(502, "invalid_quota_json", "Quota source returned invalid JSON");
                    auto catalog = parse_quota_catalog(data, now);
                    // A lagging download never replaces a newer stored table
                    // (such as a fresher embedded snapshot).
                    if (row) {
                        const auto &previous = row->at("catalog").as_object();
                        if (quota_catalog_published(catalog) < quota_catalog_published(previous)) {
                            catalog = previous;
                            catalog["fetched_at"] = now;
                        }
                    }
                    store_.save_price_source(quota_source, catalog, now);
                } catch (const Error &e) {
                    if (!token.stop_requested())
                        store_.save_price_source(quota_source, {}, 0, e.code);
                }
                load_catalog();
            }
            std::lock_guard lock(mutex_);
            if (error_ != "quota_snapshot_invalid")
                error_.clear();
        } catch (const Error &e) {
            std::lock_guard lock(mutex_);
            error_ = e.code;
        } catch (const std::exception &) {
            std::lock_guard lock(mutex_);
            error_ = "quota_refresh_internal";
        }
        std::unique_lock lock(mutex_);
        refreshing_ = false;
        wake_.wait_for(lock, token, std::chrono::minutes(1), [&] { return requested_; });
    }
}
} // namespace ais2api
