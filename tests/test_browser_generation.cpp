// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "browser_generation_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <thread>

using namespace ais2api;
using namespace std::chrono_literals;
namespace gf = generation_fixture;
namespace {
Request request(bool streaming = false) {
    return normalize_gemini(
        "gemini-4-synthetic",
        json::parse(
            R"({"contents":[{"parts":[{"text":"original context"}]}],"generationConfig":{"maxOutputTokens":65536}})")
            .as_object(),
        streaming, Config{});
}
std::string run(gf::Script &script, const Request &r, std::vector<GenerationEvent> &events,
                Limits limits = {}, std::stop_token cancel = {}, EventSink observe = {}) {
    try {
        browser_detail::run_generation(
            r, limits, script.io(r),
            [&](GenerationEvent event) {
                if (observe)
                    observe(event);
                events.push_back(std::move(event));
            },
            cancel);
    } catch (const Error &error) {
        return error.code;
    }
    return {};
}
std::size_t count(const std::vector<GenerationEvent> &events, GenerationEvent::Kind kind) {
    return std::count_if(events.begin(), events.end(),
                         [&](const auto &e) { return e.kind == kind; });
}
} // namespace

TEST_CASE("Browser continuation replays one empty HTTP denial with the identical body",
          "[browser][continuation][continuation-retry]") {
    gf::Script script;
    script.attempts = {gf::first(), {gf::denied()}, gf::last()};
    std::vector<GenerationEvent> events;
    REQUIRE(run(script, request(), events).empty());
    REQUIRE(script.bodies.size() == 3);
    REQUIRE(script.checkpoints == std::vector<std::string>{"", "private-synthetic-checkpoint"});
    CHECK(script.bodies[1] == script.bodies[2]);
    CHECK(script.bodies[0] != script.bodies[1]);
    CHECK(json::parse(script.bodies[2]).at(4) == "!synthetic-snapshot");
    CHECK(json::parse(script.bodies[2]).at(16) == "private-synthetic-checkpoint");
    REQUIRE(script.acknowledgments.size() == 3);
    CHECK(script.acknowledgments[0] == std::vector<std::uint64_t>{0, 1});
    CHECK(script.acknowledgments[1] == std::vector<std::uint64_t>{0});
    CHECK(script.acknowledgments[2] == std::vector<std::uint64_t>{0, 1});
    CHECK(script.completions == 2);
    CHECK(count(events, GenerationEvent::Kind::round_finished) == 1);
    CHECK(count(events, GenerationEvent::Kind::continuation_started) == 1);
    CHECK(count(events, GenerationEvent::Kind::continuation_failed) == 1);
    CHECK(count(events, GenerationEvent::Kind::continuation_retry_started) == 1);
    auto failure = std::find_if(events.begin(), events.end(), [](const auto &e) {
        return e.kind == GenerationEvent::Kind::continuation_failed;
    });
    REQUIRE(failure != events.end());
    CHECK(failure->data == json::object{{"error", "permission_denied"}});
    CHECK(script.sent_at[2] - script.sent_at[1] >= 1900ms);
    unsigned finishes = 0;
    std::string text;
    for (const auto &event : events) {
        CHECK(json::serialize(event.data).find("private-synthetic-checkpoint") ==
              std::string::npos);
        if (event.kind != GenerationEvent::Kind::chunk)
            continue;
        if (auto candidates = event.data.if_contains("candidates"))
            for (const auto &value : candidates->as_array()) {
                const auto &candidate = value.as_object();
                finishes += candidate.contains("finishReason");
                if (auto content = candidate.if_contains("content"))
                    for (const auto &part : content->at("parts").as_array())
                        text += string_or(part.as_object(), "text");
            }
    }
    CHECK(text == "hello world");
    CHECK(finishes == 1);
}

TEST_CASE("Browser relay preserves UTF-8 and acknowledges arbitrary byte splits",
          "[browser][continuation][continuation-retry]") {
    const auto bytes = gf::stream(gf::reply("雪 and \"quoted\"", 3, 1));
    for (std::size_t split = 1; split < bytes.size(); ++split) {
        INFO("split=" << split);
        gf::Script script;
        script.attempts = {{gf::pending(std::string_view(bytes).substr(0, split)),
                            gf::pending(std::string_view(bytes).substr(split), 2), gf::done()}};
        std::vector<GenerationEvent> events;
        REQUIRE(run(script, request(), events).empty());
        CHECK(script.acknowledgments.front() == std::vector<std::uint64_t>{0, 1, 2});
        std::string text;
        for (const auto &event : events)
            if (auto candidates = event.data.if_contains("candidates"))
                for (const auto &candidate : candidates->as_array())
                    if (auto content = candidate.as_object().if_contains("content"))
                        for (const auto &part : content->at("parts").as_array())
                            text += string_or(part.as_object(), "text");
        CHECK(text == "雪 and \"quoted\"");
        CHECK(script.bodies.size() == 1);
    }
}

