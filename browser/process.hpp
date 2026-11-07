// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/common.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/types.h>
#endif
namespace ais2api::browser_detail {
// Each launch writes its own <prefix>*.log; keep the most recent ones only.
void prune_browser_logs(const fs::path& directory, std::string_view prefix = "browser-", std::size_t keep = 8);
void pause(std::stop_token token, std::chrono::milliseconds duration);
// A browser process and every descendant: a Job Object on Windows, a process
// group on POSIX. The child gets a private home, cache, settings and temporary
// directory below `private_root`; the service environment is never changed.
struct ProcessOptions {
    // Set after the private directories, replacing inherited values.
    std::vector<std::pair<std::string, std::string>> environment;
    // Inherited variables whose names start with one of these are dropped.
    std::vector<std::string> unset_prefixes;
    // Receives each stdout/stderr line (without the line break) on a reader
    // thread, never after stop() returns. Without it the output is discarded.
    std::function<void(std::string_view)> on_output;
};
class OwnedProcess {
public:
    OwnedProcess() = default;
    OwnedProcess(const OwnedProcess&) = delete;
    OwnedProcess& operator=(const OwnedProcess&) = delete;
    ~OwnedProcess() { stop(); }
    void start(const fs::path& executable, const std::vector<std::string>& args, const fs::path& private_root,
               const ProcessOptions& options = {});
    bool alive();
    void stop() noexcept;
private:
    void read_output(std::function<void(std::string_view)> on_output);
    void stop_reader() noexcept;
    std::thread reader_;
    std::atomic<bool> stopping_{false};
#ifdef _WIN32
    void* process_ = nullptr;
    void* job_ = nullptr;
    void* output_ = nullptr;
#else
    pid_t pid_ = -1;
    int temporary_directory_fd_ = -1;
    int output_ = -1;
#endif
};
} // namespace ais2api::browser_detail
