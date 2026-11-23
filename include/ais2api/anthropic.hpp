// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "protocol.hpp"

namespace ais2api {
// An Anthropic Messages request lowered to a native Gemini request body, which
// normalize_gemini then validates like any other.
struct AnthropicRequest {
    std::string model;
    bool stream = false;
    // Whether the client asked for thinking: the reply then carries thinking
    // blocks, whose signatures make the next turn's thought signatures.
    bool thinking = false;
    json::object gemini;
    // Fields dropped under ignore_unsupported_parameters, by name only.
    json::array ignored;
};
// `count_only` accepts the /v1/messages/count_tokens body (no max_tokens).
AnthropicRequest anthropic_to_gemini(const json::object &input, bool ignore_unsupported,
                                     bool count_only = false, const Config &config = {});
// The client's thinking setting, read before the request is queued.
bool anthropic_thinking(const json::object &input);

// Local token estimate of a lowered request: text and JSON through the Gemma 4
// tokenizer, media at a fixed per-item cost.
std::size_t anthropic_input_tokens(const json::object &gemini);

// {"type":"error","error":{"type":...,"message":...}} with the type the
// Anthropic API uses for the status.
json::object anthropic_error(const Error &error);

// Streams one Gemini response as Anthropic SSE events (each event's "type" is
// its SSE name). anthropic_response replays the same events into a message,
// so both shapes always agree.
class AnthropicStream {
  public:
    AnthropicStream(std::string model, std::string id, bool thinking);
    json::array push(const json::object &gemini_delta);
    json::array finish();
    json::array fail(const Error &error);
    // Parts with no Anthropic block (images, code execution) seen so far.
    std::size_t dropped() const { return dropped_; }

  private:
    enum class Block { none, text, thinking, tool_use };
    void open(json::array &events, Block type, json::object block);
    void close(json::array &events);
    void signature(json::array &events, const json::value &value);
    json::object usage() const;
    std::string model_, id_;
    bool thinking_;
    // visible_: a text or tool_use block was opened, so a new thinking block
    // would trail visible content.
    bool started_ = false, signed_ = false, calls_ = false, visible_ = false;
    Block block_ = Block::none;
    std::size_t index_ = 0, ordinal_ = 0, dropped_ = 0;
    std::string finish_reason_;
    json::object usage_;
};
json::object anthropic_response(const json::object &gemini, std::string_view model, std::string_view id,
                                bool thinking, std::size_t *dropped = nullptr);
} // namespace ais2api
