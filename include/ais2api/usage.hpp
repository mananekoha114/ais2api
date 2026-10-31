// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "store.hpp"
#include <mutex>

namespace ais2api {
// Derive usage, known costs and outcome from stored attempts without repricing.
// Shared by live finalization, restart recovery and versioned history migration.
void summarize_request(json::object &record, bool terminal);

// Joins producer completion and HTTP delivery. Either thread may finish first;
// the stored terminal row and totals are written only after both have finished.
class RequestUsage {
  public:
    RequestUsage(Store &store, std::string id, std::string endpoint);
    const std::string &id() const { return id_; }
    void configure(std::string_view model, bool stream);
    bool start();
    void normalized(std::string_view model, const json::object &gemini);
    void begin_attempt(std::string_view account);
    void event(const json::object &event, bool content);
    void end_attempt(std::string_view error = {});
    void producer_done(std::string_view error = {});
    void http_done(unsigned status, std::string_view error, bool delivered);

  private:
    Store &store_;
    const std::string id_;
    std::mutex mutex_;
    json::object record_, quote_;
    bool started_ = false, producer_done_ = false, http_done_ = false, terminal_ = false;
    bool additional_ = false, attempt_open_ = false, usage_valid_ = true;
    Clock::time_point created_ = Clock::now(), started_at_ = created_;
    void save();
    void end_attempt_locked(std::string_view error);
};
} // namespace ais2api
