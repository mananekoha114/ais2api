// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/flow.hpp"
#include "ais2api/runtime.hpp"
#include "fpgen_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <future>
#include <fstream>
#include <thread>
using namespace ais2api;
using namespace std::chrono_literals;
namespace {
struct Directory {
    fs::path root = fs::temp_directory_path() / ("ais2api-setup-" + random_hex(8));
    Directory() { fs::create_directories(root); }
    ~Directory() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};
void make_archive(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &files,
                  bool zip = false) {
    auto *out = archive_write_new();
    if (zip) {
        archive_write_set_format_zip(out);
        archive_write_set_options(out, "zip:compression=store");
    } else
        archive_write_set_format_pax_restricted(out);
#ifdef _WIN32
    REQUIRE(archive_write_open_filename_w(out, path.c_str()) == ARCHIVE_OK);
#else
    REQUIRE(archive_write_open_filename(out, path.c_str()) == ARCHIVE_OK);
#endif
    for (auto &[name, data] : files) {
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_size(entry, data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0755);
        REQUIRE(archive_write_header(out, entry) == ARCHIVE_OK);
        REQUIRE(archive_write_data(out, data.data(), data.size()) ==
                static_cast<la_ssize_t>(data.size()));
        archive_entry_free(entry);
    }
    archive_write_close(out);
    archive_write_free(out);
}
std::size_t entries(const fs::path &directory) {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(directory), fs::directory_iterator()));
}
} // namespace
TEST_CASE("the runtime manifest pins the fpgen model", "[setup]") {
    const auto pin = fpgen_model_pin();
    CHECK(pin.tag == "model-2/2026");
    CHECK(pin.url.starts_with("https://github.com/scrapfly/fingerprint-generator/releases/download/"));
    CHECK(pin.file_sha256.size() == 3);
    CHECK(fpgen_model_directory("data", pin) == fs::path("data") / "runtime" / "fpgen" / "model-2-2026");

    const auto model = [](std::string tag, std::string url, std::string digest, std::string file) {
        return json::object{{"fpgen_model", json::object{{"tag", tag}, {"url", url}, {"size", 1}, {"sha256", digest},
                                                          {"file_sha256", json::object{{file, digest}}}}}};
    };
    const std::string digest(64, 'a'), url = "https://example.com/model.zip";
    CHECK(fpgen_model_pin(model("model-1/2026", url, digest, "values.dat.zst")).file_sha256.size() == 1);
    for (const auto &manifest :
         {model("../x", url, digest, "a"), model(".x", url, digest, "a"), model("a\\b", url, digest, "a"),
          model("", url, digest, "a"), model("a", "http://example.com/model.zip", digest, "a"),
          model("a", url, std::string(64, 'A'), "a"), model("a", url, digest, "a/b"), json::object{}})
        CHECK_THROWS_AS(fpgen_model_pin(manifest), Error);
}
TEST_CASE("setup installs the pinned fpgen model and leaves other directories alone", "[setup]") {
    Directory d;
    // A model fpgen loads, archived as a release is.
    fpgen_fixture::Fixture fixture;
    const auto v = fixture.add("1");
    fixture.load({fpgen_fixture::node("V", {}, fpgen_fixture::codes({v}), fpgen_fixture::table({{v, 1}}))});
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto &[name, digest] : fixture.sha256s)
        files.emplace_back(name, read_file(fixture.directory.path / name));
    const auto archive = d.root / "model.zip";
    make_archive(archive, files, true);
    const FpgenModelPin pin{"model-9/2030", "https://example.com/model.zip", sha256(read_file(archive)),
                            fs::file_size(archive), fixture.sha256s};
    const auto directory = fpgen_model_directory(d.root, pin);

    CHECK(fpgen_model_state(directory, pin) == "missing");
    install_fpgen_model(archive, directory, pin);
    CHECK(fpgen_model_state(directory, pin) == "installed");
    CHECK(entries(directory) == 4);  // the three files and the marker
    CHECK(entries(directory.parent_path()) == 1);
    REQUIRE_NOTHROW(fpgen::Model::load(directory, pin.file_sha256));

    // A damaged model, or another release under the same tag, is replaced.
    std::ofstream(directory / "values.dat.zst", std::ios::binary) << "x";
    CHECK(fpgen_model_state(directory, pin) == "damaged");
    install_fpgen_model(archive, directory, pin);
    CHECK(fpgen_model_state(directory, pin) == "installed");
    auto other = pin;
    other.sha256 = std::string(64, 'a');
    CHECK(fpgen_model_state(directory, other) == "damaged");

    // An archive without exactly the pinned files changes nothing.
    auto altered = files, extra = files, missing = files;
    altered[0].second += "x";
    extra.emplace_back("README", "x");
    missing.pop_back();
    for (const auto &contents : {altered, extra, missing}) {
        const auto bad = d.root / ("bad-" + random_hex(4) + ".zip");
        make_archive(bad, contents, true);
        CHECK_THROWS_AS(install_fpgen_model(bad, directory, pin), Error);
        CHECK(fpgen_model_state(directory, pin) == "installed");
        CHECK(entries(directory.parent_path()) == 1);
    }

    // A directory setup did not create is left alone.
    const auto foreign = d.root / "foreign";
    fs::create_directories(foreign);
    std::ofstream(foreign / "values.dat.zst", std::ios::binary) << "mine";
    CHECK(fpgen_model_state(foreign, pin) == "foreign");
    CHECK_THROWS_AS(install_fpgen_model(archive, foreign, pin), Error);
    CHECK(entries(foreign) == 1);
}
TEST_CASE("Linux setup accepts exact supported os-release identities", "[setup]") {
    auto debian = linux_runtime_packages("PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\n"
                                         "VERSION_ID=\"13\"\nID=debian\n");
    REQUIRE(debian == linux_runtime_packages("ID=\"ubuntu\"\nVERSION_ID=24.04\n"));
    REQUIRE(debian == linux_runtime_packages("# metadata\r\nID='debian'\r\nVERSION_ID='13'\r\n"));
    for (auto package : {"libasound2t64", "libcups2t64", "libgtk-3-0t64",
                         "libatk1.0-0t64", "libatk-bridge2.0-0t64"})
        REQUIRE(std::find(debian.begin(), debian.end(), package) != debian.end());
}
TEST_CASE("Linux setup rejects unsupported distributions and substring lookalikes", "[setup]") {
    for (auto release : {"ID=debian\nVERSION_ID=12\n", "ID=debian\nVERSION_ID=130\n",
                         "ID=ubuntu\nVERSION_ID=24.04.1\n", "ID=fedora\nVERSION_ID=13\n",
                         "ID=mint\nID_LIKE=ubuntu\nVERSION_ID=24.04\n",
                         "NAME=ubuntu 24.04\n", "# ID=ubuntu\nVERSION_ID=24.04\n",
                         "ID=ubuntu-derivative\nVERSION_ID=24.04\n",
                         "ID=ubuntu\nVERSION_ID=\"24.04\n", "ID=debian\n"})
        REQUIRE_THROWS_AS(linux_runtime_packages(release), Error);
}
TEST_CASE("relay producer cannot advance before downstream acknowledgement", "[flow]") {
    EventPipe pipe(4096);
    std::atomic<bool> finished = false;
    auto producer = std::async(std::launch::async, [&] {
        pipe.push({{"text", "bounded"}}, Clock::now() + 2s);
        finished = true;
        pipe.finish();
    });
    std::optional<json::object> event;
    for (unsigned i = 0; i < 100 && !event; ++i) {
        event = pipe.take();
        if (!event)
            std::this_thread::sleep_for(2ms);
    }
    REQUIRE(event);
    REQUIRE_FALSE(finished);
    REQUIRE_FALSE(pipe.take());
    pipe.acknowledge();
    REQUIRE(producer.wait_for(1s) == std::future_status::ready);
    producer.get();
    REQUIRE(pipe.done());
}
TEST_CASE("relay cancellation releases a blocked producer", "[flow]") {
    EventPipe pipe(4096);
    auto producer = std::async(std::launch::async, [&] {
        try {
            pipe.push({{"text", "pending"}}, Clock::now() + 5s);
        } catch (const Error &e) {
            return e.status;
        }
        return 0;
    });
    std::this_thread::sleep_for(20ms);
    pipe.cancel();
    REQUIRE(producer.wait_for(1s) == std::future_status::ready);
    REQUIRE(producer.get() == 499);
}
TEST_CASE("relay object limit and consumer deadline are enforced", "[flow]") {
    EventPipe small(8);
    REQUIRE_THROWS_AS(small.push({{"text", "oversized"}}, Clock::now() + 1s), Error);
    EventPipe pipe(1024);
    REQUIRE_THROWS_AS(pipe.push({{"text", "unconsumed"}}, Clock::now() + 30ms), Error);
}
TEST_CASE("archive extraction rejects escapes and preserves unrelated files", "[setup]") {
    Directory d;
    auto archive = d.root / "escape.tar";
    make_archive(archive, {{"../escape", "no"}});
    REQUIRE_THROWS_AS(extract_archive(archive, d.root / "stage"), Error);
    REQUIRE_FALSE(fs::exists(d.root / "escape"));
    auto existing = d.root / "occupied";
    fs::create_directories(existing);
    atomic_write(existing / "user.txt", "keep");
    REQUIRE_THROWS_AS(extract_archive(archive, existing), Error);
    REQUIRE(read_file(existing / "user.txt") == "keep");
}
TEST_CASE("archive valid files and duplicate paths have deterministic behavior", "[setup]") {
    Directory d;
    auto input = d.root / "runtime.tar";
    make_archive(input, {{"bin/chrome", "executable"}, {"resources/data", "payload"}});
    extract_archive(input, d.root / "runtime");
    REQUIRE(read_file(d.root / "runtime/bin/chrome") == "executable");
    make_archive(d.root / "duplicate.tar", {{"same", "one"}, {"same", "two"}});
    REQUIRE_THROWS_AS(extract_archive(d.root / "duplicate.tar", d.root / "duplicate"), Error);
}
TEST_CASE("artifact and media validation do not downgrade HTTPS", "[setup]") {
    Directory d;
    REQUIRE_THROWS_AS(download_verified("http://example.org/a", d.root / "x", std::string(64, '0')),
                      Error);
    REQUIRE_THROWS_AS(fetch_public_media("https://127.0.0.1/private", 1024), Error);
    REQUIRE_THROWS_AS(fetch_public_media("file:///etc/passwd", 1024), Error);
}
#ifdef _WIN32
TEST_CASE("Windows archive names cannot overwrite through aliases or device paths", "[setup]") {
    Directory d;
    for (auto name : {"NUL.txt", "COM1/data", "file.", "file "}) {
        auto input = d.root / ("unsafe-" + random_hex(4) + ".tar");
        make_archive(input, {{name, "payload"}});
        REQUIRE_THROWS_AS(extract_archive(input, d.root / ("stage-" + random_hex(4))), Error);
    }
    auto input = d.root / "case.tar";
    make_archive(input, {{"File", "first"}, {"file", "second"}});
    REQUIRE_THROWS_AS(extract_archive(input, d.root / "case"), Error);
    REQUIRE(read_file(d.root / "case/File") == "first");
}
#endif
