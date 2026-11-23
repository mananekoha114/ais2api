// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend_router.hpp"
#include "ais2api/management.hpp"
#include "ais2api/runtime.hpp"
#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

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
    std::string benefit_tier(const Account &account) override {
        calls_.log.push_back(name_ + ":benefit_tier:" + account.id);
        return "pro";
    }
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
    CHECK(adopted_backend("camoufox") == "auto");
    CHECK(adopted_backend("auto") == "auto");
    CHECK(adopted_backend("") == "auto");
    for (auto stored : {"auto", "camoufox"})
        CHECK(effective_backend(stored, "camoufox", "windows-x64") == "camoufox");
    CHECK(effective_backend("auto", "camoufox") == default_firefox_backend());
    // Without the engine compiled in, Camoufox stands in for it.
    CHECK(effective_backend("spidermonkey", "camoufox") == (spidermonkey_available() ? "spidermonkey" : "camoufox"));
    CHECK(identity_family("spidermonkey") == "firefox");
    CHECK(effective_backend("", "camoufox") == default_firefox_backend());
    CHECK(effective_backend("unknown", "camoufox") == default_firefox_backend());
    CHECK(identity_family("auto") == "firefox");
    CHECK(identity_family("camoufox") == "firefox");
    CHECK(valid_account_backend("spidermonkey"));
    CHECK(valid_account_backend("auto"));
    CHECK(valid_account_backend("camoufox"));
    CHECK_FALSE(valid_account_backend("cloakbrowser"));
    CHECK_FALSE(valid_account_backend(""));
    CHECK_FALSE(valid_account_backend("Camoufox"));
    CHECK(backends_needed("camoufox", {}, "windows-x64") == std::vector<std::string>{"camoufox"});
    CHECK(backends_needed("camoufox", {"", "auto"}, "windows-x64") == std::vector<std::string>{"camoufox"});
    CHECK(backends_needed("camoufox", {"camoufox"}, "windows-x64") == std::vector<std::string>{"camoufox"});
    // SpiderMonkey stays distinct when compiled in; otherwise Camoufox stands in for it.
    CHECK(backends_needed("camoufox", {"spidermonkey"}, "windows-x64") ==
          (spidermonkey_available() ? std::vector<std::string>{"camoufox", "spidermonkey"}
                                    : std::vector<std::string>{"camoufox"}));
    CHECK(backend_deprecations("camoufox", {}).empty());
    CHECK(backend_deprecations("camoufox", {"auto", "camoufox", "spidermonkey", ""}).empty());
}

