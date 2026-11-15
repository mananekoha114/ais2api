// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend_router.hpp"
#include "ais2api/diagnostics.hpp"
#include "ais2api/management.hpp"
#include "ais2api/runtime.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace ais2api;
namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-failover-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string import_account(Store &store, const std::string &cookie, const std::string &name = {}) {
    return string_or(store.import_accounts(json::value("SAPISID=synthetic-" + cookie), name)[0].as_object(), "id");
}

Account make_account(std::string id) {
    Account result;
    result.id = std::move(id);
    return result;
}

json::object text_event(std::string text) {
    json::array parts{json::object{{"text", std::move(text)}}};
    json::array candidates{
        json::object{{"index", 0}, {"content", json::object{{"role", "model"}, {"parts", std::move(parts)}}}}};
    return {{"candidates", std::move(candidates)}, {"responseId", "synthetic-failover"}};
}

json::object finish_event() {
    json::array candidates{json::object{{"index", 0}, {"finishReason", "STOP"}}};
    return {{"candidates", std::move(candidates)},
            {"usageMetadata",
             json::object{{"promptTokenCount", 4}, {"candidatesTokenCount", 2}, {"totalTokenCount", 6}}}};
}

struct AttemptRecord {
    std::string backend;
    std::string account_id;
    std::string model;
};

struct FailoverState {
    std::mutex mutex;
    std::vector<AttemptRecord> attempts;
    std::vector<std::string> events;
    std::function<void(std::string_view backend, const Account &account, const Request &request, unsigned backend_call)>
        behavior;
    std::map<std::string, unsigned, std::less<>> backend_calls;
};

class ScriptedGateway final : public GenerationBackend {
  public:
    ScriptedGateway(std::string name, std::shared_ptr<FailoverState> state)
        : name_(std::move(name)), state_(std::move(state)) {}

    void generate(const Request &request, const Account &account, const EventSink &emit, std::stop_token) override {
        unsigned call_index = 0;
        std::function<void(std::string_view, const Account &, const Request &, unsigned)> behavior;
        {
            std::lock_guard lock(state_->mutex);
            call_index = ++state_->backend_calls[name_];
            state_->attempts.push_back({name_, account.id, request.model});
            behavior = state_->behavior;
        }
        if (behavior) behavior(name_, account, request, call_index);
        emit(text_event("ok:" + name_));
        emit(finish_event());
    }

    json::array models(const Account &, std::stop_token) override { return {}; }
    json::object probe(const Account &, std::stop_token) override { return {{"ok", true}, {"status", "ok"}}; }
    json::object diagnostics() override { return {{"status", name_ + "_ready"}}; }
    void reconfigure(const Config &) override {}
    void close() override {
        std::lock_guard lock(state_->mutex);
        state_->events.push_back(name_ + ":close");
    }
    void reset_session() override {
        std::lock_guard lock(state_->mutex);
        state_->events.push_back(name_ + ":reset_session");
    }
    void clear_template() override {
        std::lock_guard lock(state_->mutex);
        state_->events.push_back(name_ + ":clear_template");
    }
    bool release_idle() override { return false; }

  private:
    std::string name_;
    std::shared_ptr<FailoverState> state_;
};

void receive_timeout(tcp::socket &socket) {
#ifdef _WIN32
    DWORD timeout = 5000;
#else
    timeval timeout{5, 0};
#endif
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                     sizeof(timeout)) != 0)
        throw std::runtime_error("Cannot configure failover test receive timeout");
}

struct FailoverServer {
    Directory directory;
    Config config;
    std::string key = "failover-test-" + random_hex(24);
    std::shared_ptr<FailoverState> state = std::make_shared<FailoverState>();
    std::unique_ptr<Store> router_store;
    std::string first_id;
    std::string second_id;
    std::jthread thread;
    std::atomic<bool> failed{false};

