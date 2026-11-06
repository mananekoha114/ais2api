// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "protocol.hpp"
#include <set>

namespace ais2api {
// One logical generation. Holds only checkpoint/terminal metadata; content
// passes straight through the sink and its existing downstream backpressure.
class NativeContinuation {
  public:
    explicit NativeContinuation(const Request &request,
                                std::optional<Clock::time_point> deadline = {},
                                std::stop_token cancel = {})
        : budget_(request.output_budget.value_or(0)), stream_(request.stream),
          deadline_(request.stream ? std::nullopt : deadline), cancel_(cancel) {}
    // Starts the unary deadline once, after the first round has been sent.
    void start_deadline(Clock::time_point deadline) {
        if (!stream_ && !deadline_)
            deadline_ = deadline;
    }
    const std::string &token() const { return token_; }
    void check() const;
    void push(json::object event, const EventSink &sink);
    // Call only after a complete, successful transport/parser round. Returns
    // true when another round is required; errors never become a normal finish.
    bool finish(const EventSink &sink);

  private:
    std::int64_t budget_, output_ = 0;
    const bool stream_;
    std::optional<Clock::time_point> deadline_;
    const std::stop_token cancel_;
    unsigned rounds_ = 0;
    std::string token_, checkpoint_, response_id_;
    std::set<std::string> checkpoints_;
    json::object terminal_, usage_;
};
} // namespace ais2api