TEST_CASE("The continuation retry budget belongs to the whole generation",
          "[browser][continuation][continuation-retry]") {
    gf::Script script;
    auto r = request();
    SECTION("a second denial ends the answer") {
        script.attempts = {gf::first(), {gf::denied()}, {gf::denied()}};
    }
    SECTION("a later checkpoint cannot allocate another retry") {
        r.output_budget = 100000;
        script.attempts = {gf::first(),
                           {gf::denied()},
                           gf::response(gf::reply("world", 32768, 2, "second-checkpoint", 77744)),
                           {gf::denied()}};
    }
    std::vector<GenerationEvent> events;
    CHECK(run(script, r, events) == "permission_denied");
    CHECK(script.bodies.size() == script.attempts.size());
    CHECK(count(events, GenerationEvent::Kind::continuation_retry_started) == 1);
}

TEST_CASE("Only an unambiguous empty ordinary continuation HTTP 403 can retry",
          "[browser][continuation][continuation-retry]") {
    for (int status : {401, 403, 429, 500, 503})
        for (const auto &tier : {"unknown", "invalid", "pro"}) {
            INFO("status=" << status << " tier=" << tier);
            if (status == 403 && std::string_view(tier) == "pro")
                continue;
            gf::Script script;
            script.attempts = {gf::first(), {gf::denied(status, tier)}};
            std::vector<GenerationEvent> events;
            CHECK_FALSE(run(script, request(), events).empty());
            CHECK(script.bodies.size() == 2);
            CHECK(count(events, GenerationEvent::Kind::continuation_retry_started) == 0);
        }
    SECTION("ordinary first-round denial") {
        gf::Script script;
        script.attempts = {{gf::denied()}};
        std::vector<GenerationEvent> events;
        CHECK(run(script, request(), events) == "permission_denied");
        CHECK(script.bodies.size() == 1);
    }
    SECTION("HTTP 200 RPC denial") {
        gf::Script script;
        script.attempts = {gf::first(),
                           {gf::pending(")]}'\n[[7,\"Permission denied\",[]]]"), gf::done()}};
        std::vector<GenerationEvent> events;
        CHECK(run(script, request(), events) == "permission_denied");
        CHECK(script.bodies.size() == 2);
    }
    SECTION("missing pending-state proof") {
        auto denial = gf::denied();
        denial.erase("pending");
        gf::Script script;
        script.attempts = {gf::first(), {denial}};
        std::vector<GenerationEvent> events;
        CHECK(run(script, request(), events) == "permission_denied");
        CHECK(script.bodies.size() == 2);
    }
    SECTION("transport failure without an HTTP response") {
        auto lost = gf::denied(0);
        lost["errorBody"] = "";
        gf::Script script;
        script.attempts = {gf::first(), {lost}};
        std::vector<GenerationEvent> events;
        CHECK(run(script, request(), events) == "upstream_error");
        CHECK(script.bodies.size() == 2);
    }
}

TEST_CASE("Any upstream bytes prevent continuation replay even without visible text",
          "[browser][continuation][continuation-retry]") {
    for (const auto &bytes :
         {std::string("[[[null,"), std::string(" \n"), gf::stream(gf::reply("partial", 1, 0))}) {
        INFO("bytes=" << bytes);
        gf::Script script;
        script.attempts = {gf::first(), {gf::pending(bytes), gf::denied()}};
        std::vector<GenerationEvent> events;
        CHECK(run(script, request(), events) == "permission_denied");
        CHECK(script.bodies.size() == 2);
        CHECK(count(events, GenerationEvent::Kind::continuation_retry_started) == 0);
    }
    SECTION("a pending block cannot be discarded for a terminal denial") {
        auto pending = gf::pending("[[[null,");
        pending["done"] = true;
        pending["status"] = 403;
        pending["error"] = "upstream_http_403";
        gf::Script script;
        script.attempts = {gf::first(), {pending, gf::denied()}};
        std::vector<GenerationEvent> events;
        CHECK(run(script, request(), events) == "permission_denied");
        CHECK(script.acknowledgments.back() == std::vector<std::uint64_t>{0, 1});
        CHECK(script.bodies.size() == 2);
    }
}