    FailoverServer(unsigned max_attempts = 3, BackendRouter::Readiness readiness = {}, std::string platform = {}) {
        net::io_context io;
        tcp::acceptor available(io, {tcp::v4(), 0});
        config.port = available.local_endpoint().port();
        available.close();
        config.data_dir = directory.path / "data";
        config.config_file = directory.path / "config.json";
        config.browser_backend = "camoufox";
        const auto fake_camoufox = directory.path / "camoufox.exe";
        atomic_write(fake_camoufox, "stub");
        config.browser_executable = fake_camoufox;
        config.limits.max_attempts = max_attempts;
        config.limits.queue_seconds = 5;
        config.limits.generation_seconds = 5;
        config.limits.stream_idle_seconds = 5;
        config.api_key = key;
        config.admin_key = key;
        atomic_write(config.config_file, json::serialize(config.to_json()), true);

        router_store = std::make_unique<Store>(config.data_dir);
        first_id = import_account(*router_store, "first-account", "First");
        second_id = import_account(*router_store, "second-account", "Second");
        adopt_backends(*router_store, config);

        auto factory = [state = state](std::string_view backend, const Config &, Store &) {
            return std::make_unique<ScriptedGateway>(std::string(backend), state);
        };
        auto router = std::make_unique<BackendRouter>(config, *router_store, std::move(factory), std::move(readiness),
                                                      std::move(platform));
        thread = std::jthread([this, router = std::move(router)]() mutable {
            try {
                serve_with_backend(config, std::move(router));
            } catch (...) {
                failed = true;
            }
        });
        bool ready = false;
        for (const auto deadline = Clock::now() + 30s; Clock::now() < deadline && !failed;) {
            try {
                ready = bool_or(local_call(config, {{"command", "status"}}), "running");
            } catch (const Error &) {
            }
            if (ready) break;
            std::this_thread::sleep_for(20ms);
        }
        if (!ready) {
            try {
                local_call(config, {{"command", "stop"}});
            } catch (...) {
            }
            if (thread.joinable()) thread.join();
            throw std::runtime_error("Failover test service did not become ready");
        }
    }

    ~FailoverServer() {
        try {
            local_call(config, {{"command", "stop"}});
        } catch (...) {
        }
        if (thread.joinable()) thread.join();
        router_store.reset();
    }

    http::response<http::string_body> call(std::string target, std::string body = {},
                                           http::verb method = http::verb::post) {
        net::io_context io;
        tcp::socket socket(io);
        socket.connect({net::ip::make_address("127.0.0.1"), config.port});
        receive_timeout(socket);
        http::request<http::string_body> req(method, std::move(target), 11);
        req.set(http::field::host, "127.0.0.1");
        req.set(http::field::authorization, "Bearer " + key);
        if (!body.empty()) {
            req.set(http::field::content_type, "application/json");
            req.body() = std::move(body);
        }
        req.prepare_payload();
        http::write(socket, req);
        boost::beast::flat_buffer input;
        http::response_parser<http::string_body> parser;
        parser.body_limit(262144);
        http::read(socket, input, parser);
        return parser.release();
    }

    http::response<http::string_body> generate_chat(std::string model = "gemini-2.5-pro") {
        const json::object payload{
            {"model", std::move(model)},
            {"messages", json::array{json::object{{"role", "user"}, {"content", "hi"}}}},
            {"stream", false}};
        return call("/v1/chat/completions", json::serialize(payload), http::verb::post);
    }
};
} // namespace

