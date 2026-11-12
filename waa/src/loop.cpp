// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "loop.hpp"
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#  include <process.h>
#  include <windows.h>
#else
#  include <pthread.h>
#endif

namespace ais2api::waa {
namespace {
// The interpreter recurses deeply; the engine's stack quota is measured from
// the thread's real stack, so the thread gets a fixed, generous one.
constexpr std::size_t thread_stack_bytes = 8 * 1024 * 1024;
} // namespace

#ifdef _WIN32
namespace {
unsigned __stdcall thread_start(void *argument) {
    auto *start = static_cast<std::pair<void (*)(void *), void *> *>(argument);
    const auto [function, self] = *start;
    delete start;
    function(self);
    return 0;
}
} // namespace
EventLoop::EventLoop() {
    auto *start = new std::pair<void (*)(void *), void *>(&EventLoop::entry, this);
    const auto handle = _beginthreadex(nullptr, static_cast<unsigned>(thread_stack_bytes), thread_start, start,
                                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!handle) {
        delete start;
        throw std::runtime_error("Could not start the WAA event loop thread");
    }
    thread_ = reinterpret_cast<void *>(handle);
}
void EventLoop::join() {
    close();
    if (std::exchange(joined_, true)) return;
    WaitForSingleObject(static_cast<HANDLE>(thread_), INFINITE);
    CloseHandle(static_cast<HANDLE>(thread_));
}
#else
namespace {
void *thread_start(void *argument) {
    auto *start = static_cast<std::pair<void (*)(void *), void *> *>(argument);
    const auto [function, self] = *start;
    delete start;
    function(self);
    return nullptr;
}
} // namespace
EventLoop::EventLoop() {
    auto *start = new std::pair<void (*)(void *), void *>(&EventLoop::entry, this);
    auto *thread = new pthread_t;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, thread_stack_bytes);
    const int failed = pthread_create(thread, &attributes, thread_start, start);
    pthread_attr_destroy(&attributes);
    if (failed) {
        delete start;
        delete thread;
        throw std::runtime_error("Could not start the WAA event loop thread");
    }
    thread_ = thread;
}
void EventLoop::join() {
    close();
    if (std::exchange(joined_, true)) return;
    auto *thread = static_cast<pthread_t *>(thread_);
    pthread_join(*thread, nullptr);
    delete thread;
}
#endif

EventLoop::~EventLoop() { join(); }

void EventLoop::entry(void *self) { static_cast<EventLoop *>(self)->run(); }

void EventLoop::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (closing_) break;
                const auto now = std::chrono::steady_clock::now();
                // A due timer runs where it would have been posted: ahead of
                // tasks queued after its deadline.
                if (!timers_.empty() && timers_.top().ready <= now &&
                    (queue_.empty() || timers_.top().ready < queue_.front().ready)) {
                    job = std::move(const_cast<Entry &>(timers_.top()).job);
                    timers_.pop();
                    break;
                }
                if (!queue_.empty()) {
                    job = std::move(queue_.front().job);
                    queue_.pop_front();
                    break;
                }
                if (timers_.empty()) wake_.wait(lock);
                else wake_.wait_until(lock, timers_.top().ready);
            }
            if (closing_) break;
        }
        try {
            job();
        } catch (...) {
        }
        // Release what the task captured before the next one runs.
        job = nullptr;
        try {
            if (after_task) after_task();
        } catch (...) {
        }
    }
    try {
        if (cleanup) cleanup();
    } catch (...) {
    }
    {
        std::lock_guard lock(mutex_);
        exited_ = true;
    }
    exited_cv_.notify_all();
}

bool EventLoop::post(Job job) {
    {
        std::lock_guard lock(mutex_);
        if (closing_) return false;
        queue_.push_back({std::chrono::steady_clock::now(), ++sequence_, std::move(job)});
    }
    wake_.notify_one();
    return true;
}

void EventLoop::schedule(std::chrono::nanoseconds delay, Job job) {
    if (delay <= std::chrono::nanoseconds::zero()) {
        post(std::move(job));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        timers_.push({std::chrono::steady_clock::now() + delay, ++sequence_, std::move(job)});
    }
    wake_.notify_one();
}

void EventLoop::close() {
    {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        closing_ = true;
        // Tasks may hold what only the loop's thread may release; they are
        // dropped here, and everything of that kind is released by `cleanup`.
        queue_.clear();
        while (!timers_.empty()) timers_.pop();
    }
    closed_.request_stop();
    wake_.notify_one();
}

bool EventLoop::wait_exit(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return exited_cv_.wait_for(lock, timeout, [&] { return exited_; });
}
} // namespace ais2api::waa
