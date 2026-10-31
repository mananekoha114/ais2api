// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "store.hpp"
namespace ais2api {
// Shared classification for client finish reasons and request accounting.
bool content_filtered(std::string_view reason);
enum class Protocol { gemini, openai };
struct Request {
    Protocol protocol = Protocol::gemini;
    std::string model, principal;
    bool stream = false;
    bool include_usage = false;
    bool include_thoughts = true;
    json::array ignored_parameters;
    json::object gemini;
};
using EventSink = std::function<void(json::object)>;
Request normalize_gemini(std::string model, const json::object& body, bool stream, const Config& config);
// Whether model listings add a "<id>-search" variant (Google Search on by
// default) for this discovered or configured model entry.
bool lists_search_variant(std::string_view id, const json::object& entry);
Request normalize_openai(const json::object& body, const Config& config, Store* store = nullptr, std::string principal = {});
json::array encode_contents(const json::array& contents);
json::array encode_schema(const json::object& schema);
json::array encode_request(const Request& request, const json::array& base_template, std::string_view browser_snapshot,
                           std::string_view timezone = "UTC");
// Wire locations skipped while decoding (unknown or malformed non-function
// slots) are reported under this internal key so the service can log them;
// take_upstream_ignored removes it before the event reaches a client.
inline constexpr std::string_view upstream_ignored_key = "ais2apiUpstreamIgnored";
json::object decode_chunk(const json::value& wire_chunk);
json::array take_upstream_ignored(json::object& event);
void filter_thoughts(json::object& event);
bool has_content(const json::object& event);
// Merges one streamed event into `aggregate`, failing without changes when the
// result would exceed `byte_limit` serialized bytes. A caller merging a whole
// stream passes `size_bound`, an upper bound on the aggregate's serialized size
// (2 for an empty object) that this function maintains, to avoid re-measuring
// the aggregate on every event.
void merge_event(json::object& aggregate, const json::object& event, std::size_t byte_limit,
                 std::size_t* size_bound = nullptr);
// Whether merging appends part `b` to the preceding part `a` as one text part.
bool joins_text(const json::object& a, const json::object& b);
// The size_bound growth merge_event allows for an event with these counts of
// candidates and content parts.
std::size_t merge_growth_bound(const json::object& event, std::size_t candidates, std::size_t parts);
json::object openai_response(const json::object& gemini, std::string_view model, std::string_view id, Store* store = nullptr, std::string_view principal = {});
json::object openai_delta(const json::object& gemini_delta, std::string_view model, std::string_view id, Store* store = nullptr, std::string_view principal = {});
class WireStreamParser {
public:
    explicit WireStreamParser(std::size_t max_object_bytes = 64 * 1024 * 1024);
    void feed(std::string_view bytes, const std::function<void(json::value)>& emit);
    void finish();
    std::size_t buffered_bytes() const;
private:
    std::size_t limit_, depth_ = 0;
    std::string buffer_, preamble_;
    bool quoted_ = false, escaped_ = false, started_ = false, prefix_done_ = false;
    std::size_t collecting_depth_ = 0;
};
}
