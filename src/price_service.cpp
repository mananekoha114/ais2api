// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/price_service.hpp"
#include "ais2api/pricing.hpp"
#include <curl/curl.h>
#include <utility>

namespace ais2api {
namespace {
#include "pricing_snapshot.inc"
constexpr std::size_t download_limit = 4 * 1024 * 1024;
std::string fetch_catalog(std::string_view url, const std::string &proxy,
                          std::stop_token token) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK)
        throw Error(502, "price_network_init", "Cannot initialize price download");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw Error(502, "price_network_init", "Cannot initialize price download");
    const std::string address(url);
    struct Download {
        std::string text;
        bool exceeded = false;
    } data;
    curl_easy_setopt(curl.get(), CURLOPT_URL, address.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "ais2api/0.1");
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    if (!proxy.empty())
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, proxy.c_str());
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char *ptr, std::size_t size, std::size_t count, void *context) -> std::size_t {
            auto &d = *static_cast<Download *>(context);
            if (size && count > (download_limit - d.text.size()) / size) {
                d.exceeded = true;
                return 0;
            }
            auto bytes = size * count;
            try {
                d.text.append(ptr, bytes);
            } catch (...) {
                return 0;
            }
            return bytes;
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<std::stop_token *>(context)->stop_requested() ? 1 : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &token);
    auto code = curl_easy_perform(curl.get());
    check_cancel(token);
    if (data.exceeded)
        throw Error(502, "price_catalog_too_large", "Price catalog exceeds 4 MiB");
    if (code != CURLE_OK)
        throw Error(502, "price_download_failed", "Price catalog download failed");
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200)
        throw Error(502, "price_http_" + std::to_string(status), "Price source rejected download");
    return std::move(data.text);
}
} // namespace
std::string_view embedded_price_catalog() {
    return {reinterpret_cast<const char *>(embedded_pricing_bytes), sizeof embedded_pricing_bytes};
}
PriceService::PriceService(Store &store, std::function<Config()> config, bool automatic,
                           Fetch fetch)
    : store_(store), config_(std::move(config)), fetch_(fetch ? std::move(fetch) : fetch_catalog),
      automatic_(automatic), worker_([this](std::stop_token token) { run(token); }) {}
PriceService::~PriceService() { stop(); }
void PriceService::stop() {
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
}
void PriceService::refresh() {
    {
        std::lock_guard lock(mutex_);
        requested_ = true;
    }
    wake_.notify_all();
}
json::object PriceService::status() {
    json::array sources;
    auto saved = store_.price_sources();
    json::object item{{"source", price_source}, {"url", price_catalog_url}, {"fetched_at", 0},
                      {"attempted_at", 0}, {"error", ""}, {"models", 0}, {"published_at", ""},
                      {"embedded", false}};
    if (auto p = saved.if_contains(price_source)) {
        auto &s = p->as_object();
        for (auto key : {"fetched_at", "attempted_at", "error"})
            item[key] = s.at(key);
        auto &catalog = s.at("catalog").as_object();
        item["models"] = catalog.size();
        item["published_at"] = price_catalog_published(catalog);
        // Never downloaded: the prices come from the build's snapshot.
        item["embedded"] = !catalog.empty() && s.at("fetched_at").as_int64() == 0;
    }
    sources.push_back(std::move(item));
    std::lock_guard lock(mutex_);
    return {{"currency", "USD"},
            {"refreshing", refreshing_ || requested_},
            {"sources", std::move(sources)},
            {"error", error_}};
}
void PriceService::seed() {
    // Install the embedded snapshot unless a stored catalog is as recent.
    try {
        auto snapshot = parse_price_catalog(json::parse(embedded_price_catalog()), 0);
        auto saved = store_.price_sources();
        auto p = saved.if_contains(price_source);
        if (!p || price_catalog_published(p->at("catalog").as_object()) < price_catalog_published(snapshot))
            store_.seed_price_source(price_source, snapshot);
    } catch (const std::exception &) {
        std::lock_guard lock(mutex_);
        error_ = "price_snapshot_invalid";
    }
}
void PriceService::run(std::stop_token token) {
    seed();
    while (!token.stop_requested()) {
        bool manual;
        {
            std::lock_guard lock(mutex_);
            manual = std::exchange(requested_, false);
            refreshing_ = manual;
        }
        try {
            auto cfg = config_();
            store_.prune_requests(
                (unix_seconds() - static_cast<std::int64_t>(cfg.request_retention_days) * 86400) *
                1000);
            auto saved = store_.price_sources();
            auto p = saved.if_contains(price_source);
            bool due = !p || (unix_seconds() - p->at("fetched_at").as_int64() >= 86400 &&
                              unix_seconds() - p->at("attempted_at").as_int64() >= 3600);
            if (manual || (automatic_ && due)) {
                {
                    std::lock_guard lock(mutex_);
                    refreshing_ = true;
                }
                try {
                    auto text = fetch_(price_catalog_url, cfg.proxy, token);
                    boost::system::error_code ec;
                    auto data = json::parse(text, ec);
                    if (ec)
                        throw Error(502, "invalid_price_json",
                                    "Price source returned invalid JSON");
                    auto now = unix_seconds();
                    auto catalog = parse_price_catalog(data, now);
                    // A lagging download never replaces newer stored prices
                    // (such as a fresher embedded snapshot).
                    if (p && price_catalog_published(catalog) <
                                 price_catalog_published(p->at("catalog").as_object())) {
                        catalog = p->at("catalog").as_object();
                        for (auto &entry : catalog)
                            entry.value().as_object()["fetched_at"] = now;
                    }
                    store_.save_price_source(price_source, catalog, now);
                } catch (const Error &e) {
                    if (!token.stop_requested())
                        store_.save_price_source(price_source, {}, 0, e.code);
                }
            }
            std::lock_guard lock(mutex_);
            error_.clear();
        } catch (const Error &e) {
            std::lock_guard lock(mutex_);
            error_ = e.code;
        } catch (const std::exception &) {
            std::lock_guard lock(mutex_);
            error_ = "price_refresh_internal";
        }
        std::unique_lock lock(mutex_);
        refreshing_ = false;
        wake_.wait_for(lock, token, std::chrono::minutes(1), [&] { return requested_; });
    }
}
} // namespace ais2api
