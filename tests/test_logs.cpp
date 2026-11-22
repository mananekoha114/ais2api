// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/logs.hpp"
#include "ais2api/log_sources.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
using namespace ais2api;
namespace {
struct LogDirectory {
    fs::path path = fs::temp_directory_path() / ("ais2api-logs-" + random_hex(8));
    ~LogDirectory() { std::error_code error; fs::remove_all(path, error); }
};
}
TEST_CASE("Journal persists all history and pages by byte cursor", "[logs]") {
    LogDirectory dir;
    {
        logs::Journal journal(dir.path);
        for (int i = 0; i < 510; ++i) journal.append({{"event", "test"}, {"sequence", i}});
    }
    logs::Journal journal(dir.path);
    auto first = journal.read("0", 500);
    REQUIRE(first.at("entries").as_array().size() == 500);
    CHECK(first.at("has_more").as_bool());
    auto next = string_or(first, "next_cursor");
    auto last = journal.read(next, 500);
    CHECK(last.at("entries").as_array().size() == 10);
    CHECK_FALSE(last.at("has_more").as_bool());
    const auto &record = first.at("entries").as_array()[0].as_object().at("record").as_object();
    CHECK(record.contains("timestamp"));
    CHECK(record.contains("id"));
    CHECK(journal.read(string_or(last, "next_cursor")).at("entries").as_array().empty());
    for (auto cursor : {"-1", "x", "1", "999999999", "18446744073709551616", ""}) {
        try { journal.read(cursor); FAIL("invalid cursor accepted"); }
        catch (const Error &error) { CHECK(error.status == 400); }
    }
    CHECK_THROWS_AS(journal.read("0", 0), Error);
    CHECK_THROWS_AS(journal.read("0", 501), Error);
}
TEST_CASE("Journal serializes concurrent append operations", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path);
    std::vector<std::jthread> workers;
    std::atomic<int> failures{0};
    for (int worker = 0; worker < 4; ++worker)
        workers.emplace_back([&, worker] {
            try { for (int i = 0; i < 20; ++i) journal.append({{"worker", worker}, {"sequence", i}}); }
            catch (...) { ++failures; }
        });
    workers.clear();
    CHECK(failures == 0);
    CHECK(journal.read().at("entries").as_array().size() == 80);
}
TEST_CASE("Journal rejects unsafe paths oversized entries and incomplete records", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path);
    CHECK_THROWS_AS(journal.append({{"message", std::string(logs::Journal::max_record_bytes, 'x')}}), Error);
    journal.append({{"event", "complete"}});
    auto first = journal.read();
    {
        std::ofstream out(dir.path / "management.log", std::ios::binary | std::ios::app);
        out << "{\"event\":\"partial";
    }
    auto partial = journal.read();
    CHECK(partial.at("entries").as_array().size() == 1);
    CHECK(string_or(partial, "next_cursor") == string_or(first, "next_cursor"));
    CHECK_FALSE(partial.at("has_more").as_bool());
    REQUIRE_NOTHROW(journal.append({{"event", "later"}}));
    auto healed = journal.read();
    REQUIRE(healed.at("entries").as_array().size() == 2);
    CHECK(string_or(healed.at("entries").as_array()[1].as_object().at("record").as_object(), "event") == "later");
    fs::remove(dir.path / "management.log");
    {
        std::ofstream out(dir.path / "management.log", std::ios::binary | std::ios::trunc);
        out << "{\"event\":\"truncated-only";
    }
    REQUIRE_NOTHROW(journal.append({{"event", "after-zero"}}));
    auto reset = journal.read();
    REQUIRE(reset.at("entries").as_array().size() == 1);
    CHECK(string_or(reset.at("entries").as_array()[0].as_object().at("record").as_object(), "event") == "after-zero");
    fs::remove(dir.path / "management.log");
    fs::create_directory(dir.path / "management.log");
    CHECK_THROWS_AS(journal.read(), Error);
    CHECK_THROWS_AS(journal.append({{"event", "failed"}}), Error);
}
TEST_CASE("Journal allows symlinked ancestors while refusing symlinked data directory", "[logs]") {
    LogDirectory dir;
    fs::create_directories(dir.path / "real" / "data");
    std::error_code error;
    fs::create_directory_symlink(dir.path / "real", dir.path / "alias", error);
    if (error) { SUCCEED("Symlink creation unavailable for this account"); return; }
    CHECK_NOTHROW(logs::Journal(dir.path / "alias" / "data"));
    CHECK_THROWS_AS(logs::Journal(dir.path / "alias"), Error);
}
TEST_CASE("Journal refuses symlink files", "[logs]") {
    LogDirectory dir;
    secure_directory(dir.path);
    atomic_write(dir.path / "target", "");
    std::error_code error;
    fs::create_symlink(dir.path / "target", dir.path / "management.log", error);
    if (error) { SUCCEED("Symlink creation unavailable for this account"); return; }
    CHECK_THROWS_AS(logs::Journal(dir.path), Error);
}
TEST_CASE("Journal bounds page bytes without losing complete records", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path);
    for (int i = 0; i < 10; ++i)
        journal.append({{"sequence", i}, {"message", std::string(60000, 'x')}});
    std::string cursor = "0";
    std::size_t total = 0;
    bool more = true;
    while (more) {
        auto page = journal.read(cursor, 500);
        auto count = page.at("entries").as_array().size();
        REQUIRE(count > 0);
        CHECK(count <= 4);
        for (const auto& entry : page.at("entries").as_array())
            CHECK(entry.as_object().at("record").as_object().at("sequence").as_int64() == static_cast<std::int64_t>(total++));
        cursor = string_or(page, "next_cursor");
        more = bool_or(page, "has_more");
    }
    CHECK(total == 10);
}

