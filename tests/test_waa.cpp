// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The WAA event loop, which needs no JavaScript engine.
#include "../waa/src/loop.hpp"
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <mutex>
#include <string>
#include <thread>

using namespace ais2api;
using namespace std::chrono_literals;
namespace {
// The order in which a loop ran its tasks.
struct Trace {
    std::mutex mutex;
    std::string order;
    void add(char task) {
        std::lock_guard lock(mutex);
        order += task;
    }
    std::string read() {
        std::lock_guard lock(mutex);
        return order;
    }
};
} // namespace

TEST_CASE("The event loop runs tasks and timers in the order they became ready", "[waa]") {
    Trace trace;
    waa::EventLoop loop;
    std::promise<void> finished;
    REQUIRE(loop.post([&] {
        // Both timers fall due while this task runs; the second task is
        // queued after them.
        loop.schedule(20ms, [&] { trace.add('c'); });
        loop.schedule(5ms, [&] { trace.add('b'); });
        loop.schedule(0ms, [&] { trace.add('a'); });
        std::this_thread::sleep_for(60ms);
        loop.post([&] { trace.add('d'); });
        loop.schedule(30ms, [&] {
            trace.add('e');
            finished.set_value();
        });
    }));
    REQUIRE(finished.get_future().wait_for(5s) == std::future_status::ready);
    // A task posted with no delay keeps its place in the queue: 'a' was
    // queued when the timers were not yet due.
    CHECK(trace.read() == "abcde");
}

TEST_CASE("The event loop survives a failing task and runs its hooks", "[waa]") {
    Trace trace;
    std::promise<void> finished;
    {
        waa::EventLoop loop;
        loop.post([&] {
            loop.after_task = [&] { trace.add('.'); };
            loop.cleanup = [&] { trace.add('!'); };
            trace.add('a');
        });
        loop.post([&] {
            trace.add('b');
            throw std::runtime_error("dropped");
        });
        loop.post([&] {
            trace.add('c');
            finished.set_value();
        });
        REQUIRE(finished.get_future().wait_for(5s) == std::future_status::ready);
    }
    CHECK(trace.read() == "a.b.c.!");
}

TEST_CASE("A closed event loop drops what was queued and takes no more", "[waa]") {
    Trace trace;
    waa::EventLoop loop;
    std::promise<void> running, release;
    loop.post([&] {
        running.set_value();
        release.get_future().wait();
        trace.add('a');
    });
    loop.post([&] { trace.add('b'); });
    loop.schedule(1ms, [&] { trace.add('c'); });
    running.get_future().wait();
    const auto closed = loop.closed();
    CHECK_FALSE(closed.stop_requested());
    loop.close();
    CHECK(closed.stop_requested());
    CHECK_FALSE(loop.post([&] { trace.add('d'); }));
    // The running task is not interrupted.
    CHECK_FALSE(loop.wait_exit(20ms));
    release.set_value();
    CHECK(loop.wait_exit(5s));
    loop.join();
    CHECK(trace.read() == "a");
}
