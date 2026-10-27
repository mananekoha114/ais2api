// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/management.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
using namespace ais2api;
namespace {
struct AdminDirectory {
    fs::path path = fs::temp_directory_path() / ("ais2api-admin-config-" + random_hex(8));
    AdminDirectory() { fs::create_directories(path); }
    ~AdminDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};
struct CompatibilityEnvironment {
    static constexpr const char* key = "AIS2API_IGNORE_UNSUPPORTED_PARAMETERS";
    std::optional<std::string> previous;
    CompatibilityEnvironment() {
        if (const auto* value = std::getenv(key)) previous = value;
        set("");
    }
    ~CompatibilityEnvironment() { set(previous ? previous->c_str() : ""); }
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(key, value);
#else
        if (*value) setenv(key, value, 1);
        else unsetenv(key);
#endif
    }
};
}
TEST_CASE("unsupported parameter compatibility config persists and updates without restart", "[config][admin]") {
    CompatibilityEnvironment environment;
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    CHECK_FALSE(Config{}.ignore_unsupported_parameters);
    CHECK_FALSE(cfg.ignore_unsupported_parameters);
    CHECK(cfg.to_json().at("ignore_unsupported_parameters") == false);
    auto enabled = cfg.patched({{"ignore_unsupported_parameters", true}});
    CHECK(enabled.ignore_unsupported_parameters);
    CHECK_FALSE(restart_required(cfg, enabled));
    enabled.save();
    CHECK(Config::load(cfg.config_file).ignore_unsupported_parameters);
    CHECK(json::parse(read_file(cfg.config_file)).as_object().at("ignore_unsupported_parameters") == true);
    auto disabled = enabled.patched({{"ignore_unsupported_parameters", false}});
    CHECK_FALSE(disabled.ignore_unsupported_parameters);
    CHECK_FALSE(restart_required(enabled, disabled));
    for (const auto& invalid : json::array{0, 1, "true", nullptr, json::array{}, json::object{}})
        CHECK_THROWS_AS(cfg.patched({{"ignore_unsupported_parameters", invalid}}), Error);

    environment.set("false");
    CHECK_FALSE(Config::load(cfg.config_file).ignore_unsupported_parameters);
    CHECK(Config::load(cfg.config_file, {{"ignore_unsupported_parameters", true}}).ignore_unsupported_parameters);
    environment.set("true");
    CHECK(Config::load(dir.path / "missing.json").ignore_unsupported_parameters);
    for (const auto* invalid : {"1", "\"true\"", "null", "invalid"}) {
        environment.set(invalid);
        CHECK_THROWS_AS(Config::load(cfg.config_file), Error);
    }
    CHECK(json::parse(read_file(cfg.config_file)).as_object().at("ignore_unsupported_parameters") == true);
}
TEST_CASE("browser DevTools origins and site isolation keep Chromium defaults and need a restart", "[config][admin][browser]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    CHECK_FALSE(cfg.browser_remote_allow_origins);
    CHECK(cfg.browser_site_isolation);
    CHECK(cfg.to_json().at("browser_remote_allow_origins") == false);
    CHECK(cfg.to_json().at("browser_site_isolation") == true);
    auto relaxed = cfg.patched({{"browser_remote_allow_origins", true}});
    CHECK(relaxed.browser_remote_allow_origins);
    CHECK(restart_required(cfg, relaxed));
    auto shared = cfg.patched({{"browser_site_isolation", false}});
    CHECK_FALSE(shared.browser_site_isolation);
    CHECK(restart_required(cfg, shared));
    shared.save();
    CHECK_FALSE(Config::load(cfg.config_file).browser_site_isolation);
    for (const auto& invalid : json::array{0, 1, "true", nullptr})
        for (auto key : {"browser_remote_allow_origins", "browser_site_isolation"})
            CHECK_THROWS_AS(cfg.patched({{key, invalid}}), Error);
}
TEST_CASE("search model function policy defaults to dropping functions and updates without restart", "[config][admin][search-model]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    CHECK(Config{}.search_model_drop_functions);
    CHECK(cfg.search_model_drop_functions);
    CHECK(cfg.to_json().at("search_model_drop_functions") == true);
    auto keep = cfg.patched({{"search_model_drop_functions", false}});
    CHECK_FALSE(keep.search_model_drop_functions);
    CHECK_FALSE(restart_required(cfg, keep));
    keep.save();
    CHECK_FALSE(Config::load(cfg.config_file).search_model_drop_functions);
    CHECK(json::parse(read_file(cfg.config_file)).as_object().at("search_model_drop_functions") == false);
    for (const auto& invalid : json::array{0, "false", nullptr, json::object{}})
        CHECK_THROWS_AS(cfg.patched({{"search_model_drop_functions", invalid}}), Error);
}
TEST_CASE("idle browser release is off by default and updates without restart", "[config][admin][idle-release]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    CHECK(Config{}.browser_idle_seconds == 0);
    CHECK(cfg.browser_idle_seconds == 0);
    CHECK(cfg.to_json().at("browser_idle_seconds") == 0);
    auto enabled = cfg.patched({{"browser_idle_seconds", 900}});
    CHECK(enabled.browser_idle_seconds == 900);
    CHECK_FALSE(restart_required(cfg, enabled));
    enabled.save();
    CHECK(Config::load(cfg.config_file).browser_idle_seconds == 900);
    CHECK(json::parse(read_file(cfg.config_file)).as_object().at("browser_idle_seconds") == 900);
    CHECK(Config::load(cfg.config_file, {{"browser_idle_seconds", 0}}).browser_idle_seconds == 0);
    for (const auto& valid : json::array{0, 10, 86400})
        CHECK_NOTHROW(cfg.patched({{"browser_idle_seconds", valid}}));
    for (const auto& invalid : json::array{1, 9, 86401, -1, 4294967296LL, true, "900", nullptr, 1.5})
        CHECK_THROWS_AS(cfg.patched({{"browser_idle_seconds", invalid}}), Error);
    Config direct = cfg;
    direct.browser_idle_seconds = 5;
    CHECK_THROWS_AS(direct.validate(), Error);
}
TEST_CASE("management config authentication and origins are independent", "[config][admin]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    CHECK_FALSE(cfg.authenticate_admin(""));
    cfg = cfg.patched({{"api_key", "api-test"}, {"admin_key", "admin-test"},
                       {"admin_origins", json::array{"http://localhost:8080", "https://example.com", "http://[::1]:3000"}}});
    CHECK(cfg.authenticate_admin("admin-test"));
    CHECK_FALSE(cfg.authenticate_admin("api-test"));
    CHECK_FALSE(cfg.authenticate("admin-test"));
    CHECK(cfg.to_json().at("admin_origins") == cfg.admin_origins);
    Store store(dir.path / "data");
    auto shown = manage(store, cfg, {{"command", "config.show"}});
    CHECK(shown.at("config").as_object().at("admin_key") == "***");
    for (const auto& value : json::array{7, true, json::object{}, "https://example.com/", "https://user@example.com", "https://*.example.com", "https://example.com?x", "https://example.com#x", "ftp://example.com", "https://example.com:99999", "https://example.com:", "https://bad..example"})
        CHECK_THROWS_AS(cfg.patched({{"admin_origins", json::array{value}}}), Error);
    CHECK_THROWS_AS(cfg.patched({{"admin_origins", "https://example.com"}}), Error);
    CHECK_THROWS_AS(cfg.patched({{"admin_key", "with space"}}), Error);
    CHECK_THROWS_AS(cfg.patched({{"admin_key", 7}}), Error);
}
TEST_CASE("management config patches validate without file changes", "[config][admin]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    cfg.model_aliases = {{"old", "model"}};
    cfg.model_defaults = {{"old", 1}};
    auto patched = cfg.patched({{"limits", json::object{{"queue_size", 12}}},
        {"model_aliases", json::object{{"new", "replacement"}}}, {"model_defaults", json::object{}}});
    CHECK(patched.limits.queue_size == 12);
    CHECK(patched.limits.request_bytes == cfg.limits.request_bytes);
    CHECK_FALSE(patched.model_aliases.contains("old"));
    CHECK(patched.model_defaults.empty());
    CHECK_FALSE(fs::exists(cfg.config_file));
    CHECK_FALSE(restart_required(cfg, patched));
    CHECK(restart_required(cfg, cfg.patched({{"port", 18000}})));
    for (const auto& patch : std::vector<json::object>{
        {{"unknown", 1}}, {{"limits", json::object{{"unknown", 1}}}},
        {{"limits", json::object{{"queue_size", "12"}}}}, {{"limits", json::object{{"queue_size", 0}}}},
        {{"limits", false}}, {{"headless", 1}}, {{"model_aliases", json::object{{"x", 1}}}},
        {{"api_key", "***"}}, {{"cookie_import_key", "***"}}, {{"admin_key", "***"}}})
        CHECK_THROWS_AS(cfg.patched(patch), Error);
}
TEST_CASE("account metadata updates validate input and unknown ids", "[management][admin]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    Store store(dir.path / "data");
    CHECK_THROWS_AS(manage(store, cfg, {{"command", "accounts.update"}, {"id", "missing"}, {"name", "updated"}}), Error);
    Account account;
    account.id = "test-account";
    account.name = "before";
    account.storage_state = {{"cookies", json::array{json::object{{"name", "SID"},
        {"value", "synthetic"}, {"domain", ".google.com"}, {"path", "/"}, {"secure", true}}}},
        {"origins", json::array{}}};
    store.put_account(account);
    auto result = manage(store, cfg, {{"command", "accounts.update"}, {"id", account.id}, {"name", "after"}, {"disabled", true}});
    CHECK(result.at("ok") == true);
    CHECK_FALSE(result.contains("storage_state"));
    CHECK(store.account(account.id).name == "after");
    CHECK(store.account(account.id).disabled);
    for (const auto& patch : std::vector<json::object>{ {{"name", 7}}, {{"disabled", "false"}}, {{"storage_state", json::object{}}} }) {
        auto command = patch; command["command"] = "accounts.update"; command["id"] = account.id;
        CHECK_THROWS_AS(manage(store, cfg, command), Error);
    }
}
TEST_CASE("new configurations start with the default management password", "[config][admin]") {
    AdminDirectory dir;
    auto fresh = Config::load(dir.path / "config.json");
    CHECK(fresh.admin_key == default_admin_key);
    CHECK(fresh.admin_key_is_default());
    CHECK(fresh.authenticate_admin("admin"));
    fresh.save();
    CHECK(Config::load(fresh.config_file).admin_key_is_default());
    // Existing files keep their value: an empty key still disables management.
    auto existing = fresh;
    existing.admin_key.clear();
    existing.save();
    const auto before = read_file(existing.config_file);
    auto reloaded = Config::load(existing.config_file);
    CHECK(reloaded.admin_key.empty());
    CHECK_FALSE(reloaded.authenticate_admin("admin"));
    CHECK(read_file(existing.config_file) == before);
    // The default can never be configured again through a patch.
    CHECK_THROWS_AS(reloaded.patched({{"admin_key", "admin"}}), Error);
    CHECK(reloaded.patched({{"admin_key", "replacement-admin"}}).admin_key == "replacement-admin");
}
TEST_CASE("management password replacement policy", "[config][admin]") {
    CHECK_NOTHROW(validate_admin_password("strong-password-1", "admin"));
    for (const auto* rejected : {"", "short", "1234567", "admin", "with space1", "tab\tpassword"})
        CHECK_THROWS_AS(validate_admin_password(rejected, "admin"), Error);
    CHECK_THROWS_AS(validate_admin_password("same-password", "same-password"), Error);
    CHECK_THROWS_AS(validate_admin_password(std::string(4097, 'x'), "admin"), Error);
    CHECK_NOTHROW(validate_admin_password(std::string(4096, 'x'), "admin"));
    try {
        validate_admin_password("short", "admin");
    } catch (const Error& error) {
        CHECK(error.status == 400);
        CHECK(error.code == "admin_password_invalid");
    }
}
TEST_CASE("default management password peers are limited to local networks", "[config][admin]") {
    for (const auto* local : {"127.0.0.1", "127.8.9.10", "::1", "10.1.2.3", "172.16.0.1", "172.31.255.254",
                              "192.168.1.20", "169.254.3.4", "fe80::1", "fd12:3456::1", "fc00::1",
                              "::ffff:192.168.0.2", "::ffff:127.0.0.1"})
        CHECK(local_network_address(local));
    for (const auto* remote : {"8.8.8.8", "172.15.255.255", "172.32.0.1", "100.64.0.1", "192.169.0.1",
                               "11.0.0.1", "2001:4860::8888", "::ffff:8.8.8.8", "::", "0.0.0.0",
                               "not-an-address", ""})
        CHECK_FALSE(local_network_address(remote));
}
TEST_CASE("build_app upstream is opt-in, needs a Build App link and site isolation, and needs a restart", "[config][admin][build-app]") {
    AdminDirectory dir;
    auto cfg = Config::load(dir.path / "config.json");
    CHECK(Config{}.upstream == "makersuite");
    CHECK(cfg.to_json().at("upstream") == "makersuite");
    CHECK(cfg.to_json().at("build_app_url") == "");
    constexpr std::string_view link = "https://ai.studio/apps/0f1e2d3c-4b5a-6978-8a9b-0c1d2e3f4a5b";
    CHECK_THROWS_AS(cfg.patched({{"upstream", "build_app"}}), Error);
    auto app = cfg.patched({{"upstream", "build_app"}, {"build_app_url", link}});
    CHECK(app.upstream == "build_app");
    CHECK(restart_required(cfg, app));
    CHECK(restart_required(app, app.patched({{"build_app_url", "https://aistudio.google.com/apps/drive/abc_DEF-1"}})));
    app.save();
    const auto saved = Config::load(cfg.config_file);
    CHECK(saved.upstream == "build_app");
    CHECK(saved.build_app_url == link);
    CHECK_THROWS_AS(app.patched({{"browser_site_isolation", false}}), Error);
    // A link may be kept while the default upstream is in use.
    CHECK(cfg.patched({{"build_app_url", link}}).upstream == "makersuite");
    for (const auto* invalid : {"http://ai.studio/apps/abc", "https://ai.studio/apps/", "https://ai.studio/apps/abc/def",
                                "https://ai.studio/apps/abc?authuser=1", "https://ai.studio/apps/abc#x", "https://ai.studio/apps/a%2Fb",
                                "https://aistudio.google.com/apps/a/b/c/d", "https://aistudio.google.com/prompts/new_chat",
                                "https://evil.example/apps/abc", "https://ai.studio.evil.example/apps/abc", "https://ai.studio/apps/abc/"})
        CHECK_THROWS_AS(cfg.patched({{"build_app_url", invalid}}), Error);
    for (const auto& invalid : json::array{"", "Build_App", "auto", nullptr, 1})
        CHECK_THROWS_AS(cfg.patched({{"upstream", invalid}, {"build_app_url", link}}), Error);
}
