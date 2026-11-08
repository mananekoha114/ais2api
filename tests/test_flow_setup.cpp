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
TEST_CASE("leftover browser profiles are removed and installs are kept", "[setup]") {
    Directory dir;
    const auto runtime = dir.root / "runtime";
    const std::string hex(24, 'a');
    for (const auto *name : {"camoufox", "fpgen", "downloads", "camoufox-fontconfig", "chromium-146-linux-x64",
                             "camoufox-ABCDEFABCDEFABCDEFABCDEF", "browser-0123", "staging-12345678"})
        fs::create_directories(runtime / name / "kept");
    fs::create_directories(runtime / ("camoufox-" + hex) / "profile");
    fs::create_directories(runtime / ("browser-" + std::string(24, '0')) / "home");
    std::ofstream(runtime / ("camoufox-" + std::string(24, 'b')), std::ios::binary) << "not a profile";
    CHECK(remove_profile_leftovers(dir.root) == 2);
    CHECK_FALSE(fs::exists(runtime / ("camoufox-" + hex)));
    CHECK(fs::is_regular_file(runtime / ("camoufox-" + std::string(24, 'b'))));
    CHECK(entries(runtime) == 9);
    CHECK(remove_profile_leftovers(dir.root / "missing") == 0);
}

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
TEST_CASE("only a dev bundle embeds the pinned fpgen model", "[setup]") {
    Directory d;
    Config config;
    config.data_dir = d.root;
    config.browser_backend = "camoufox";
    const auto pin = fpgen_model_pin();
    const auto model = doctor(config).at("fpgen_model").as_object();
#ifdef AIS2API_DEV_BUNDLE
    CHECK(fpgen_model_embedded(pin));
    REQUIRE_NOTHROW(load_fpgen_model(d.root, pin));
    CHECK(string_or(model, "source") == "embedded");
    CHECK(string_or(model, "state") == "installed");
    CHECK_FALSE(model.contains("directory"));
#else
    CHECK_FALSE(fpgen_model_embedded(pin));
    CHECK_THROWS_AS(load_fpgen_model(d.root, pin), Error);
    CHECK(string_or(model, "source") == "setup");
    CHECK(string_or(model, "state") == "missing");
#endif
    // Another model than the embedded one is setup's to install.
    auto other = pin;
    other.tag = "model-9/2030";
    CHECK_FALSE(fpgen_model_embedded(other));
    other = pin;
    other.file_sha256.begin()->second = std::string(64, 'a');
    CHECK_FALSE(fpgen_model_embedded(other));
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
    REQUIRE_NOTHROW(load_fpgen_model(d.root, pin));

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
namespace {
// The fake installs below are Windows ones on every host.
const std::string windows_program = "camoufox.exe";
std::string application_ini(const std::string &version) {
    return "; This file is not used.\n[App]\nVendor=Camoufox\nVersion=" + version + "\n\n[Gecko]\nMinVersion=158.0.1\n";
}
// The two placeholders setup and the launch rewrite in Camoufox's Linux fonts.conf.
const std::string linux_fonts_conf = "<fontconfig>\n\t<dir prefix=\"cwd\">fonts</dir>\n"
                                     "\t<cachedir prefix=\"xdg\">fontconfig</cachedir>\n</fontconfig>\n";
const std::string linux_groups = R"({"groups":["L","LM","LMW","LW","M","MW","W"],"readBy":{"lin":["L","LM","LMW","LW"],)"
                                 R"("mac":["LM","LMW","M","MW"],"win":["LMW","LW","MW","W"]}})";
// An archive's files as a Camoufox release for `platform` lays them out.
std::vector<std::pair<std::string, std::string>> camoufox_files(const std::string &platform, const std::string &version) {
    std::vector<std::pair<std::string, std::string>> files{{std::string(camoufox_program(platform)), "exe"},
                                                           {"application.ini", application_ini(version)},
                                                           {"properties.json", "[]"},
                                                           {"fonts/TwemojiMozilla.ttf", "font"}};
    if (platform == "windows-x64") return files;
    files.insert(files.end(), {{"fonts/groups.json", linux_groups},
                               {"fontconfig/linux/fonts.conf", linux_fonts_conf},
                               {"fontconfig/windows/fonts.conf", linux_fonts_conf}});
    for (const auto *group : {"L", "LM", "LMW", "LW", "M", "MW", "W"})
        files.emplace_back("fonts/" + std::string(group) + "/" + group + ".ttf", group);
    return files;
}
// An install as setup leaves it, without the archive it came from.
fs::path fake_camoufox(const fs::path &data_dir, const std::string &version) {
    const auto directory = camoufox_root(data_dir) / (version + "-windows-x64");
    fs::create_directories(directory);
    atomic_write(directory / windows_program, "exe");
    atomic_write(directory / "properties.json", "[]");
    atomic_write(directory / "application.ini", application_ini(version));
    atomic_write(directory / ".ais2api-runtime.json",
                 json::serialize(json::object{{"version", 1}, {"browser", "camoufox"}, {"browser_version", version},
                                              {"platform", "windows-x64"}, {"sha256", std::string(64, 'a')}}));
    return directory;
}
} // namespace
TEST_CASE("the runtime manifest pins Camoufox for Windows and Linux", "[setup]") {
    const auto source = json::parse(read_file(fs::path(AIS2API_SOURCE_DIR) / "runtime-manifest.json")).as_object();
    const auto pin = camoufox_pin(source, "windows-x64");
    REQUIRE(pin);
    CHECK(pin->version == "158.0.1-beta.41");
    CHECK(pin->url == "https://github.com/daijro/camoufox/releases/download/v158.0.1-beta.41/"
                      "camoufox-158.0.1-beta.41-win.x86_64.zip");
    CHECK(pin->size == 1320075455);
    CHECK(camoufox_directory("data", *pin) == fs::path("data") / "runtime" / "camoufox" / "158.0.1-beta.41-windows-x64");
    // Termux runs the Linux ARM64 build in its container.
    for (const auto &[platform, archive] : {std::pair<std::string, std::string>{"linux-x64", "lin.x86_64"},
                                            {"linux-arm64", "lin.arm64"}, {"termux-arm64", "lin.arm64"}}) {
        const auto linux_pin = camoufox_pin(source, platform);
        REQUIRE(linux_pin);
        CHECK(linux_pin->version == pin->version);
        CHECK(linux_pin->url == "https://github.com/daijro/camoufox/releases/download/v158.0.1-beta.41/"
                                "camoufox-158.0.1-beta.41-" + archive + ".zip");
    }
    CHECK(camoufox_pin(source, "termux-arm64")->sha256 == camoufox_pin(source, "linux-arm64")->sha256);
    CHECK(camoufox_keeps("termux-arm64", "fonts/LMW/a.ttf"));
    CHECK_FALSE(camoufox_keeps("termux-arm64", "fonts/W/a.ttf"));
    CHECK_FALSE(camoufox_pin(json::object{}, "windows-x64"));

    const auto manifest = [](std::string version, std::string url, std::string digest, std::uint64_t size) {
        return json::object{{"camoufox", json::object{{"version", version},
                                                      {"artifacts", json::object{{"windows-x64", json::object{
                                                          {"url", url}, {"sha256", digest}, {"size", size}}}}}}}};
    };
    const std::string digest(64, 'a'), url = "https://example.com/camoufox.zip";
    CHECK(camoufox_pin(manifest("159.0-beta.1", url, digest, 1), "windows-x64")->version == "159.0-beta.1");
    for (const auto &bad : {manifest("../x", url, digest, 1), manifest("a/b", url, digest, 1), manifest("", url, digest, 1),
                            manifest("1", "http://example.com/camoufox.zip", digest, 1),
                            manifest("1", url, std::string(64, 'A'), 1), manifest("1", url, digest, 0)})
        CHECK_THROWS_AS(camoufox_pin(bad, "windows-x64"), Error);
}
TEST_CASE("archive extraction skips the entries the caller leaves out", "[setup]") {
    Directory d;
    const auto input = d.root / "runtime.tar";
    make_archive(input, {{"keep/a", "a"}, {"fonts/b", "b"}, {"./fonts/c", "c"}, {"fontsmore/d", "d"}});
    std::vector<std::string> asked;
    extract_archive(input, d.root / "runtime", [&](std::string_view name) {
        asked.emplace_back(name);
        return name.substr(0, 6) != "fonts/";
    });
    CHECK(asked == std::vector<std::string>{"keep/a", "fonts/b", "fonts/c", "fontsmore/d"});
    CHECK(read_file(d.root / "runtime/keep/a") == "a");
    CHECK(read_file(d.root / "runtime/fontsmore/d") == "d");
    CHECK_FALSE(fs::exists(d.root / "runtime/fonts"));
    // An entry left out is checked like any other.
    make_archive(d.root / "escape.tar", {{"fonts/../../escape", "no"}});
    CHECK_THROWS_AS(extract_archive(d.root / "escape.tar", d.root / "escape", [](std::string_view) { return false; }),
                    Error);
}
TEST_CASE("Camoufox installs without its bundled fonts and uBlock Origin", "[setup]") {
    const std::string windows = "windows-x64";
    CHECK(camoufox_program(windows) == "camoufox.exe");
    CHECK_FALSE(camoufox_keeps(windows, "fonts"));
    CHECK_FALSE(camoufox_keeps(windows, "fonts/001__Arial.ttf"));
    CHECK(camoufox_keeps(windows, "fonts/TwemojiMozilla.ttf"));
    CHECK_FALSE(camoufox_keeps(windows, "addons/UBO/manifest.json"));
    CHECK(camoufox_keeps(windows, "addons"));
    CHECK(camoufox_keeps(windows, "fontsmore"));
    CHECK(camoufox_keeps(windows, "browser/omni.ja"));
    CHECK(camoufox_keeps(windows, "distribution/policies.json"));
    CHECK(camoufox_keeps(windows, "fontconfig/windows/fonts.conf"));

    Directory d;
    const std::string version = "158.0.1-beta.41";
    std::vector<std::pair<std::string, std::string>> files{
        {windows_program, "exe"},           {"application.ini", application_ini(version)},
        {"properties.json", "[]"},           {"browser/omni.ja", "omni"},
        {"distribution/policies.json", "{}"}, {"fonts/TwemojiMozilla.ttf", "font"},
        {"fonts/001__Arial.ttf", "font"},    {"addons/UBO/manifest.json", "{}"}};
    const auto archive = d.root / "camoufox.zip";
    make_archive(archive, files, true);
    const CamoufoxPin pin{version, "windows-x64", "https://example.com/camoufox.zip", sha256(read_file(archive)),
                          fs::file_size(archive)};
    const auto directory = camoufox_directory(d.root, pin);

    CHECK(camoufox_state(directory, pin) == "missing");
    install_camoufox(archive, directory, pin);
    CHECK(camoufox_state(directory, pin) == "installed");
    CHECK(read_file(directory / "browser/omni.ja") == "omni");
    CHECK(fs::exists(directory / "distribution/policies.json"));
    CHECK(read_file(directory / "fonts/TwemojiMozilla.ttf") == "font");
    CHECK(entries(directory / "fonts") == 1);
    CHECK_FALSE(fs::exists(directory / "addons/UBO"));
    const auto marker = json::parse(read_file(directory / ".ais2api-runtime.json")).as_object();
    CHECK(string_or(marker, "browser") == "camoufox");
    CHECK(string_or(marker, "sha256") == pin.sha256);
    CHECK(entries(directory.parent_path()) == 1);

    // A damaged install, or another build of the version, is replaced.
    fs::remove(directory / "properties.json");
    CHECK(camoufox_state(directory, pin) == "damaged");
    install_camoufox(archive, directory, pin);
    CHECK(camoufox_state(directory, pin) == "installed");
    auto other = pin;
    other.sha256 = std::string(64, 'b');
    CHECK(camoufox_state(directory, other) == "damaged");
    // So is an install made under other extraction rules.
    auto owner = json::parse(read_file(directory / ".ais2api-runtime.json")).as_object();
    owner["kept"] = json::array{};
    atomic_write(directory / ".ais2api-runtime.json", json::serialize(owner));
    CHECK(camoufox_state(directory, pin) == "damaged");
    install_camoufox(archive, directory, pin);
    CHECK(camoufox_state(directory, pin) == "installed");

    // An archive of another version, or without the browser, changes nothing.
    auto wrong_version = files, no_program = files;
    wrong_version[1].second = application_ini("157.0");
    no_program.erase(no_program.begin());
    for (const auto &contents : {wrong_version, no_program}) {
        const auto bad = d.root / ("bad-" + random_hex(4) + ".zip");
        make_archive(bad, contents, true);
        CHECK_THROWS_AS(install_camoufox(bad, directory, pin), Error);
        CHECK(camoufox_state(directory, pin) == "installed");
        CHECK(entries(directory.parent_path()) == 1);
    }

    // A directory setup did not create is left alone.
    const auto foreign = camoufox_root(d.root) / "mine";
    fs::create_directories(foreign);
    atomic_write(foreign / windows_program, "mine");
    CHECK(camoufox_state(foreign, pin) == "foreign");
    CHECK_THROWS_AS(install_camoufox(archive, foreign, pin), Error);
    CHECK(read_file(foreign / windows_program) == "mine");
}
TEST_CASE("Camoufox for Linux keeps the font groups a Linux identity reads", "[setup]") {
    for (const std::string platform : {"linux-x64", "linux-arm64"}) {
        CHECK(camoufox_program(platform) == "camoufox-bin");
        for (const auto *kept : {"fonts/groups.json", "fonts/TwemojiMozilla.ttf", "fonts/L/a.ttf", "fonts/LM/a.ttf",
                                 "fonts/LMW/a.ttf", "fonts/LW/a.ttf", "fonts/L", "fontconfig/linux/fonts.conf",
                                 "camoufox-bin", "browser/omni.ja", "addons"})
            CHECK(camoufox_keeps(platform, kept));
        for (const auto *skipped : {"fonts", "fonts/M/a.ttf", "fonts/MW/a.ttf", "fonts/W/a.ttf", "fonts/Lx/a.ttf",
                                    "fontconfig/windows/fonts.conf", "fontconfig/macos/fonts.conf", "addons/UBO/a"})
            CHECK_FALSE(camoufox_keeps(platform, skipped));
    }

    Directory d;
    const std::string version = "158.0.1-beta.41";
    const auto files = camoufox_files("linux-x64", version);
    const auto archive = d.root / "camoufox.zip";
    make_archive(archive, files, true);
    const CamoufoxPin pin{version, "linux-x64", "https://example.com/camoufox.zip", sha256(read_file(archive)),
                          fs::file_size(archive)};
    const auto directory = camoufox_directory(d.root, pin);
    install_camoufox(archive, directory, pin);
    CHECK(camoufox_state(directory, pin) == "installed");
    CHECK(entries(directory / "fonts") == 6);
    CHECK(read_file(directory / "fonts/LMW/LMW.ttf") == "LMW");
    CHECK_FALSE(fs::exists(directory / "fonts/W"));
    CHECK(entries(directory / "fontconfig") == 1);
    const auto marker = json::parse(read_file(directory / ".ais2api-runtime.json")).as_object();
    CHECK(marker.at("skipped").as_array().size() == 3);
    // The rules are the platform's: a Windows install of it is not this one.
    auto windows = pin;
    windows.platform = "windows-x64";
    CHECK(camoufox_state(directory, windows) != "installed");

    // A release that groups its fonts otherwise is not installed.
    const auto rejected = [&](std::vector<std::pair<std::string, std::string>> contents) {
        const auto bad = d.root / ("bad-" + random_hex(4) + ".zip");
        make_archive(bad, contents, true);
        try {
            install_camoufox(bad, directory, pin);
            FAIL("installed an archive laid out otherwise");
        } catch (const Error &error) {
            CHECK(error.code == "camoufox_layout_changed");
        }
        CHECK(camoufox_state(directory, pin) == "installed");
        CHECK(entries(directory.parent_path()) == 1);
    };
    const auto replaced = [&](const std::string &name, std::string contents) {
        auto changed = files;
        for (auto &file : changed)
            if (file.first == name) file.second = std::move(contents);
        return changed;
    };
    const auto without = [&](const std::string &name) {
        auto changed = files;
        std::erase_if(changed, [&](const auto &file) { return file.first == name; });
        return changed;
    };
    rejected(replaced("fonts/groups.json", R"({"readBy":{"lin":["L","LM","LMW","LW","N"]}})"));
    rejected(replaced("fonts/groups.json", R"({"readBy":{"lin":["L","LM","LMW"]}})"));
    rejected(replaced("fonts/groups.json", "not json"));
    rejected(without("fonts/groups.json"));
    rejected(without("fonts/LW/LW.ttf"));
    rejected(without("fontconfig/linux/fonts.conf"));
    rejected(replaced("fontconfig/linux/fonts.conf", "<fontconfig><dir>fonts</dir></fontconfig>"));
}
TEST_CASE("setup keeps two Camoufox installs and a rollback holds the earlier one", "[setup]") {
    Directory d;
    const auto selection = [&] { return camoufox_selection(d.root); };
    CHECK(selection().current.empty());
    fake_camoufox(d.root, "1.0");
    select_camoufox(d.root, "1.0-windows-x64");
    CHECK(selection().current == "1.0-windows-x64");
    CHECK(selection().previous.empty());
    fake_camoufox(d.root, "2.0");
    select_camoufox(d.root, "2.0-windows-x64");
    fake_camoufox(d.root, "3.0");
    const auto foreign = camoufox_root(d.root) / "mine";
    fs::create_directories(foreign);
    select_camoufox(d.root, "3.0-windows-x64");
    CHECK(selection().current == "3.0-windows-x64");
    CHECK(selection().previous == "2.0-windows-x64");
    CHECK_FALSE(fs::exists(camoufox_root(d.root) / "1.0-windows-x64"));
    CHECK(fs::exists(camoufox_root(d.root) / "2.0-windows-x64"));
    CHECK(fs::exists(foreign));
    // Selecting the current install again keeps the earlier one.
    select_camoufox(d.root, "3.0-windows-x64");
    CHECK(selection().previous == "2.0-windows-x64");

    hold_previous_camoufox(d.root);
    CHECK(selection().current == "2.0-windows-x64");
    CHECK(selection().previous == "3.0-windows-x64");
    CHECK(selection().held);
    select_camoufox(d.root, "3.0-windows-x64");
    CHECK(selection().current == "3.0-windows-x64");
    CHECK(selection().previous == "2.0-windows-x64");
    CHECK_FALSE(selection().held);

    Directory fresh;
    fake_camoufox(fresh.root, "1.0");
    select_camoufox(fresh.root, "1.0-windows-x64");
    CHECK_THROWS_AS(hold_previous_camoufox(fresh.root), Error);
    CHECK_THROWS_AS(select_camoufox(fresh.root, "../x"), Error);
    // current.json names directories under the Camoufox root, nothing else.
    atomic_write(camoufox_root(fresh.root) / "current.json", R"({"current":"../../x","previous":"a/b"})");
    CHECK(camoufox_selection(fresh.root).current.empty());
    CHECK(camoufox_selection(fresh.root).previous.empty());
}
TEST_CASE("the Camoufox backend runs browser_executable only when it names Camoufox", "[setup]") {
    Directory d;
    Config config;
    config.data_dir = d.root;
    config.browser_executable = d.root / "chrome.exe";
    CHECK(camoufox_executable(config).path.empty());
    const auto installed = fake_camoufox(d.root, "1.0");
    select_camoufox(d.root, "1.0-windows-x64");
    CHECK(camoufox_executable(config).path == installed / windows_program);
    CHECK(camoufox_executable(config).source == "setup");
    config.browser_executable = d.root / "build" / "Camoufox.exe";
    CHECK(camoufox_executable(config).path == config.browser_executable);
    CHECK(camoufox_executable(config).source == "browser_executable");
    config.browser_executable = d.root / "build" / "camoufox-bin";
    CHECK(camoufox_executable(config).source == "browser_executable");
    // A damaged install is not run.
    config.browser_executable.clear();
    fs::remove(installed / "application.ini");
    CHECK(camoufox_executable(config).path.empty());
}
TEST_CASE("doctor tells the launcher when Camoufox needs setup", "[setup]") {
    Directory d;
    Config config;
    config.data_dir = d.root;
    config.browser_backend = "camoufox";
    auto result = doctor(config);
    CHECK(string_or(result, "build_mode") == build_mode());
    CHECK_FALSE(bool_or(result, "browser_configured"));
    CHECK_FALSE(bool_or(result, "browser_exists"));
    CHECK(string_or(result.at("camoufox").as_object(), "source").empty());

    fake_camoufox(d.root, "1.0");
    select_camoufox(d.root, "1.0-windows-x64");
    result = doctor(config);
    CHECK(bool_or(result, "browser_configured"));
    // The fpgen model is still missing.
    CHECK_FALSE(bool_or(result, "browser_exists"));
    const auto &camoufox = result.at("camoufox").as_object();
    CHECK(string_or(camoufox, "source") == "setup");
    CHECK(string_or(camoufox, "current") == "1.0-windows-x64");
    // Version 1.0 is not the pinned one where this platform pins Camoufox.
    CHECK(bool_or(camoufox, "setup_needed") == camoufox_pin().has_value());
    CHECK_FALSE(bool_or(camoufox, "held"));
    CHECK_FALSE(camoufox.contains("bundle"));
}
TEST_CASE("the Camoufox bundled beside the program comes before setup's", "[setup]") {
    const auto pin = camoufox_pin();
    if (!pin) return;  // This platform pins no Camoufox yet.
    Directory d;
    const auto program = d.root / "bin";
    Config config;
    config.data_dir = program / "data";
    config.browser_backend = "camoufox";
    const auto bundle = camoufox_bundle_directory(program, *pin);
    CHECK(bundle == program / "camoufox-runtime" / (pin->version + "-" + pin->platform));
    fake_camoufox(config.data_dir, "1.0");
    select_camoufox(config.data_dir, "1.0-windows-x64");
    CHECK(camoufox_executable(config, program).source == "setup");

    // A dev bundle carries the pinned build as setup installs it.
    const auto archive = d.root / "camoufox.zip";
    make_archive(archive, camoufox_files(pin->platform, pin->version), true);
    install_camoufox(archive, bundle, *pin);
    const auto found = camoufox_executable(config, program);
    CHECK(found.source == "bundle");
    CHECK(found.path == bundle / camoufox_program(pin->platform));
    auto result = doctor(config, program);
    CHECK(string_or(result, "program_directory") == path_utf8(program));
    auto camoufox = result.at("camoufox").as_object();
    CHECK(string_or(camoufox, "source") == "bundle");
    CHECK_FALSE(bool_or(camoufox, "setup_needed"));
    CHECK(string_or(camoufox.at("bundle").as_object(), "state") == "installed");
    // It ships with the program: rolling back means the previous package.
    try {
        rollback_runtime(config, program);
        FAIL("a rollback replaced the bundled Camoufox");
    } catch (const Error &error) {
        CHECK(error.code == "bundled_runtime");
    }
    config.browser_executable = d.root / "build" / "camoufox.exe";
    CHECK(camoufox_executable(config, program).source == "browser_executable");
    config.browser_executable.clear();

    // One trimmed under other rules, or damaged, is passed over.
    auto owner = json::parse(read_file(bundle / ".ais2api-runtime.json")).as_object();
    owner["kept"] = json::array{};
    atomic_write(bundle / ".ais2api-runtime.json", json::serialize(owner));
    CHECK(camoufox_executable(config, program).source == "setup");
    camoufox = doctor(config, program).at("camoufox").as_object();
    CHECK(string_or(camoufox, "source") == "setup");
    CHECK(string_or(camoufox.at("bundle").as_object(), "state") == "damaged");
    install_camoufox(archive, bundle, *pin);
    CHECK(camoufox_executable(config, program).source == "bundle");
    fs::remove(bundle / "application.ini");
    CHECK(camoufox_executable(config, program).source == "setup");
}
TEST_CASE("Linux setup accepts exact supported os-release identities", "[setup]") {
    auto debian = linux_runtime_packages("PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\n"
                                         "VERSION_ID=\"13\"\nID=debian\n");
    REQUIRE(debian == linux_runtime_packages("ID=\"ubuntu\"\nVERSION_ID=24.04\n"));
    REQUIRE(debian == linux_runtime_packages("# metadata\r\nID='debian'\r\nVERSION_ID='13'\r\n"));
    for (auto package : {"libasound2t64", "libcups2t64", "libgtk-3-0t64",
                         "libatk1.0-0t64", "libatk-bridge2.0-0t64"})
        REQUIRE(std::find(debian.begin(), debian.end(), package) != debian.end());
    // Camoufox runs on Xvfb with Mesa, and brings its own fonts.
    const auto camoufox = camoufox_linux_packages("ID=debian\nVERSION_ID=13\n");
    REQUIRE(camoufox == camoufox_linux_packages("ID=ubuntu\nVERSION_ID=\"24.04\"\n"));
    for (auto package : {"xvfb", "libgtk-3-0t64", "libasound2t64", "libgl1-mesa-dri"})
        REQUIRE(std::find(camoufox.begin(), camoufox.end(), package) != camoufox.end());
    for (const auto &package : camoufox) REQUIRE_FALSE(package.starts_with("fonts-"));
}
TEST_CASE("missing_packages reads what a dpkg status file records as installed", "[setup]") {
    const std::string status = "Package: xvfb\nStatus: install ok installed\nArchitecture: arm64\n\n"
                               "Package: libegl1\r\nStatus: deinstall ok config-files\r\n\r\n"
                               "Package: fontconfig\nStatus: hold ok installed \nDescription: x\n continued\n\n"
                               "Package: libgtk-3-0t64\nStatus: install ok half-configured\n\n"
                               "Package: libasound2t64\nArchitecture: armhf\nStatus: install ok not-installed\n\n"
                               "Package: libasound2t64\nArchitecture: arm64\nStatus: install ok installed";
    CHECK(missing_packages(status, {"xvfb", "libegl1", "fontconfig", "libgtk-3-0t64", "libasound2t64", "libdbus-1-3"}) ==
          std::vector<std::string>{"libegl1", "libgtk-3-0t64", "libdbus-1-3"});
    CHECK(missing_packages("", {"xvfb"}) == std::vector<std::string>{"xvfb"});
    // Only a stanza's own fields count.
    CHECK(missing_packages("Package: a\nDescription: Status: install ok installed\n", {"a"}) == std::vector<std::string>{"a"});
}
TEST_CASE("Linux setup rejects unsupported distributions and substring lookalikes", "[setup]") {
    for (auto release : {"ID=debian\nVERSION_ID=12\n", "ID=debian\nVERSION_ID=130\n",
                         "ID=ubuntu\nVERSION_ID=24.04.1\n", "ID=fedora\nVERSION_ID=13\n",
                         "ID=mint\nID_LIKE=ubuntu\nVERSION_ID=24.04\n",
                         "NAME=ubuntu 24.04\n", "# ID=ubuntu\nVERSION_ID=24.04\n",
                         "ID=ubuntu-derivative\nVERSION_ID=24.04\n",
                         "ID=ubuntu\nVERSION_ID=\"24.04\n", "ID=debian\n"}) {
        REQUIRE_THROWS_AS(linux_runtime_packages(release), Error);
        REQUIRE_THROWS_AS(camoufox_linux_packages(release), Error);
    }
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
