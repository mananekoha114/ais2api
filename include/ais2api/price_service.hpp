// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "store.hpp"
#include <condition_variable>
#include <thread>

namespace ais2api {
class PriceService {
  public:
    using Fetch =
        std::function<std::string(std::string_view, const std::string &, std::stop_token)>;
    PriceService(Store &store, std::function<Config()> config, bool automatic = true,
                 Fetch fetch = {});
    ~PriceService();
    json::object status();
    void refresh();
    void stop();

  private:
    Store &store_;
    std::function<Config()> config_;
    Fetch fetch_;
    bool automatic_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool requested_ = false, refreshing_ = false;
    std::string error_;
    std::jthread worker_;
    void run(std::stop_token token);
};
} // namespace ais2api
