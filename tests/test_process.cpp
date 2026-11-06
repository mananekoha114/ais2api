// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../browser/process.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <mutex>

using namespace ais2api;
using namespace ais2api::browser_detail;
using namespace std::chrono_literals;

namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-process-test-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
// The system shell and arguments that run the script for this platform.
std::pair<fs::path, std::vector<std::string>> shell(std::string_view posix, std::string_view powershell) {
#ifdef _WIN32
    (void)posix;
    const auto root = std::getenv("SystemRoot");
    REQUIRE(root != nullptr);
    return {fs::path(root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe",
            {"-NoProfile", "-NonInteractive", "-Command", std::string(powershell)}};
#else
    (void)powershell;
    const fs::path sh = fs::exists("/bin/sh") ? "/bin/sh" : "/system/bin/sh";
    return {sh, {"-c", std::string(posix)}};
#endif
}
struct Lines {
    std::mutex mutex;
    std::vector<std::string> values;
    bool contains(std::string_view line) {
        std::lock_guard lock(mutex);
        return std::find(values.begin(), values.end(), line) != values.end();
    }
};
}

TEST_CASE("owned process passes extra environment and captures both output streams", "[process]") {
#ifdef _WIN32
    _putenv_s("AIS2API_UNSET_PROBE", "inherited");
#else
    setenv("AIS2API_UNSET_PROBE", "inherited", 1);
#endif
    Directory directory;
    Lines lines;
    ProcessOptions options;
    options.environment = {{"AIS2API_PROBE", "probe-value"}};
    options.unset_prefixes = {"AIS2API_UNSET_"};
    options.on_output = [&](std::string_view line) { std::lock_guard lock(lines.mutex); lines.values.emplace_back(line); };
    auto [executable, args] = shell(
        R"(echo "$AIS2API_PROBE"; echo stderr-line >&2; echo "unset:$AIS2API_UNSET_PROBE")",
        R"(Write-Output $env:AIS2API_PROBE; [Console]::Error.WriteLine('stderr-line'); Write-Output ('unset:' + $env:AIS2API_UNSET_PROBE))");
    OwnedProcess process;
    process.start(executable, args, directory.path / "private", options);
    const auto deadline = Clock::now() + 60s;
    while (process.alive() && Clock::now() < deadline) std::this_thread::sleep_for(20ms);
    CHECK_FALSE(process.alive());
    process.stop();
    CHECK(lines.contains("probe-value"));
    CHECK(lines.contains("stderr-line"));
    CHECK(lines.contains("unset:"));
}

TEST_CASE("stopping an owned process ends its output reader at once", "[process]") {
    Directory directory;
    Lines lines;
    ProcessOptions options;
    options.on_output = [&](std::string_view line) { std::lock_guard lock(lines.mutex); lines.values.emplace_back(line); };
    auto [executable, args] = shell("echo started; sleep 60", "Write-Output started; Start-Sleep -Seconds 60");
    OwnedProcess process;
    process.start(executable, args, directory.path / "private", options);
    const auto deadline = Clock::now() + 60s;
    while (!lines.contains("started") && Clock::now() < deadline) std::this_thread::sleep_for(20ms);
    REQUIRE(lines.contains("started"));
    CHECK(process.alive());
    const auto stopping = Clock::now();
    process.stop();
    CHECK(Clock::now() - stopping < 10s);
    CHECK_FALSE(process.alive());
}
