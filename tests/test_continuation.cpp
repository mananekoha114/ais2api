// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "ais2api/continuation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <thread>
using namespace ais2api;
namespace {
Request budget(std::int64_t count) {
    Request r;
    r.output_budget = count;
    return r;
}
json::object checkpoint(std::int64_t output, std::string token = "checkpoint-1",
                        std::string finish = "MAX_TOKENS", std::int64_t thoughts = 0) {
    json::array parts;
    parts.push_back(json::array{nullptr, "text"});
    json::array candidate(9);
    candidate[0] = json::array{std::move(parts), "model"};
    candidate[1] = finish == "STOP" ? 1 : finish == "PROHIBITED_CONTENT" ? 8 : 2;
    candidate[8] = token;
    json::array candidates;
    candidates.push_back(std::move(candidate));
    json::array usage(10);
    usage[0] = 10;
    usage[1] = output - thoughts;
    usage[2] = output + 10;
    usage[9] = thoughts;
    return decode_chunk(json::array{std::move(candidates), nullptr, usage, nullptr, nullptr,
                                    nullptr, nullptr, "round-id"});
}
std::string error_code(const std::function<void()> &operation) {
    try {
        operation();
    } catch (const Error &error) {
        return error.code;
    }
    return {};
}
} // namespace

TEST_CASE("Private checkpoints stay internal and only the last finish reaches clients",
          "[continuation]") {
    NativeContinuation state(budget(65536));
    std::vector<GenerationEvent> events;
    EventSink sink = [&](GenerationEvent e) { events.push_back(std::move(e)); };
    auto event = checkpoint(32768);
    REQUIRE(event.at("candidates").at(0).as_object().contains(continuation_token_key));
    state.push(event, sink);
    REQUIRE(events.size() == 1);
    CHECK_FALSE(events[0].data.at("candidates").at(0).as_object().contains("finishReason"));
    CHECK(json::serialize(events[0].data).find("checkpoint-1") == std::string::npos);
    REQUIRE(state.finish(sink));
    REQUIRE(state.token() == "checkpoint-1");
    REQUIRE(events.back().kind == GenerationEvent::Kind::round_finished);
    event = checkpoint(11955, "", "STOP");
    event["responseId"] = "different-round-id";
    state.push(event, sink);
    REQUIRE(events.back().data.at("responseId") == "round-id");
    REQUIRE_FALSE(state.finish(sink));
    REQUIRE(events.back().kind == GenerationEvent::Kind::chunk);
    REQUIRE(events.back().data.at("candidates").at(0).at("finishReason") == "STOP");
    for (auto &e : events)
        CHECK(json::serialize(e.data).find("checkpoint-1") == std::string::npos);
}

TEST_CASE("Completed rounds carry the latest usage independently of the terminal chunk",
          "[continuation][usage][round-snapshot]") {
    NativeContinuation state(budget(65536));
    std::vector<GenerationEvent> events;
    EventSink sink = [&](GenerationEvent event) { events.push_back(std::move(event)); };
    const auto usage = checkpoint(32768).at("usageMetadata").as_object();
    state.push({{"usageMetadata", checkpoint(100).at("usageMetadata")}}, sink);
    state.push({{"usageMetadata", usage}}, sink);
    auto terminal = checkpoint(32768);
    terminal.erase("usageMetadata");
    state.push(std::move(terminal), sink);
    REQUIRE(state.finish(sink));
    REQUIRE(events.back().kind == GenerationEvent::Kind::round_finished);
    CHECK(events.back().data.at("usageMetadata") == usage);
    CHECK(events.back().data.at("candidates").at(0).at("finishReason") == "MAX_TOKENS");
    CHECK(json::serialize(events.back().data).find("checkpoint-1") == std::string::npos);
}

