// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/common.hpp"
#include <functional>

namespace ais2api::browser_detail {
// Incremental text/event-stream parser for the public Gemini API's
// `alt=sse` responses. Emits the data of each event, with the lines of a
// multi-line data field joined by "\n". Other fields and comments are
// ignored. An event larger than `max_event_bytes` fails as invalid_upstream.
class SseParser {
public:
    explicit SseParser(std::size_t max_event_bytes) : limit_(max_event_bytes) {}
    void feed(std::string_view bytes, const std::function<void(std::string_view)>& emit) {
        while (!bytes.empty()) {
            // A CRLF split across two feeds: the CR already ended the line.
            if (after_cr_ && bytes.front() == '\n') bytes.remove_prefix(1);
            after_cr_ = false;
            const auto end = bytes.find_first_of("\r\n");
            const auto piece = bytes.substr(0, end);
            if (line_.size() + piece.size() + data_.size() > limit_)
                throw Error(502, "invalid_upstream", "Gemini API stream event exceeds the object limit");
            line_.append(piece);
            if (end == std::string_view::npos) return;
            after_cr_ = bytes[end] == '\r';
            bytes.remove_prefix(end + 1);
            line(emit);
        }
    }
    // Dispatches an event the stream ended without a blank line after.
    void finish(const std::function<void(std::string_view)>& emit) {
        if (!line_.empty()) line(emit);
        dispatch(emit);
    }
private:
    std::size_t limit_;
    std::string line_, data_;
    bool has_data_ = false, after_cr_ = false;
    void dispatch(const std::function<void(std::string_view)>& emit) {
        if (!has_data_) return;
        const auto data = std::move(data_);
        data_.clear();
        has_data_ = false;
        emit(data);
    }
    void line(const std::function<void(std::string_view)>& emit) {
        const auto text = std::move(line_);
        line_.clear();
        if (text.empty()) { dispatch(emit); return; }
        if (text.front() == ':') return;
        const auto colon = text.find(':');
        if (std::string_view(text).substr(0, colon) != "data") return;
        std::string_view value;
        if (colon != std::string::npos) {
            value = std::string_view(text).substr(colon + 1);
            if (value.starts_with(' ')) value.remove_prefix(1);
        }
        if (has_data_) data_ += '\n';
        data_.append(value);
        has_data_ = true;
    }
};
} // namespace ais2api::browser_detail