TEST_CASE("Journal rotates without breaking cursors or losing a following reader", "[logs]") {
    LogDirectory dir;
    std::vector<std::int64_t> seen;
    std::string cursor = "0";
    auto follow = [&](logs::Journal& journal) {
        for (bool more = true; more;) {
            auto page = journal.read(cursor, 7);
            for (const auto& entry : page.at("entries").as_array()) {
                const auto& record = entry.as_object().at("record").as_object();
                if (string_or(record, "event") == "test") seen.push_back(record.at("sequence").as_int64());
            }
            const auto next = string_or(page, "next_cursor");
            CHECK(std::stoull(next) >= std::stoull(cursor));
            cursor = next;
            more = bool_or(page, "has_more");
        }
    };
    {
        logs::Journal journal(dir.path, 4096);
        for (int i = 0; i < 300; ++i) {
            journal.append({{"event", "test"}, {"sequence", i}, {"message", std::string(40, 'x')}});
            if (i % 25 == 0) follow(journal);
        }
        follow(journal);
    }
    REQUIRE(seen.size() == 300);
    for (std::size_t i = 0; i < seen.size(); ++i) CHECK(seen[i] == static_cast<std::int64_t>(i));
    CHECK(fs::file_size(dir.path / "management.log") <= 4096 + 512);
    CHECK(fs::exists(dir.path / "management.1.log"));
    // A reopened journal keeps the cursor space; a stale cursor resumes at the
    // oldest retained record instead of failing.
    logs::Journal journal(dir.path, 4096);
    journal.append({{"event", "test"}, {"sequence", 300}});
    follow(journal);
    CHECK(seen.back() == 300);
    auto oldest = journal.read("0", 500);
    auto entries = oldest.at("entries").as_array();
    REQUIRE_FALSE(entries.empty());
    CHECK(string_or(entries[0].as_object().at("record").as_object(), "event") == "journal_rotated");
    CHECK(std::stoull(string_or(entries[0].as_object(), "cursor")) > 0);
    std::size_t total = 0;
    for (std::string at = "0";;) {
        auto page = journal.read(at, 500);
        total += page.at("entries").as_array().size();
        at = string_or(page, "next_cursor");
        if (!bool_or(page, "has_more")) break;
    }
    CHECK(total > 0);
    try { journal.read(std::to_string(std::stoull(cursor) + 1)); FAIL("cursor off a line boundary accepted"); }
    catch (const Error& error) { CHECK(error.status == 400); }
}
TEST_CASE("Journal writes a batch with one call and reports oversized records", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path);
    std::vector<json::object> batch;
    batch.push_back({{"event", "first"}});
    batch.push_back({{"message", std::string(logs::Journal::max_record_bytes, 'x')}});
    batch.push_back({{"event", "second"}});
    CHECK_THROWS_AS(journal.append(std::move(batch)), Error);
    auto entries = journal.read().at("entries").as_array();
    REQUIRE(entries.size() == 2);
    CHECK(string_or(entries[1].as_object().at("record").as_object(), "event") == "second");
}
TEST_CASE("Journal trims service.log when it is this process's stderr", "[logs]") {
    LogDirectory dir;
    fs::create_directories(dir.path);
    const auto service = dir.path / "service.log";
    const std::string filler(5000, 's');
    // Opened append-only like the spawned service's inherited output.
#ifdef _WIN32
    HANDLE file = CreateFileW(service.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    auto put = [&](std::string_view data) {
        DWORD written = 0;
        REQUIRE(WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
    };
    put(filler);
    HANDLE saved = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_ERROR_HANDLE, file);
    logs::Journal journal(dir.path, 4096);
    SetStdHandle(STD_ERROR_HANDLE, saved);
#else
    const int file = ::open(service.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    REQUIRE(file >= 0);
    auto put = [&](std::string_view data) { REQUIRE(::write(file, data.data(), data.size()) == static_cast<ssize_t>(data.size())); };
    put(filler);
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(file, STDERR_FILENO);
    logs::Journal journal(dir.path, 4096);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
#endif
    journal.append({{"event", "trim"}});
    CHECK(fs::file_size(dir.path / "service.1.log") == filler.size());
    CHECK(fs::file_size(service) == 0);
    // The append-only writer continues at the new end of the file.
    put("next\n");
    CHECK(fs::file_size(service) == 5);
#ifdef _WIN32
    CloseHandle(file);
#else
    ::close(file);
#endif
    // Not the stderr target: never touched.
    LogDirectory other;
    fs::create_directories(other.path);
    atomic_write(other.path / "service.log", filler);
    logs::Journal unrelated(other.path, 4096);
    unrelated.append({{"event", "trim"}});
    CHECK(fs::file_size(other.path / "service.log") == filler.size());
    CHECK_FALSE(fs::exists(other.path / "service.1.log"));
}
TEST_CASE("Journal concurrent reader survives rapid rotations and log_sources handles truncation", "[logs]") {
    LogDirectory dir;
    logs::Journal journal(dir.path, 512);
    std::atomic<bool> done{false};
    std::atomic<int> read_errors{0};
    std::vector<std::int64_t> seen;
    std::jthread reader([&] {
        std::string cursor = "0";
        while (true) {
            bool more = false;
            try {
                auto page = journal.read(cursor, 7);
                for (const auto& entry : page.at("entries").as_array()) {
                    const auto& record = entry.as_object().at("record").as_object();
                    if (string_or(record, "event") == "race")
                        seen.push_back(record.at("sequence").as_int64());
                }
                cursor = string_or(page, "next_cursor");
                more = bool_or(page, "has_more");
            } catch (...) {
                ++read_errors;
                break;
            }
            if (done.load() && !more) break;
            if (!more) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    for (int i = 0; i < 120; ++i)
        journal.append({{"event", "race"}, {"sequence", i}, {"message", std::string(40, 'x')}});
    done.store(true);
    reader.join();
    CHECK(read_errors.load() == 0);
    REQUIRE(seen.size() == 120);
    for (std::size_t i = 0; i < seen.size(); ++i)
        CHECK(seen[i] == static_cast<std::int64_t>(i));

    atomic_write(dir.path / "service.log", "line-one\nline-two\n", true);
    auto listed = log_sources(dir.path).at("sources").as_array();
    CHECK(listed.size() >= 2);
    auto chunk = read_log_source(dir.path, "service", "0", 100);
    CHECK(string_or(chunk, "next_cursor") == "18");
    atomic_write(dir.path / "service.log", "short\n", true);
    try {
        (void)read_log_source(dir.path, "service", "18", 100);
        FAIL("Truncated offset was accepted");
    } catch (const Error& error) {
        CHECK(error.status == 409);
        CHECK(error.code == "log_truncated");
    }
}