TEST_CASE("Termux and desktop Firefox routing select the native default and needed runtimes",
          "[router][setup][failover]") {
    for (const auto *desktop : {"windows-x64", "linux-x64", "linux-arm64"}) {
        CHECK(default_firefox_backend(desktop) == "camoufox");
        CHECK(effective_backend("auto", "camoufox", desktop) == "camoufox");
        CHECK(effective_backend("", "camoufox", desktop) == "camoufox");
        CHECK(backends_needed("camoufox", {"auto"}, desktop) == std::vector<std::string>{"camoufox"});
    }

    const std::string expected_termux = spidermonkey_available() ? "spidermonkey" : "camoufox";
    CHECK(default_firefox_backend("termux-arm64") == expected_termux);
    CHECK(effective_backend("auto", "camoufox", "termux-arm64") == expected_termux);
    CHECK(effective_backend("", "camoufox", "termux-arm64") == expected_termux);
    CHECK(backends_needed("camoufox", {"auto"}, "termux-arm64") == std::vector<std::string>{expected_termux});

    if (spidermonkey_available()) {
        CHECK(backends_needed("camoufox", {"auto", "camoufox"}, "termux-arm64") ==
              std::vector<std::string>{"spidermonkey", "camoufox"});
        CHECK(backends_needed("camoufox", {"spidermonkey"}, "windows-x64") ==
              std::vector<std::string>{"camoufox", "spidermonkey"});
    } else {
        CHECK(backends_needed("camoufox", {"auto", "camoufox"}, "termux-arm64") ==
              std::vector<std::string>{"camoufox"});
        CHECK(backends_needed("camoufox", {"spidermonkey"}, "windows-x64") ==
              std::vector<std::string>{"camoufox"});
    }
}

TEST_CASE("doctor, setup and safe diagnostics report SpiderMonkey and fpgen readiness",
          "[setup][diagnostics][failover]") {
    Directory dir;
    Config config;
    config.data_dir = dir.path / "data";
    config.config_file = dir.path / "config.json";
    config.browser_backend = "camoufox";

    const auto termux_doc = doctor(config, dir.path, std::vector<std::string>{"auto"}, "termux-arm64");
    CHECK(termux_doc.contains("fpgen_model"));
    if (spidermonkey_available()) {
        CHECK(termux_doc.at("backends_needed").as_array() == json::array{"spidermonkey"});
        CHECK_FALSE(termux_doc.contains("camoufox"));
        REQUIRE(termux_doc.contains("spidermonkey"));
        const auto &sm = termux_doc.at("spidermonkey").as_object();
        CHECK(sm.at("available") == true);
        CHECK(termux_doc.at("browser_configured") == true);
#ifdef AIS2API_DEV_BUNDLE
        CHECK(string_or(sm, "state") == "ready");
        CHECK(sm.at("setup_needed") == false);
        CHECK(termux_doc.at("browser_exists") == true);
#else
        CHECK(string_or(sm, "state") == "missing");
        CHECK(sm.at("setup_needed") == true);
        CHECK(termux_doc.at("browser_exists") == false);
#endif
    } else {
        CHECK(termux_doc.at("backends_needed").as_array() == json::array{"camoufox"});
    }

    DiagnosticsSources sources;
    sources.config = config;
    sources.runtime = termux_doc;
    sources.platform = "termux-arm64";
    const auto report = safe_report(sources);
    const auto &runtime = report.at("runtime").as_object();
    if (spidermonkey_available()) {
        CHECK(runtime.at("backends_needed").as_array() == json::array{"spidermonkey"});
        REQUIRE(runtime.contains("spidermonkey"));
        CHECK(runtime.at("spidermonkey").as_object().at("available") == true);
    } else {
        CHECK(runtime.at("backends_needed").as_array() == json::array{"camoufox"});
    }

    try {
        rollback_runtime(config, dir.path, "spidermonkey");
        FAIL("expected rollback_missing for spidermonkey");
    } catch (const Error &error) {
        CHECK(error.status == 409);
        CHECK(error.code == "rollback_missing");
    }

    try {
        setup_runtime(config, false, "firefox");
        FAIL("expected invalid_backend");
    } catch (const Error &error) {
        CHECK(error.status == 400);
        CHECK(error.code == "invalid_backend");
    }

    if (!spidermonkey_available()) {
        try {
            setup_runtime(config, false, "spidermonkey");
            FAIL("expected backend_unavailable");
        } catch (const Error &error) {
            CHECK(error.status == 400);
            CHECK(error.code == "backend_unavailable");
        }
    }

    CHECK(firefox_backend_ready("spidermonkey", config, dir.path) == spidermonkey_available());
    CHECK_FALSE(firefox_backend_ready("camoufox", config, dir.path));
    const auto fake_camoufox = dir.path / "camoufox.exe";
    atomic_write(fake_camoufox, "stub");
    config.browser_executable = fake_camoufox;
    if (runtime_platform() != "termux-arm64") {
        CHECK(firefox_backend_ready("camoufox", config, dir.path));
    }
}

