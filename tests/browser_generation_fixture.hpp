// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "../browser/generation.hpp"

namespace generation_fixture {
using namespace ais2api;
inline std::string base64(std::string_view bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    unsigned bits = 0, count = 0;
    for (unsigned char byte : bytes) {
        bits = (bits << 8) | byte;
        count += 8;
        while (count >= 6) {
            count -= 6;
            result.push_back(alphabet[(bits >> count) & 63]);
        }
    }
    if (count)
        result.push_back(alphabet[(bits << (6 - count)) & 63]);
    while (result.size() % 4)
        result.push_back('=');
    return result;
}
inline json::object pending(std::string_view bytes, std::int64_t sequence = 1) {
    return {{"pending", json::object{{"sequence", sequence}, {"bytes", base64(bytes)}}},
            {"done", false},
            {"status", 200},
            {"benefitTier", "pro"}};
}
inline json::object done() {
    return {{"pending", nullptr},
            {"done", true},
            {"status", 200},
            {"error", ""},
            {"benefitTier", "pro"}};
}
inline json::object denied(int status = 403, std::string tier = "pro") {
    return {{"pending", nullptr},
            {"done", true},
            {"status", status},
            {"error", "upstream_http_" + std::to_string(status)},
            {"errorBody", json::serialize(json::array{status == 403 ? 7 : status,
                                                      "Synthetic upstream denial", json::array{}})},
            {"benefitTier", tier},
            {"retryAfter", 0}};
}
inline json::array reply(std::string text, int output, int finish, std::string checkpoint = {},
                         int prompt = 44976) {
    json::array candidate(9);
    candidate[0] = json::array{json::array{json::array{nullptr, text}}, "model"};
    candidate[1] = finish;
    if (!checkpoint.empty())
        candidate[8] = checkpoint;
    return {json::array{candidate},
            nullptr,
            json::array{prompt, output, prompt + output},
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            "synthetic-wire-response"};
}
inline std::string stream(const json::array &chunk) {
    return ")]}'\n[[" + json::serialize(chunk) + "]]";
}
inline std::vector<json::value> response(const json::array &chunk) {
    return {pending(stream(chunk)), done()};
}
inline std::vector<json::value> first() {
    return response(reply("hello ", 32768, 2, "private-synthetic-checkpoint"));
}
inline std::vector<json::value> last() {
    auto chunk = reply("world", 11955, 1, {}, 77744);
    chunk[7] = "synthetic-wire-next-response";
    return response(chunk);
}

// Only I/O is scripted. Parsing, ACKs, limits and retries are production code.
struct Script {
    std::vector<std::vector<json::value>> attempts;
    std::vector<std::string> bodies, checkpoints;
    std::vector<std::vector<std::uint64_t>> acknowledgments;
    std::vector<Clock::time_point> sent_at;
    std::function<void(std::size_t, const std::function<void()> &)> before_send;
    std::function<void(std::size_t, std::size_t)> before_next;
    std::size_t starts = 0, current = 0, position = 0, completions = 0;

    browser_detail::GenerationIO io(const Request &request) {
        browser_detail::GenerationIO result;
        result.encode = [this, &request](std::string_view checkpoint) {
            checkpoints.emplace_back(checkpoint);
            return json::serialize(
                encode_request(request, {}, "!synthetic-snapshot", "UTC", checkpoint));
        };
        result.start = [this](std::string_view body, const std::function<void()> &check) {
            current = starts++;
            position = 0;
            check();
            if (before_send)
                before_send(current, check);
            check();
            bodies.emplace_back(body);
            sent_at.push_back(Clock::now());
            acknowledgments.emplace_back();
        };
        result.next = [this](std::uint64_t acknowledgment) {
            acknowledgments.back().push_back(acknowledgment);
            if (before_next)
                before_next(current, position);
            return attempts.at(current).at(position++);
        };
        result.completed = [this] { ++completions; };
        return result;
    }
};
} // namespace generation_fixture
