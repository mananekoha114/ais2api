// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "store.hpp"
#include "tier_quota.hpp"
#include <condition_variable>
#include <memory>
#include <thread>

namespace ais2api {
// The tier quota catalog as maintained on this repository's main branch.
inline constexpr std::string_view quota_catalog_url =
    "https://raw.githubusercontent.com/mananekoha114/ais2api/main/quotas/aistudio.json";
// How often the published catalog is checked, and the pause after a failure.
inline constexpr std::int64_t quota_refresh_seconds = 6 * 3600;
inline constexpr std::int64_t quota_retry_seconds = 3600;
// The catalog snapshot embedded at build time from quotas/aistudio.json.
std::string_view embedded_quota_catalog();
// The effective table without a running QuotaService, for offline commands:
// the stored catalog, or the embedded snapshot when it is newer or nothing is
// stored, with `config`'s tier_quotas. Writes nothing.
TierPolicy stored_tier_policy(Store &store, const Config &config);
// Keeps the newest of the downloaded catalog, the stored copy (the
// price_sources row quota_source) and the embedded snapshot, and combines it
// with the configuration's tier_quotas.
class QuotaService {
  public:
    // Receives the URL and the configured proxy.
    using Fetch =
        std::function<std::string(std::string_view, const std::string &, std::stop_token)>;
    QuotaService(Store &store, std::function<Config()> config, bool automatic = true,
                 Fetch fetch = {});
    ~QuotaService();
    QuotaService(const QuotaService &) = delete;
    QuotaService &operator=(const QuotaService &) = delete;
    // {refreshing, error, source: {url, fetched_at, attempted_at, error,
    // published_at, models, embedded}, models: TierPolicy::effective()}
    json::object status();
    void refresh();
    void stop();
    // The effective table for `config`; rebuilt only when the catalog or the
    // configuration's tier_quotas change.
    std::shared_ptr<const TierPolicy> policy(const Config &config);

  private:
    Store &store_;
    std::function<Config()> config_;
    Fetch fetch_;
    bool automatic_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool requested_ = false, refreshing_ = false;
    std::string error_;
    json::object catalog_, overrides_;
    std::shared_ptr<const TierPolicy> policy_;
    std::jthread worker_;
    void run(std::stop_token token);
    void seed();
    void load_catalog();
};
} // namespace ais2api