TEST_CASE("Store persists 6-hour auto backend switches and clears them on manual actions",
          "[store][admin][diagnostics][failover]") {
    CHECK(backend_switch_seconds == 21600);
    Directory dir;
    Config config;
    config.data_dir = dir.path / "data";
    config.config_file = dir.path / "config.json";
    config.browser_backend = "camoufox";
    const auto fake_camoufox = dir.path / "camoufox.exe";
    atomic_write(fake_camoufox, "stub");
    config.browser_executable = fake_camoufox;

    std::string auto_id, pinned_id;
    std::int64_t before = 0;
    {
        Store store(config.data_dir);
        auto_id = import_account(store, "auto-acc", "Auto");
        pinned_id = import_account(store, "pinned-acc", "Pinned");
        adopt_backends(store, config);
        store.set_backend(pinned_id, "camoufox");

        before = unix_seconds();
        store.switch_backend(auto_id, "spidermonkey");
        const auto sw = store.backend_switch(auto_id);
        REQUIRE(sw.has_value());
        CHECK(sw->backend == "spidermonkey");
        CHECK(sw->until >= before + backend_switch_seconds - 2);
        CHECK(sw->until <= unix_seconds() + backend_switch_seconds + 2);
    }

    // Reopen Store to verify SQLite persistence across restarts.
    Store store(config.data_dir);
    const auto persisted = store.backend_switch(auto_id);
    REQUIRE(persisted.has_value());
    CHECK(persisted->backend == "spidermonkey");

    const auto statuses = store.account_status(config);
    REQUIRE(statuses.size() == 2);
    const auto &auto_status = statuses[0].as_object();
    const auto fp_view = manage(store, config, {{"command", "accounts.fingerprint"}, {"id", auto_id}});
    const auto listed = manage(store, config, {{"command", "accounts.list"}}).at("accounts").as_array();
    const auto diag = manage(store, config, {{"command", "diagnostics.report"}});
    const auto &diag_acc = diag.at("accounts").as_array()[0].as_object();

    if (spidermonkey_available()) {
        CHECK(string_or(auto_status, "effective_backend") == "spidermonkey");
        CHECK(auto_status.at("backend_switched_until").to_number<std::int64_t>() == persisted->until);
        CHECK(string_or(fp_view, "effective_backend") == "spidermonkey");
        CHECK(fp_view.at("backend_switched_until").to_number<std::int64_t>() == persisted->until);
        CHECK(string_or(listed[0].as_object(), "effective_backend") == "spidermonkey");
        CHECK(listed[0].as_object().at("backend_switched_until").to_number<std::int64_t>() == persisted->until);
        CHECK(string_or(diag_acc, "label") == "account-1");
        CHECK(string_or(diag_acc, "effective_backend") == "spidermonkey");
        CHECK(diag_acc.at("backend_switched_until").to_number<std::int64_t>() == persisted->until);
    } else {
        CHECK(string_or(auto_status, "effective_backend") == std::string(default_firefox_backend()));
        CHECK_FALSE(auto_status.contains("backend_switched_until"));
        CHECK(string_or(fp_view, "effective_backend") == std::string(default_firefox_backend()));
        CHECK_FALSE(fp_view.contains("backend_switched_until"));
        CHECK_FALSE(diag_acc.contains("backend_switched_until"));
    }

    // Switching to Camoufox reports backend_switched_until only when Camoufox is configured.
    store.switch_backend(auto_id, "camoufox", 1200);
    if (runtime_platform() != "termux-arm64") {
        const auto with_camoufox = store.account_status(config)[0].as_object();
        CHECK(string_or(with_camoufox, "effective_backend") == "camoufox");
        CHECK(with_camoufox.contains("backend_switched_until"));
    }
    Config unconfigured = config;
    unconfigured.browser_executable.clear();
    const auto without_camoufox = store.account_status(unconfigured)[0].as_object();
    CHECK_FALSE(without_camoufox.contains("backend_switched_until"));

    // Expired switches are ignored.
    store.switch_backend(auto_id, "camoufox", -5);
    CHECK_FALSE(store.backend_switch(auto_id).has_value());
    CHECK_FALSE(store.account_status(config)[0].as_object().contains("backend_switched_until"));

    // Manual set_backend clears active switches.
    store.switch_backend(auto_id, "camoufox", 3600);
    REQUIRE(store.backend_switch(auto_id).has_value());
    store.set_backend(auto_id, "auto");
    CHECK_FALSE(store.backend_switch(auto_id).has_value());

    // reset_account clears active switches.
    store.switch_backend(auto_id, "camoufox", 3600);
    REQUIRE(store.backend_switch(auto_id).has_value());
    store.reset_account(auto_id);
    CHECK_FALSE(store.backend_switch(auto_id).has_value());

    // clear_cooldown (both full and model-scoped) clears active switches.
    store.switch_backend(auto_id, "camoufox", 3600);
    REQUIRE(store.backend_switch(auto_id).has_value());
    store.clear_cooldown(auto_id, "gemini-2.5-pro");
    CHECK_FALSE(store.backend_switch(auto_id).has_value());

    store.switch_backend(auto_id, "camoufox", 3600);
    REQUIRE(store.backend_switch(auto_id).has_value());
    store.clear_cooldown(auto_id);
    CHECK_FALSE(store.backend_switch(auto_id).has_value());
}

