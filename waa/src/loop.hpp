// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <utility>
#include <vector>

namespace ais2api::waa {
// The virtual machine's event loop: one thread, which owns the JSContext,
// runs posted tasks and due timers one at a time in the order they became
// ready. After AIStudio2API's internal/waa/loop.go (MIT).
class EventLoop {
  public:
    using Job = std::function<void()>;
    using Time = std::chrono::steady_clock::time_point;

    EventLoop();
    // Closes the loop and joins its thread.
    ~EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    // Queues a task; false once the loop is closed.
    bool post(Job job);
    // Queues a task when `delay` has passed.
    void schedule(std::chrono::nanoseconds delay, Job job);
    // Stops taking tasks and drops the queued ones and the timers. The task
    // that is running finishes; the thread then runs `cleanup` and ends.
    void close();
    // Requested when the loop closes.
    std::stop_token closed() const { return closed_.get_token(); }
    // Whether the thread ended within `timeout`.
    bool wait_exit(std::chrono::milliseconds timeout);
    // Closes the loop and waits for its thread, however long the running
    // task takes. Not from the loop's own thread.
    void join();

    // Set from the loop's own thread, by its first task: `after_task` runs
    // after every task, `cleanup` once when the loop ends. What a task or
    // one of these throws is dropped: tasks report their own failures.
    Job after_task, cleanup;

  private:
    struct Entry {
        Time ready;
        std::uint64_t sequence;
        Job job;
        bool operator>(const Entry &other) const {
            return ready != other.ready ? ready > other.ready : sequence > other.sequence;
        }
    };
    void run();
    static void entry(void *self);

    std::mutex mutex_;
    std::condition_variable wake_, exited_cv_;
    std::deque<Entry> queue_;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> timers_;
    std::uint64_t sequence_ = 0;
    bool closing_ = false, exited_ = false, joined_ = false;
    std::stop_source closed_;
    void *thread_ = nullptr;
};
} // namespace ais2api::waa