TEST_CASE("stored accounts settle on the backend they used and keep it", "[router][state]") {
    Directory dir;
    std::string first, second;
    {
        Store store(dir.path);
        first = import_account(store, "first");
        CHECK(store.account_backend(first) == std::string{});
        CHECK(store.stored_backends() == std::vector<std::string>{""});
        CHECK(store.adopt_backends("auto") == 1);
        CHECK(store.adopt_backends("auto") == 0);
        second = import_account(store, "second");
        CHECK(store.adopt_backends("auto") == 1);
        CHECK(store.fingerprint(first).backend == "auto");
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

TEST_CASE("Store migrates legacy CloakBrowser accounts to auto once on open", "[router][state]") {
    Directory dir;
    std::string id;
    {
        Store store(dir.path);
        id = import_account(store, "legacy");
        store.set_backend(id, "camoufox");
    }
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(path_utf8(dir.path / "state.sqlite3").c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "UPDATE fingerprints SET backend='cloakbrowser'", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    CHECK(stored_backends(dir.path) == std::vector<std::string>{"auto"});
    {
        Store reopened(dir.path);
        CHECK(reopened.migrated_cloakbrowser_backends() == 1);
        CHECK(reopened.fingerprint(id).backend == "auto");
        CHECK(reopened.account_backend(id) == std::string{"auto"});
    }
    Store third(dir.path);
    CHECK(third.migrated_cloakbrowser_backends() == 0);
}

TEST_CASE("the router serves each account on its own gateway and keeps one browser", "[router]") {
    Directory dir;
    Store store(dir.path);
    const auto sm = import_account(store, "sm"), cam = import_account(store, "cam");
    store.set_backend(sm, "spidermonkey");
    store.set_backend(cam, "camoufox");
    Config config;
    config.data_dir = dir.path;
    Calls calls;
    BackendRouter router(config, store, recording(calls), {}, "windows-x64");

    // Before any work the default backend's gateway answers diagnostics.
    auto diagnostics = router.diagnostics();
    CHECK(string_or(diagnostics, "status") == "camoufox_status");
    CHECK(string_or(diagnostics, "backend") == "camoufox");
    CHECK(string_or(diagnostics, "default_backend") == "camoufox");

    router.generate({}, account(sm), {}, {});
    router.generate({}, account(cam), {}, {});
    if (spidermonkey_available()) {
        CHECK(calls.log == std::vector<std::string>{"create:camoufox", "create:spidermonkey", "spidermonkey:generate:" + sm,
                                                    "spidermonkey:close", "camoufox:generate:" + cam});
        CHECK(router.backend_for(account(sm)) == "spidermonkey");
    } else {
        CHECK(calls.log == std::vector<std::string>{"create:camoufox", "camoufox:generate:" + sm,
                                                    "camoufox:generate:" + cam});
        CHECK(router.backend_for(account(sm)) == "camoufox");
    }
    CHECK(string_or(router.diagnostics(), "backend") == "camoufox");
    CHECK(router.backend_for(account(cam)) == "camoufox");

    calls.log.clear();
    router.reset_session();
    router.clear_template();
    CHECK(router.release_idle());
    if (spidermonkey_available()) {
        CHECK(calls.log == std::vector<std::string>{"camoufox:reset_session", "camoufox:clear_template",
                                                    "camoufox:release_idle", "spidermonkey:release_idle"});
    } else {
        CHECK(calls.log == std::vector<std::string>{"camoufox:reset_session", "camoufox:clear_template",
                                                    "camoufox:release_idle"});
    }

    calls.log.clear();
    Account candidate = account("acc_candidate");
    router.probe_import(candidate);
    CHECK(calls.log == std::vector<std::string>{"camoufox:probe:acc_candidate"});
    config.browser_backend = "camoufox";
    calls.log.clear();
    router.reconfigure(config);
    if (spidermonkey_available()) {
        CHECK(calls.log == std::vector<std::string>{"camoufox:reconfigure:camoufox", "spidermonkey:reconfigure:camoufox"});
    } else {
        CHECK(calls.log == std::vector<std::string>{"camoufox:reconfigure:camoufox"});
    }
    CHECK(router.backend_for(Account{}) == "camoufox");
    calls.log.clear();
    router.close();
    if (spidermonkey_available()) {
        CHECK(calls.log == std::vector<std::string>{"camoufox:close", "spidermonkey:close"});
    } else {
        CHECK(calls.log == std::vector<std::string>{"camoufox:close"});
    }
}

TEST_CASE("the router asks only the running gateway for an account's tier", "[router][tier]") {
    Directory dir;
    Store store(dir.path);
    const auto cam = import_account(store, "cam");
    store.set_backend(cam, "camoufox");
    Config config;
    config.data_dir = dir.path;
    Calls calls;
    BackendRouter router(config, store, recording(calls), {}, "windows-x64");
    // Nothing runs yet: no gateway is created, started or asked.
    CHECK(router.benefit_tier(account(cam)).empty());
    CHECK(calls.log.empty());
    router.generate({}, account(cam), {}, {});
    calls.log.clear();
    CHECK(router.benefit_tier(account(cam)) == "pro");
    CHECK(calls.log == std::vector<std::string>{"camoufox:benefit_tier:" + cam});
}

TEST_CASE("accounts switch freely between Firefox backends and reject CloakBrowser", "[router][admin][fingerprint]") {
    Directory dir;
    Store store(dir.path);
    Config config;
    config.data_dir = dir.path;
    const auto id = import_account(store, "switch");
    const auto other = import_account(store, "other");
    // Listing settles the accounts on "auto".
    const auto listed = manage(store, config, {{"command", "accounts.list"}}).at("accounts").as_array();
    CHECK(string_or(listed[0].as_object(), "backend") == "auto");

    auto view = manage(store, config, {{"command", "accounts.fingerprint"}, {"id", id}});
    CHECK(string_or(view, "backend") == "auto");
    CHECK(string_or(view, "identity") == "firefox");

    const auto update = [&](json::object fields) {
        fields["command"] = "accounts.fingerprint.update";
        fields["id"] = id;
        return manage(store, config, fields);
    };
    view = update({{"backend", "camoufox"}, {"locale", "en-US"}});
    CHECK(string_or(view, "backend") == "camoufox");
    CHECK(string_or(view, "effective_backend") == "camoufox");
    CHECK(string_or(view, "identity") == "firefox");
    CHECK(string_or(view, "locale") == "en-US");
    // Deprecated boolean confirm_identity_change remains tolerated as a no-op.
    view = update({{"backend", "auto"}, {"confirm_identity_change", true}});
    CHECK(string_or(view, "backend") == "auto");
    CHECK(string_or(view, "effective_backend") == std::string(default_firefox_backend()));
    for (const auto &fields : std::vector<json::object>{{{"backend", "cloakbrowser"}}, {{"backend", "firefox"}},
                                                       {{"backend", 1}}, {{"backend", "auto"}, {"confirm_identity_change", "yes"}}})
        CHECK_THROWS_AS(update(fields), Error);
    if (spidermonkey_available()) {
        view = update({{"backend", "spidermonkey"}});
        CHECK(string_or(view, "effective_backend") == "spidermonkey");
        CHECK(string_or(view, "identity") == "firefox");
        view = update({{"backend", "auto"}});
    } else {
        try {
            update({{"backend", "spidermonkey"}});
            FAIL("expected an unavailable backend");
        } catch (const Error &error) {
            CHECK(error.status == 400);
            CHECK(error.code == "backend_unavailable");
        }
    }

    const auto all = [&](json::object fields) {
        fields["command"] = "accounts.backend.all";
        return manage(store, config, fields);
    };
    CHECK_THROWS_AS(all({{"backend", "cloakbrowser"}}), Error);
    auto moved = all({{"backend", "camoufox"}});
    CHECK(moved.at("updated") == 2);
    CHECK(moved.at("identity_changed") == 0);
    CHECK(store.fingerprint(other).backend == "camoufox");
    moved = all({{"backend", "auto"}});
    CHECK(moved.at("identity_changed") == 0);
    CHECK(moved.at("updated") == 2);
}

TEST_CASE("doctor checks every backend the stored accounts need", "[router][setup]") {
    Directory dir;
    Config config;
    config.data_dir = dir.path;
    config.browser_executable = dir.path / "missing-camoufox";
    auto result = doctor(config, dir.path, std::vector<std::string>{}, "windows-x64");
    CHECK(result.at("backends_needed").as_array() == json::array{"camoufox"});
    CHECK(result.at("deprecations").as_array().empty());
    CHECK(result.contains("camoufox"));
    CHECK(result.contains("fpgen_model"));
    CHECK(result.at("browser_exists") == false);
    if (spidermonkey_available()) {
        result = doctor(config, dir.path, std::vector<std::string>{"auto"}, "termux-arm64");
        CHECK(result.at("backends_needed").as_array() == json::array{"spidermonkey"});
        CHECK(result.contains("spidermonkey"));
        CHECK_FALSE(result.contains("camoufox"));
    }
}

TEST_CASE("BackendRouter serializes concurrent switches and recovers when close throws", "[router]") {
    if (!spidermonkey_available()) return;
    Directory dir;
    Store store(dir.path);
    const auto sm = import_account(store, "sm"), cam = import_account(store, "cam");
    store.set_backend(sm, "spidermonkey");
    store.set_backend(cam, "camoufox");
    Config config;
    config.data_dir = dir.path;

    struct Shared {
        std::atomic<int> in_close{0};
        std::atomic<int> concurrent_close{0};
        std::atomic<int> cam_closes{0};
        std::atomic<bool> throw_cam_close{false};
    };
    auto shared = std::make_shared<Shared>();
    class SwitchGateway final : public GenerationBackend {
      public:
        SwitchGateway(std::string name, std::shared_ptr<Shared> state)
            : name_(std::move(name)), state_(std::move(state)) {}
        void generate(const Request &, const Account &, const EventSink &, std::stop_token) override {}
        json::array models(const Account &, std::stop_token) override { return {}; }
        json::object probe(const Account &, std::stop_token) override { return {}; }
        json::object diagnostics() override { return {{"status", name_}}; }
        void reconfigure(const Config &) override {}
        void close() override {
            if (state_->in_close.fetch_add(1) > 0) state_->concurrent_close.fetch_add(1);
            struct Guard {
                std::atomic<int> &count;
                ~Guard() { count.fetch_sub(1); }
            } guard{state_->in_close};
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            if (name_ == "camoufox") {
                state_->cam_closes.fetch_add(1);
                if (state_->throw_cam_close.exchange(false))
                    throw Error(500, "close_failed", "Simulated close error");
            }
        }
        void reset_session() override {}
        void clear_template() override {}
        bool release_idle() override { return false; }

      private:
        std::string name_;
        std::shared_ptr<Shared> state_;
    };

    BackendRouter router(
        config, store,
        [shared](std::string_view backend, const Config &, Store &) {
            return std::make_unique<SwitchGateway>(std::string(backend), shared);
        },
        {}, "windows-x64");

    std::vector<std::jthread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < 6; ++i)
                router.generate({}, account(((t + i) % 2 == 0) ? sm : cam), {}, {});
        });
    }
    workers.clear();
    CHECK(shared->concurrent_close.load() == 0);

    router.generate({}, account(cam), {}, {});
    const int closes_before = shared->cam_closes.load();
    shared->throw_cam_close.store(true);
    CHECK_THROWS_AS(router.generate({}, account(sm), {}, {}), Error);
    CHECK(shared->cam_closes.load() == closes_before + 1);
    // current_ was cleared before close() threw, so retrying does not close camoufox again.
    REQUIRE_NOTHROW(router.generate({}, account(sm), {}, {}));
    CHECK(shared->cam_closes.load() == closes_before + 1);
    CHECK(string_or(router.diagnostics(), "backend") == "spidermonkey");
}