TEST_CASE("Continuation obeys the total output budget including thought tokens", "[continuation]") {
    std::vector<GenerationEvent> events;
    EventSink sink = [&](GenerationEvent e) { events.push_back(std::move(e)); };
    SECTION("default budget ends after one round") {
        NativeContinuation state(budget(32768));
        state.push(checkpoint(32768), sink);
        REQUIRE_FALSE(state.finish(sink));
        REQUIRE(events.back().data.at("candidates").at(0).at("finishReason") == "MAX_TOKENS");
    }
    SECTION("three rounds use a single budget") {
        NativeContinuation state(budget(70000));
        state.push(checkpoint(32768), sink);
        REQUIRE(state.finish(sink));
        state.push(checkpoint(32768, "checkpoint-2"), sink);
        REQUIRE(state.finish(sink));
        state.push(checkpoint(4464, "checkpoint-3"), sink);
        REQUIRE_FALSE(state.finish(sink));
    }
    SECTION("a thinking-only round counts as progress") {
        NativeContinuation state(budget(40000));
        auto thought = checkpoint(32768, "thought-checkpoint", "MAX_TOKENS", 32768);
        auto &part = thought.at("candidates").at(0).at("content").at("parts").at(0).as_object();
        part["thought"] = true;
        part["thoughtSignature"] = "synthetic-signature";
        state.push(thought, sink);
        REQUIRE(state.finish(sink));
        state.push(checkpoint(7232), sink);
        REQUIRE_FALSE(state.finish(sink));
        REQUIRE(json::serialize(events.front().data).find("synthetic-signature") !=
                std::string::npos);
    }
    SECTION("content filters do not resume") {
        NativeContinuation state(budget(65536));
        state.push(checkpoint(5, "token", "PROHIBITED_CONTENT"), sink);
        REQUIRE_FALSE(state.finish(sink));
        REQUIRE(events.back().data.at("candidates").at(0).at("finishReason") ==
                "PROHIBITED_CONTENT");
    }
    SECTION("reported overshoot keeps the delivered answer") {
        NativeContinuation state(budget(40000));
        state.push(checkpoint(40001), sink);
        REQUIRE_FALSE(state.finish(sink));
        REQUIRE(events.back().kind == GenerationEvent::Kind::chunk);
        REQUIRE(events.back().data.at("candidates").at(0).at("finishReason") == "MAX_TOKENS");
    }
    SECTION("an overshooting later round still ends normally") {
        NativeContinuation state(budget(40000));
        state.push(checkpoint(32768), sink);
        REQUIRE(state.finish(sink));
        state.push(checkpoint(20000, "checkpoint-2", "STOP"), sink);
        REQUIRE_FALSE(state.finish(sink));
        REQUIRE(events.back().data.at("candidates").at(0).at("finishReason") == "STOP");
    }
}

TEST_CASE("Non-string checkpoint slots stay ignored for every model family",
          "[continuation][protocol]") {
    json::array parts;
    parts.push_back(json::array{nullptr, "text"});
    json::array candidate(9);
    candidate[0] = json::array{std::move(parts), "model"};
    candidate[1] = 2;
    candidate[8] = 5;
    json::array candidates;
    candidates.push_back(std::move(candidate));
    json::array usage(10);
    usage[0] = 10;
    usage[1] = 32768;
    auto event = decode_chunk(json::array{std::move(candidates), nullptr, usage});
    CHECK_FALSE(event.at("candidates").at(0).as_object().contains(continuation_token_key));
    CHECK(take_upstream_ignored(event) == json::array{"candidate[8]"});
    // A Gemini 4 answer that needs another round reports the checkpoint missing.
    NativeContinuation state(budget(65536));
    EventSink sink = [](GenerationEvent) {};
    state.push(std::move(event), sink);
    CHECK(error_code([&] { state.finish(sink); }) == "continuation_missing_token");
}

TEST_CASE("Unusable checkpoints fail without publishing a final MAX_TOKENS", "[continuation]") {
    NativeContinuation state(budget(100000));
    std::vector<GenerationEvent> events;
    EventSink sink = [&](GenerationEvent e) { events.push_back(std::move(e)); };
    std::string expected;
    auto event = checkpoint(32768);
    SECTION("missing token") {
        event.at("candidates").at(0).as_object().erase(continuation_token_key);
        expected = "continuation_missing_token";
    }
    SECTION("missing usage") {
        event.erase("usageMetadata");
        expected = "continuation_missing_usage";
    }
    SECTION("no progress") {
        event = checkpoint(0);
        expected = "continuation_no_progress";
    }
    SECTION("repeated token") {
        state.push(event, sink);
        REQUIRE(state.finish(sink));
        expected = "continuation_repeated_token";
    }
    state.push(event, sink);
    REQUIRE(error_code([&] { state.finish(sink); }) == expected);
    REQUIRE(events.back().kind == GenerationEvent::Kind::round_finished);
    for (auto &e : events)
        if (e.kind == GenerationEvent::Kind::chunk)
            CHECK_FALSE(e.data.at("candidates").at(0).as_object().contains("finishReason"));
}

TEST_CASE("Transport errors cannot be replaced by a buffered terminal marker", "[continuation]") {
    NativeContinuation state(budget(65536));
    unsigned finished = 0;
    EventSink sink = [&](GenerationEvent e) {
        if (e.kind == GenerationEvent::Kind::round_finished)
            ++finished;
        if (e.kind == GenerationEvent::Kind::chunk)
            CHECK_FALSE(e.data.at("candidates").at(0).as_object().contains("finishReason"));
    };
    state.push(checkpoint(32768), sink);
    // A failing parser/transport never calls finish().
    REQUIRE(finished == 0);
    REQUIRE(state.token().empty());
    EventSink cancelled = [](GenerationEvent) { throw Error(499, "cancelled", "Cancelled"); };
    REQUIRE(error_code([&] { state.finish(cancelled); }) == "cancelled");
    REQUIRE(state.token().empty());
}

