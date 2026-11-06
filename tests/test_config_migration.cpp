// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;

namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-config-migration-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};

std::string read_text(const fs::path &path) {
    return read_file(path);
}
}

TEST_CASE("Config::save writes recursively indented JSON and round-trips escaped nested values", "[config][save]") {
    Directory directory;
    const auto file = directory.path / "config.json";
    auto config = Config::load(file);
    config.browser_executable = directory.path / "browser";
    config.model_defaults = {
        {"nested", json::object{
            {"message", "quote: \" slash: \\ newline: \n tab: \t"},
            {"items", json::array{1, json::object{{"deep", json::array{"x", "y"}}}, json::array{}}},
            {"empty_array", json::array{}},
            {"empty_object", json::object{}}
        }}
    };
    config.save();

    const auto saved = read_text(file);
    CHECK(saved.ends_with('\n'));
    CHECK(saved.find("\n  \"model_defaults\": {\n    \"nested\": {\n") != std::string::npos);
    CHECK(saved.find("\"items\": [\n        1,\n") != std::string::npos);
    CHECK(saved.find("\"deep\": [\n            \"x\",\n") != std::string::npos);
    CHECK(saved.find("\"empty_array\": []") != std::string::npos);
    CHECK(saved.find("\"empty_object\": {}") != std::string::npos);
    CHECK(saved.find("\"browser_executable\": \"browser\"") != std::string::npos);
    CHECK(json::parse(saved).as_object() == config.to_json(true));

    auto reloaded = Config::load(file);
    CHECK(reloaded.model_defaults == config.model_defaults);
    CHECK(reloaded.browser_executable == config.browser_executable);
}

TEST_CASE("loading a partial configuration fills defaults, preserves values and rewrites portably", "[config][migration]") {
    Directory directory;
    const auto file = directory.path / "config.json";
    atomic_write(file,
        R"({"port":19000,"data_dir":"relative-data","browser_executable":"","limits":{"queue_size":12},"model_defaults":{"nested":{"keep":"value"}}})");

    const auto config = Config::load(file);
    CHECK(config.port == 19000);
    CHECK(config.data_dir == file.parent_path() / "relative-data");
    CHECK(config.limits.queue_size == 12);
    CHECK(config.limits.request_bytes == Config{}.limits.request_bytes);
    CHECK(config.model_defaults.at("nested").as_object().at("keep") == "value");

    const auto migrated = read_text(file);
    CHECK(migrated.ends_with('\n'));
    const auto persisted = json::parse(migrated).as_object();
    CHECK(persisted.at("version") == 1);
    CHECK(persisted.at("ignore_unsupported_parameters") == false);
    CHECK(persisted.at("search_model_drop_functions") == true);
    CHECK(persisted.at("continuation_retry_enabled") == true);
    CHECK(persisted.at("limits").at("max_continuation_retries") == 1);
    CHECK(persisted.at("browser_idle_seconds") == 0);
    CHECK(persisted.at("browser_proxy_relay") == false);
    CHECK(persisted.at("browser_remote_allow_origins") == false);
    CHECK(persisted.at("browser_site_isolation") == true);
    CHECK(persisted.at("port") == 19000);
    CHECK(persisted.at("data_dir") == "relative-data");
    CHECK(persisted.at("browser_executable") == "");
    CHECK(persisted.at("limits").as_object().at("queue_size") == 12);
    CHECK(persisted.at("limits").as_object().contains("request_bytes"));
    CHECK(persisted.at("model_defaults").as_object().at("nested").as_object().at("keep") == "value");

    // Give the file an old timestamp so the second load proves it did not
    // replace an already canonical file.
    const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(3);
    fs::last_write_time(file, old_time);
    const auto preserved_time = fs::last_write_time(file);
    const auto second = Config::load(file);
    CHECK(second.port == config.port);
    CHECK(read_text(file) == migrated);
    // libc++ file times can use __int128, which Catch2 cannot stringify.
    // Compare before assertion decomposition to preserve full time precision.
    CHECK((fs::last_write_time(file) == preserved_time));
}

TEST_CASE("CLI overrides are not persisted and a missing config is not created", "[config][migration]") {
    Directory directory;
    const auto file = directory.path / "config.json";
    const auto config = Config::load(file, {{"port", 19001}, {"api_key", "temporary-cli-key"}});
    CHECK(config.port == 19001);
    CHECK(config.api_key == "temporary-cli-key");
    CHECK_FALSE(fs::exists(file));

    atomic_write(file, R"({"port":19000})");
    const auto with_override = Config::load(file, {{"port", 19001}, {"api_key", "temporary-cli-key"}});
    CHECK(with_override.port == 19001);
    const auto persisted = json::parse(read_text(file)).as_object();
    CHECK(persisted.at("port") == 19000);
    CHECK(persisted.at("api_key") == "");
}

TEST_CASE("invalid and future-version configuration files are rejected without modification", "[config][migration]") {
    Directory directory;
    const auto file = directory.path / "config.json";
    for (const auto &contents : {std::string(R"({"port":"bad"})"),
                                 std::string(R"({"version":2})"),
                                 std::string(R"([1,2,3])")}) {
        atomic_write(file, contents);
        try {
            (void)Config::load(file);
            FAIL("Invalid configuration was accepted");
        } catch (const Error &error) {
            CHECK(error.code == "invalid_config");
        }
        CHECK(read_text(file) == contents);
    }
}
