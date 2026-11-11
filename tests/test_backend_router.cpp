// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend_router.hpp"
#include "ais2api/management.hpp"
#include "ais2api/runtime.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

using namespace ais2api;
namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-router-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
std::string import_account(Store &store, const std::string &cookie) {
    return string_or(store.import_accounts(json::value("SAPISID=synthetic-" + cookie))[0].as_object(), "id");
}
// Records every call each named gateway receives.
struct Calls {
    std::vector<std::string> log;
};
class RecordingGateway final : public GenerationBackend {
  public:
    RecordingGateway(std::string name, Calls &calls, bool idle) : name_(std::move(name)), calls_(calls), idle_(idle) {}
    void generate(const Request &, const Account &account, const EventSink &, std::stop_token) override {
        calls_.log.push_back(name_ + ":generate:" + account.id);
    }
    json::array models(const Account &account, std::stop_token) override {
        calls_.log.push_back(name_ + ":models:" + account.id);
        return {};
    }
    json::object probe(const Account &account, std::stop_token) override {
        calls_.log.push_back(name_ + ":probe:" + account.id);
        return {};
    }
    json::object diagnostics() override { return {{"status", name_ + "_status"}}; }
    void reconfigure(const Config &config) override { calls_.log.push_back(name_ + ":reconfigure:" + config.browser_backend); }
    void close() override { calls_.log.push_back(name_ + ":close"); }
    void reset_session() override { calls_.log.push_back(name_ + ":reset_session"); }
    void clear_template() override { calls_.log.push_back(name_ + ":clear_template"); }
    bool release_idle() override {
        calls_.log.push_back(name_ + ":release_idle");
        return idle_;
    }

  private:
    std::string name_;
    Calls &calls_;
    bool idle_;
};
BackendRouter::Factory recording(Calls &calls) {
    return [&calls](std::string_view backend, const Config &, Store &) {
        calls.log.push_back("create:" + std::string(backend));
        return std::make_unique<RecordingGateway>(std::string(backend), calls, backend == "camoufox");
    };
}
Account account(std::string id) {
    Account result;
    result.id = std::move(id);
    return result;
}
} // namespace

TEST_CASE("account backends map onto the gateways that serve them", "[router]") {
    CHECK(adopted_backend("cloakbrowser") == "cloakbrowser");
    CHECK(adopted_backend("camoufox") == "auto");
    CHECK(effective_backend("cloakbrowser", "camoufox") == "cloakbrowser");
    for (auto stored : {"auto", "camoufox", "spidermonkey"}) CHECK(effective_backend(stored, "cloakbrowser") == "camoufox");
    CHECK(effective_backend("", "camoufox") == "camoufox");
    CHECK(effective_backend("unknown", "cloakbrowser") == "cloakbrowser");
    CHECK(identity_family("cloakbrowser") == "chrome");
    CHECK(identity_family("auto") == "firefox");
    CHECK(identity_family("camoufox") == "firefox");
    CHECK(valid_account_backend("spidermonkey"));
    CHECK_FALSE(valid_account_backend(""));
    CHECK_FALSE(valid_account_backend("Camoufox"));
    CHECK(backends_needed("cloakbrowser", {}) == std::vector<std::string>{"cloakbrowser"});
    CHECK(backends_needed("cloakbrowser", {"", "cloakbrowser", "auto"}) == std::vector<std::string>{"cloakbrowser", "camoufox"});
    CHECK(backends_needed("camoufox", {"camoufox", "cloakbrowser"}) == std::vector<std::string>{"camoufox", "cloakbrowser"});
}

TEST_CASE("CloakBrowser is reported as deprecated wherever it is still used", "[router]") {
    const auto entry = [](bool by_default, bool accounts) {
        return json::array{json::object{{"code", "cloakbrowser_backend"}, {"default_backend", by_default}, {"accounts", accounts}}};
    };
    CHECK(backend_deprecations("camoufox", {}).empty());
    CHECK(backend_deprecations("camoufox", {"auto", "camoufox", "spidermonkey", ""}).empty());
    CHECK(backend_deprecations("camoufox", {"auto", "cloakbrowser"}) == entry(false, true));
    CHECK(backend_deprecations("cloakbrowser", {}) == entry(true, false));
    CHECK(backend_deprecations("cloakbrowser", {"auto"}) == entry(true, false));
    // An unsettled account follows the default.
    CHECK(backend_deprecations("cloakbrowser", {""}) == entry(true, true));
}

