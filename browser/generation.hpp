// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "ais2api/protocol.hpp"

namespace ais2api::browser_detail {
// The account, page and snapshot belong to the caller for this entire call.
// start consumes body before returning and must check again after preparing the
// upload, before sending it.
struct GenerationIO {
    std::function<std::string(std::string_view checkpoint)> encode;
    std::function<void(std::string_view body, const std::function<void()> &check)> start;
    std::function<json::value(std::uint64_t acknowledgment)> next;
    std::function<void()> completed;
};
// Production and controlled transports share parsing, backpressure, continuation
// and bounded empty-403 retries. Only completed rounds advance the checkpoint.
void run_generation(const Request &request, const Limits &limits, bool retry_enabled,
                    const GenerationIO &io, const EventSink &sink, std::stop_token cancel);
} // namespace ais2api::browser_detail