TEST_CASE("BackendRouter routes auto accounts to ready switch targets and never moves pinned accounts",
          "[router][failover]") {
    Directory dir;
    Store store(dir.path);
    Config config;
    config.data_dir = dir.path;
    config.browser_backend = "camoufox";

    const auto auto_id = import_account(store, "auto");
    const auto cam_id = import_account(store, "cam");
    const auto sm_id = import_account(store, "sm");
    const auto chrome_id = import_account(store, "chrome");
    store.set_backend(auto_id, "auto");
    store.set_backend(cam_id, "camoufox");
    store.set_backend(sm_id, "spidermonkey");
    store.set_backend(chrome_id, "cloakbrowser");

    auto state = std::make_shared<FailoverState>();
    auto factory = [state](std::string_view backend, const Config &, Store &) {
        return std::make_unique<ScriptedGateway>(std::string(backend), state);
    };

    bool camoufox_ready = true;
    bool spidermonkey_ready = true;
    auto readiness = [&](std::string_view backend, const Config &) {
        if (backend == "camoufox") return camoufox_ready;
        if (backend == "spidermonkey") return spidermonkey_ready;
        return false;
    };

    BackendRouter desktop_router(config, store, factory, readiness, "windows-x64");
    CHECK(desktop_router.backend_for(make_account(auto_id)) == "camoufox");
    if (spidermonkey_available()) {
        REQUIRE(desktop_router.alternate_firefox_backend(make_account(auto_id)) == "spidermonkey");
        spidermonkey_ready = false;
        CHECK_FALSE(desktop_router.alternate_firefox_backend(make_account(auto_id)).has_value());
        spidermonkey_ready = true;
    } else {
        CHECK_FALSE(desktop_router.alternate_firefox_backend(make_account(auto_id)).has_value());
    }

    // Pinned accounts ignore switches and never offer an alternate Firefox backend.
    for (const auto &id : {cam_id, sm_id, chrome_id}) {
        store.switch_backend(id, "camoufox", 3600);
        CHECK_FALSE(desktop_router.alternate_firefox_backend(make_account(id)).has_value());
    }
    CHECK(desktop_router.backend_for(make_account(cam_id)) == "camoufox");
    CHECK(desktop_router.backend_for(make_account(chrome_id)) == "cloakbrowser");
    CHECK(desktop_router.backend_for(make_account(sm_id)) == (spidermonkey_available() ? "spidermonkey" : "camoufox"));

    // Simulated Termux: falls back safely when Camoufox is not installed in proot.
    BackendRouter termux_router(config, store, factory, readiness, "termux-arm64");
    store.switch_backend(auto_id, "camoufox", 3600);
    camoufox_ready = true;
    CHECK(termux_router.backend_for(make_account(auto_id)) == "camoufox");
    camoufox_ready = false;
    CHECK(termux_router.backend_for(make_account(auto_id)) ==
          (spidermonkey_available() ? "spidermonkey" : "camoufox"));
    if (spidermonkey_available()) {
        store.clear_backend_switch(auto_id);
        CHECK(termux_router.backend_for(make_account(auto_id)) == "spidermonkey");
        CHECK_FALSE(termux_router.alternate_firefox_backend(make_account(auto_id)).has_value());
        camoufox_ready = true;
        CHECK(termux_router.alternate_firefox_backend(make_account(auto_id)) == "camoufox");
    }
}