TEST_CASE("stored accounts settle on the backend they used and keep it", "[router][state]") {
    Directory dir;
    std::string first, second;
    {
        Store store(dir.path);
        first = import_account(store, "first");
        CHECK(store.account_backend(first) == std::string{});
        CHECK(store.stored_backends() == std::vector<std::string>{""});
        CHECK(store.adopt_backends("cloakbrowser") == 1);
        CHECK(store.adopt_backends("auto") == 0);
        second = import_account(store, "second");
        CHECK(store.adopt_backends("auto") == 1);
        CHECK(store.fingerprint(first).backend == "cloakbrowser");
        CHECK(store.fingerprint(second).backend == "auto");
        CHECK_FALSE(store.account_backend("acc_missing"));
        CHECK_THROWS_AS(store.set_backend("acc_missing", "camoufox"), Error);
        store.set_backend(first, "camoufox");
        // Regenerating the fingerprint keeps the backend.
        store.regenerate_fingerprint(first);
        CHECK(string_or(store.account_status()[0].as_object(), "backend") == "camoufox");
    }
    // Readable without a Store, as setup and doctor do while the service runs.
    auto stored = stored_backends(dir.path);
    std::sort(stored.begin(), stored.end());
    CHECK(stored == std::vector<std::string>{"auto", "camoufox"});
    CHECK(stored_backends(dir.path / "missing").empty());
}

TEST_CASE("the router serves each account on its own gateway and keeps one browser", "[router]") {
    Directory dir;
    Store store(dir.path);
    const auto chrome = import_account(store, "chrome"), firefox = import_account(store, "firefox");
    store.set_backend(chrome, "cloakbrowser");
    store.set_backend(firefox, "auto");
    Config config;
    config.data_dir = dir.path;
    Calls calls;
    BackendRouter router(config, store, recording(calls));

    // Before any work the default backend's gateway answers diagnostics.
    auto diagnostics = router.diagnostics();
    CHECK(string_or(diagnostics, "status") == "cloakbrowser_status");
    CHECK(string_or(diagnostics, "backend") == "cloakbrowser");
    CHECK(string_or(diagnostics, "default_backend") == "cloakbrowser");

    router.generate({}, account(chrome), {}, {});
    router.generate({}, account(firefox), {}, {});
    CHECK(calls.log == std::vector<std::string>{"create:cloakbrowser", "cloakbrowser:generate:" + chrome, "create:camoufox",
                                                "cloakbrowser:close", "camoufox:generate:" + firefox});
    CHECK(string_or(router.diagnostics(), "backend") == "camoufox");
    CHECK(router.backend_for(account(firefox)) == "camoufox");

    calls.log.clear();
    router.reset_session();
    router.clear_template();
    CHECK(router.release_idle());
    CHECK(calls.log == std::vector<std::string>{"camoufox:reset_session", "camoufox:clear_template",
                                                "camoufox:release_idle", "cloakbrowser:release_idle"});

    // An account being imported is not stored yet: it uses the default,
    // which follows the configuration without a restart.
    calls.log.clear();
    Account candidate = account("acc_candidate");
    router.probe_import(candidate);
    CHECK(calls.log == std::vector<std::string>{"camoufox:close", "cloakbrowser:probe:acc_candidate"});
    config.browser_backend = "camoufox";
    calls.log.clear();
    router.reconfigure(config);
    CHECK(calls.log == std::vector<std::string>{"camoufox:reconfigure:camoufox", "cloakbrowser:reconfigure:camoufox"});
    CHECK(router.backend_for(Account{}) == "camoufox");
    CHECK(router.backend_for(account(chrome)) == "cloakbrowser");
    calls.log.clear();
    router.close();
    CHECK(calls.log == std::vector<std::string>{"camoufox:close", "cloakbrowser:close"});
}