TEST_CASE("Usage sums round snapshots and rejects overflow", "[continuation][usage]") {
    auto first = checkpoint(32768).at("usageMetadata").as_object();
    auto second = checkpoint(11955).at("usageMetadata").as_object();
    first["promptTokenCount"] = 44976;
    first["totalTokenCount"] = 77744;
    second["promptTokenCount"] = 77744;
    second["totalTokenCount"] = 89699;
    auto sum = add_usage(first, second);
    REQUIRE(sum.at("promptTokenCount") == 122720);
    REQUIRE(sum.at("candidatesTokenCount") == 44723);
    REQUIRE(sum.at("totalTokenCount") == 167443);
    REQUIRE_THROWS_AS(add_usage({{"promptTokenCount", INT64_MAX}}, {{"promptTokenCount", 1}}),
                      Error);
}

TEST_CASE("Cancellation and the unary deadline survive round boundaries", "[continuation]") {
    const auto deadline = Clock::now() + std::chrono::milliseconds(200);
    std::stop_source cancel;
    NativeContinuation state(budget(65536), deadline, cancel.get_token());
    EventSink sink = [](GenerationEvent) {};
    state.push(checkpoint(32768), sink);
    REQUIRE(state.finish(sink));
    SECTION("time spent in previous rounds consumes the deadline") {
        std::this_thread::sleep_until(deadline);
        REQUIRE(error_code([&] { state.check(); }) == "generation_timeout");
    }
    SECTION("cancelled answers cannot start a new round") {
        cancel.request_stop();
        REQUIRE(error_code([&] { state.check(); }) == "cancelled");
    }
    SECTION("streaming does not use the unary total deadline") {
        auto request = budget(65536);
        request.stream = true;
        NativeContinuation streamed(request, Clock::now() - std::chrono::seconds(1));
        REQUIRE_NOTHROW(streamed.check());
    }
}

TEST_CASE("Final event consumption cannot turn an expired unary generation into success",
          "[continuation][deadline]") {
    const auto deadline = Clock::now() + std::chrono::milliseconds(200);
    NativeContinuation state(budget(65536), deadline);
    state.push(checkpoint(10, "", "STOP"), [](GenerationEvent) {});
    REQUIRE(error_code([&] {
                state.finish([&](GenerationEvent) { std::this_thread::sleep_until(deadline); });
            }) == "generation_timeout");
}

TEST_CASE("Tiny unique checkpoints still have a bounded round count", "[continuation]") {
    NativeContinuation state(budget(65536));
    EventSink sink = [](GenerationEvent) {};
    for (unsigned i = 0; i < 31; ++i) {
        state.push(checkpoint(1, std::to_string(i)), sink);
        REQUIRE(state.finish(sink));
    }
    state.push(checkpoint(1, "last"), sink);
    REQUIRE(error_code([&] { state.finish(sink); }) == "continuation_limit");
}

TEST_CASE("Signed tool parts survive continuation without requesting another tool round",
          "[continuation]") {
    NativeContinuation state(budget(65536));
    json::object aggregate;
    EventSink sink = [&](GenerationEvent e) {
        if (e.kind == GenerationEvent::Kind::chunk)
            merge_event(aggregate, e.data, 65536);
    };
    state.push(checkpoint(32768), sink);
    REQUIRE(state.finish(sink));
    auto tool = checkpoint(50, "", "STOP");
    auto &parts = tool.at("candidates").at(0).at("content").at("parts").as_array();
    parts = json::array{
        json::object{{"functionCall",
                      json::object{{"name", "lookup"}, {"args", json::object{{"q", "synthetic"}}}}},
                     {"thoughtSignature", "synthetic-tool-signature"}}};
    state.push(tool, sink);
    REQUIRE_FALSE(state.finish(sink));
    auto chat = openai_response(aggregate, "gemini-4-test", "logical-id");
    REQUIRE(chat.at("choices").at(0).at("finish_reason") == "tool_calls");
    REQUIRE(chat.at("choices").at(0).at("message").at("tool_calls").as_array().size() == 1);
    REQUIRE(json::serialize(chat).find("synthetic-tool-signature") != std::string::npos);
    REQUIRE(json::serialize(chat).find("checkpoint-1") == std::string::npos);
}