TEST_CASE("Service generate fails over auto Firefox accounts for 6 hours after repeated 403",
          "[server][failover]") {
    SECTION("a single 403 recovered by session rebuild stays on the initial backend") {
        FailoverServer server(3, [](std::string_view, const Config &) { return true; }, "windows-x64");
        server.state->behavior = [](std::string_view backend, const Account &, const Request &, unsigned call_index) {
            if (backend == "camoufox" && call_index == 1)
                throw Error(403, "permission_denied", "Transient BotGuard failure");
        };
        const auto response = server.generate_chat();
        CHECK(response.result_int() == 200);
        CHECK_FALSE(server.router_store->backend_switch(server.first_id).has_value());
        std::lock_guard lock(server.state->mutex);
        REQUIRE(server.state->attempts.size() == 2);
        CHECK(server.state->attempts[0].backend == "camoufox");
        CHECK(server.state->attempts[1].backend == "camoufox");
        CHECK(server.state->attempts[0].account_id == server.first_id);
        CHECK(server.state->attempts[1].account_id == server.first_id);
    }

#ifdef AIS2API_HAS_SPIDERMONKEY
    SECTION("repeated 403 after session rebuild switches auto account for 6 hours even at max_attempts=2") {
        FailoverServer server(2, [](std::string_view, const Config &) { return true; }, "windows-x64");
        server.state->behavior = [](std::string_view backend, const Account &, const Request &, unsigned) {
            if (backend == "camoufox")
                throw Error(403, "permission_denied", "Repeated BotGuard rejection on Camoufox");
        };

        const auto first_response = server.generate_chat();
        REQUIRE(first_response.result_int() == 200);
        CHECK(first_response.body().find("ok:spidermonkey") != std::string::npos);

        const auto switched = server.router_store->backend_switch(server.first_id);
        REQUIRE(switched.has_value());
        CHECK(switched->backend == "spidermonkey");
        CHECK(switched->until >= unix_seconds() + backend_switch_seconds - 30);

        {
            std::lock_guard lock(server.state->mutex);
            REQUIRE(server.state->attempts.size() == 3);
            CHECK(server.state->attempts[0].backend == "camoufox");
            CHECK(server.state->attempts[1].backend == "camoufox");
            CHECK(server.state->attempts[2].backend == "spidermonkey");
            for (const auto &attempt : server.state->attempts)
                CHECK(attempt.account_id == server.first_id);
        }

        // Next request within the 6-hour window goes straight to SpiderMonkey.
        const auto second_response = server.generate_chat();
        REQUIRE(second_response.result_int() == 200);
        {
            std::lock_guard lock(server.state->mutex);
            REQUIRE(server.state->attempts.size() == 4);
            CHECK(server.state->attempts[3].backend == "spidermonkey");
            CHECK(server.state->attempts[3].account_id == server.first_id);
        }

        // Management accounts endpoint, diagnostics counters and safe export expose the switch.
        const auto accounts_res = server.call("/api/admin/v1/accounts", {}, http::verb::get);
        REQUIRE(accounts_res.result_int() == 200);
        const auto accounts_json = json::parse(accounts_res.body()).as_object().at("accounts").as_array();
        CHECK(string_or(accounts_json[0].as_object(), "effective_backend") == "spidermonkey");
        CHECK(accounts_json[0].as_object().contains("backend_switched_until"));

        const auto diag_res = server.call("/api/admin/v1/diagnostics", {}, http::verb::get);
        REQUIRE(diag_res.result_int() == 200);
        const auto diag_json = json::parse(diag_res.body()).as_object();
        const auto &cam_events =
            diag_json.at("events").as_object().at("backends").as_object().at("camoufox").as_object();
        CHECK(cam_events.at("backend_switch").as_object().at("total") == 1);

        const auto export_res = server.call("/api/admin/v1/diagnostics/export", "{}", http::verb::post);
        REQUIRE(export_res.result_int() == 200);
        const auto export_json = json::parse(export_res.body()).as_object();
        const auto &export_acc = export_json.at("accounts").as_array()[0].as_object();
        CHECK(string_or(export_acc, "effective_backend") == "spidermonkey");
        CHECK(export_acc.contains("backend_switched_until"));
        const auto &recent = export_json.at("recent").as_array();
        const bool found_switch_event = std::any_of(recent.begin(), recent.end(), [](const json::value &item) {
            const auto &obj = item.as_object();
            return string_or(obj, "event") == "backend_switch" &&
                   string_or(obj, "from_backend") == "camoufox" &&
                   string_or(obj, "to_backend") == "spidermonkey" &&
                   string_or(obj, "code") == "permission_denied" &&
                   string_or(obj, "account") == "account-1";
        });
        CHECK(found_switch_event);
    }

    SECTION("Termux SpiderMonkey fails over to Camoufox only when Camoufox is ready") {
        std::atomic<bool> camoufox_ready{false};
        FailoverServer native_termux(
            3, [&](std::string_view backend, const Config &) { return backend == "spidermonkey" || camoufox_ready.load(); },
            "termux-arm64");
        const auto first = native_termux.first_id;
        native_termux.state->behavior = [first](std::string_view backend, const Account &account, const Request &, unsigned) {
            if (backend == "spidermonkey" && account.id == first)
                throw Error(403, "permission_denied", "BotGuard rejection on SpiderMonkey");
        };

        // Without Camoufox installed in proot, native Termux cools down account 1 and rotates to account 2.
        const auto rotated_res = native_termux.generate_chat();
        REQUIRE(rotated_res.result_int() == 200);
        CHECK_FALSE(native_termux.router_store->backend_switch(first).has_value());
        {
            std::lock_guard lock(native_termux.state->mutex);
            REQUIRE(native_termux.state->attempts.size() == 3);
            CHECK(native_termux.state->attempts[0].account_id == first);
            CHECK(native_termux.state->attempts[1].account_id == first);
            CHECK(native_termux.state->attempts[2].account_id == native_termux.second_id);
            CHECK(native_termux.state->attempts[2].backend == "spidermonkey");
        }

        // Clear cooldown and mark Camoufox ready: repeated 403 now switches account 1 to Camoufox.
        native_termux.router_store->clear_cooldown(first);
        native_termux.router_store->select_account(first);
        {
            std::lock_guard lock(native_termux.state->mutex);
            native_termux.state->attempts.clear();
        }
        camoufox_ready = true;
        const auto switched_res = native_termux.generate_chat();
        REQUIRE(switched_res.result_int() == 200);
        CHECK(switched_res.body().find("ok:camoufox") != std::string::npos);
        const auto sw = native_termux.router_store->backend_switch(first);
        REQUIRE(sw.has_value());
        CHECK(sw->backend == "camoufox");
    }

    SECTION("pinned accounts never auto-switch backends on repeated 403") {
        FailoverServer server(3, [](std::string_view, const Config &) { return true; }, "windows-x64");
        server.router_store->set_backend(server.first_id, "camoufox");
        const auto first = server.first_id;
        server.state->behavior = [first](std::string_view, const Account &account, const Request &, unsigned) {
            if (account.id == first)
                throw Error(403, "permission_denied", "Pinned Camoufox 403");
        };
        const auto response = server.generate_chat();
        REQUIRE(response.result_int() == 200);
        CHECK_FALSE(server.router_store->backend_switch(first).has_value());
        std::lock_guard lock(server.state->mutex);
        REQUIRE(server.state->attempts.size() == 3);
        CHECK(server.state->attempts[0].backend == "camoufox");
        CHECK(server.state->attempts[1].backend == "camoufox");
        CHECK(server.state->attempts[2].account_id == server.second_id);
    }

    SECTION("when both Firefox backends return 403, cools down account and rotates to next account") {
        FailoverServer server(3, [](std::string_view, const Config &) { return true; }, "windows-x64");
        const auto first = server.first_id;
        server.state->behavior = [first](std::string_view, const Account &account, const Request &, unsigned) {
            if (account.id == first)
                throw Error(403, "permission_denied", "403 on both Firefox backends");
        };

        const auto response = server.generate_chat();
        REQUIRE(response.result_int() == 200);
        {
            std::lock_guard lock(server.state->mutex);
            REQUIRE(server.state->attempts.size() == 4);
            CHECK(server.state->attempts[0].account_id == first);
            CHECK(server.state->attempts[0].backend == "camoufox");
            CHECK(server.state->attempts[1].account_id == first);
            CHECK(server.state->attempts[1].backend == "camoufox");
            CHECK(server.state->attempts[2].account_id == first);
            CHECK(server.state->attempts[2].backend == "spidermonkey");
            CHECK(server.state->attempts[3].account_id == server.second_id);
            CHECK(server.state->attempts[3].backend == "camoufox");
        }
        const auto statuses = server.router_store->account_status(server.config);
        const auto &failures = statuses[0].as_object().at("failures").as_array();
        REQUIRE_FALSE(failures.empty());
        CHECK(string_or(failures[0].as_object(), "kind") == "permission_temporary");
    }
#else
    SECTION("without SpiderMonkey compiled in, repeated 403 rotates accounts without backend switch") {
        FailoverServer server(3, [](std::string_view, const Config &) { return true; }, "windows-x64");
        const auto first = server.first_id;
        server.state->behavior = [first](std::string_view, const Account &account, const Request &, unsigned) {
            if (account.id == first)
                throw Error(403, "permission_denied", "Camoufox 403 without SpiderMonkey");
        };
        const auto response = server.generate_chat();
        REQUIRE(response.result_int() == 200);
        CHECK_FALSE(server.router_store->backend_switch(first).has_value());
        std::lock_guard lock(server.state->mutex);
        REQUIRE(server.state->attempts.size() == 3);
        CHECK(server.state->attempts[0].account_id == first);
        CHECK(server.state->attempts[1].account_id == first);
        CHECK(server.state->attempts[2].account_id == server.second_id);
    }
#endif
}