TEST_CASE("moving accounts between Chrome and Firefox identities needs confirmation", "[router][admin][fingerprint]") {
    Directory dir;
    Store store(dir.path);
    Config config;
    config.data_dir = dir.path;
    const auto id = import_account(store, "switch");
    const auto other = import_account(store, "other");
    // Listing settles the accounts on the configured backend.
    const auto listed = manage(store, config, {{"command", "accounts.list"}}).at("accounts").as_array();
    CHECK(string_or(listed[0].as_object(), "backend") == "cloakbrowser");

    auto view = manage(store, config, {{"command", "accounts.fingerprint"}, {"id", id}});
    CHECK(string_or(view, "backend") == "cloakbrowser");
    CHECK(string_or(view, "identity") == "chrome");
    CHECK(view.at("firefox_identity_saved") == false);

    const auto update = [&](json::object fields) {
        fields["command"] = "accounts.fingerprint.update";
        fields["id"] = id;
        return manage(store, config, fields);
    };
    try {
        update({{"backend", "camoufox"}});
        FAIL("expected a confirmation error");
    } catch (const Error &error) {
        CHECK(error.status == 409);
        CHECK(error.code == "identity_change_unconfirmed");
    }
    CHECK(store.fingerprint(id).backend == "cloakbrowser");
    view = update({{"backend", "camoufox"}, {"confirm_identity_change", true}, {"locale", "en-US"}});
    CHECK(string_or(view, "backend") == "camoufox");
    CHECK(string_or(view, "effective_backend") == "camoufox");
    CHECK(string_or(view, "identity") == "firefox");
    CHECK(string_or(view, "locale") == "en-US");
    // Between Firefox backends the identity stays.
    view = update({{"backend", "auto"}});
    CHECK(string_or(view, "backend") == "auto");
    CHECK(string_or(view, "effective_backend") == "camoufox");
    for (const auto &fields : std::vector<json::object>{{{"backend", "firefox"}}, {{"backend", 1}},
                                                       {{"backend", "auto"}, {"confirm_identity_change", "yes"}}})
        CHECK_THROWS_AS(update(fields), Error);
    try {
        update({{"backend", "spidermonkey"}});
        FAIL("expected an unavailable backend");
    } catch (const Error &error) {
        CHECK(error.status == 400);
        CHECK(error.code == "backend_unavailable");
    }

    // The bulk move is the explicit migration command.
    const auto all = [&](json::object fields) {
        fields["command"] = "accounts.backend.all";
        return manage(store, config, fields);
    };
    CHECK_THROWS_AS(all({{"backend", "cloakbrowser"}}), Error);
    auto moved = all({{"backend", "camoufox"}, {"confirm_identity_change", true}});
    CHECK(moved.at("updated") == 2);
    CHECK(moved.at("identity_changed") == 1);
    CHECK(store.fingerprint(other).backend == "camoufox");
    moved = all({{"backend", "auto"}});
    CHECK(moved.at("identity_changed") == 0);
    CHECK(moved.at("updated") == 2);

    // Changing the configured default leaves stored accounts alone.
    config.browser_backend = "camoufox";
    const auto fresh = import_account(store, "fresh");
    manage(store, config, {{"command", "accounts.list"}});
    CHECK(store.fingerprint(fresh).backend == "auto");
    config.browser_backend = "cloakbrowser";
    CHECK(string_or(manage(store, config, {{"command", "accounts.fingerprint"}, {"id", fresh}}), "identity") == "firefox");
}

TEST_CASE("doctor checks every backend the stored accounts need", "[router][setup]") {
    Directory dir;
    Config config;
    config.data_dir = dir.path;
    config.browser_executable = dir.path / "missing-chrome";
    auto result = doctor(config, dir.path, std::vector<std::string>{});
    CHECK(result.at("backends_needed").as_array() == json::array{"cloakbrowser"});
    CHECK(result.at("deprecations").as_array().size() == 1);
    CHECK(result.at("deprecations").at(0).at("default_backend") == true);
    CHECK(result.at("deprecations").at(0).at("accounts") == false);
    CHECK_FALSE(result.contains("camoufox"));
    // A Firefox account adds Camoufox, whose runtime this directory lacks.
    atomic_write(config.browser_executable, "stub");
    result = doctor(config, dir.path, std::vector<std::string>{"cloakbrowser"});
    CHECK(result.at("browser_exists") == true);
    result = doctor(config, dir.path, std::vector<std::string>{"cloakbrowser", "auto"});
    CHECK(result.at("backends_needed").as_array() == json::array{"cloakbrowser", "camoufox"});
    CHECK(result.contains("camoufox"));
    CHECK(result.at("browser_exists") == false);
    // On Camoufox nothing is deprecated until an account stays on CloakBrowser.
    config.browser_backend = "camoufox";
    CHECK(doctor(config, dir.path, std::vector<std::string>{"auto"}).at("deprecations").as_array().empty());
    result = doctor(config, dir.path, std::vector<std::string>{"auto", "cloakbrowser"});
    CHECK(result.at("deprecations").at(0).at("code") == "cloakbrowser_backend");
    CHECK(result.at("deprecations").at(0).at("default_backend") == false);
    CHECK(result.at("deprecations").at(0).at("accounts") == true);
}