TEST_CASE("Hidden thoughts and nontext model actions also prevent continuation replay",
          "[browser][continuation][continuation-retry]") {
    const auto inputs =
        json::parse(
            R"([{"text":"hidden","thought":true},{"functionCall":{"name":"f","args":{}}},{"inlineData":{"mimeType":"image/png","data":"AA=="}}])")
            .as_array();
    for (const auto &part : inputs) {
        auto encoded = encode_contents(
            json::array{json::object{{"role", "model"}, {"parts", json::array{part}}}});
        auto parts = encoded.at(0).at(0).as_array();
        if (part.as_object().contains("functionCall")) {
            auto &call = parts[0].as_array();
            call[3] = call[10];
            call[10] = nullptr;
        }
        auto chunk = gf::reply("", 1, 0, {}, 77744);
        chunk.at(0).at(0).at(0).at(0) = parts;
        REQUIRE(has_content(decode_chunk(chunk)));
        gf::Script script;
        script.attempts = {gf::first(), {gf::pending(gf::stream(chunk)), gf::denied()}};
        std::vector<GenerationEvent> events;
        unsigned visible = 0;
        auto r = request();
        r.include_thoughts = false;
        CHECK(run(script, r, events, {}, {}, [&](GenerationEvent event) {
                  filter_thoughts(event.data);
                  visible += has_content(event.data);
              }) == "permission_denied");
        if (bool_or(part.as_object(), "thought"))
            CHECK(visible == 1);
        CHECK(script.bodies.size() == 2);
        CHECK(count(events, GenerationEvent::Kind::continuation_retry_started) == 0);
    }
}

TEST_CASE("Cancellation during retry waiting does not create a new attempt",
          "[browser][continuation][continuation-retry]") {
    gf::Script script;
    script.attempts = {gf::first(), {gf::denied()}};
    std::stop_source stop;
    std::vector<GenerationEvent> events;
    const auto started = Clock::now();
    CHECK(run(script, request(), events, {}, stop.get_token(), [&](const auto &e) {
              if (e.kind == GenerationEvent::Kind::continuation_failed)
                  stop.request_stop();
          }) == "cancelled");
    CHECK(Clock::now() - started < 1s);
    CHECK(script.bodies.size() == 2);
    CHECK(count(events, GenerationEvent::Kind::continuation_failed) == 1);
    CHECK(count(events, GenerationEvent::Kind::continuation_retry_started) == 0);
}

TEST_CASE("Retry waiting and preparation keep the original generation deadlines",
          "[browser][continuation][continuation-retry]") {
    gf::Script script;
    script.attempts = {gf::first(), {gf::denied()}, gf::last()};
    auto r = request();
    Limits limits;
    SECTION("unary deadline cannot accommodate the wait") { limits.generation_seconds = 1; }
    SECTION("stream idle deadline cannot accommodate the wait") {
        r.stream = true;
        limits.stream_idle_seconds = 1;
    }
    SECTION("long Retry-After cannot extend the unary deadline") {
        limits.generation_seconds = 10;
        script.attempts[1][0].as_object()["retryAfter"] = 30;
    }
    std::vector<GenerationEvent> events;
    CHECK(run(script, r, events, limits) == "permission_denied");
    CHECK(script.bodies.size() == 2);
    CHECK(count(events, GenerationEvent::Kind::continuation_failed) == 0);
}

TEST_CASE("A retry rechecks the unchanged deadline after preparing its upload",
          "[browser][continuation][continuation-retry]") {
    gf::Script script;
    script.attempts = {gf::first(), {gf::denied()}, gf::last()};
    script.before_send = [](std::size_t attempt, const auto &check) {
        if (attempt == 2) {
            std::this_thread::sleep_for(1100ms);
            check();
        }
    };
    auto r = request(true);
    std::string expected = "stream_idle_timeout";
    Limits limits;
    SECTION("stream idle deadline") { limits.stream_idle_seconds = 3; }
    SECTION("unary total deadline") {
        r.stream = false;
        limits.generation_seconds = 3;
        expected = "generation_timeout";
    }
    std::vector<GenerationEvent> events;
    CHECK(run(script, r, events, limits) == expected);
    CHECK(script.starts == 3);
    CHECK(script.bodies.size() == 2);
}

TEST_CASE("Continuation retry honors a longer Retry-After without changing the body",
          "[browser][continuation][continuation-retry]") {
    gf::Script script;
    script.attempts = {gf::first(), {gf::denied()}, gf::last()};
    script.attempts[1][0].as_object()["retryAfter"] = 3;
    std::vector<GenerationEvent> events;
    REQUIRE(run(script, request(), events).empty());
    REQUIRE(script.sent_at.size() == 3);
    CHECK(script.sent_at[2] - script.sent_at[1] >= 2900ms);
    CHECK(script.bodies[1] == script.bodies[2]);
}
