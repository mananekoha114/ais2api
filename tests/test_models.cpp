// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend.hpp"
#include "ais2api/runtime.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <sqlite3.h>
#include <thread>
using namespace ais2api;
namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;
namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais-models-" + random_hex(8));
    ~Directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
Account account(std::string id, std::string index = "0") {
    Account value;
    value.id = std::move(id);
    value.email = value.id + "@example.com";
    value.auth_user = std::move(index);
    value.storage_state = {{"cookies", json::array{json::object{{"name", "SID"},
                                                                {"value", "synthetic"},
                                                                {"domain", ".google.com"},
                                                                {"path", "/"},
                                                                {"secure", true}}}}};
    return value;
}
json::array models(std::string id = "gemini-first") {
    return {json::object{{"name", "models/" + id},
                         {"displayName", "Synthetic model"},
                         {"supportedGenerationMethods", json::array{"generateContent"}},
                         {"source", "live"}}};
}
void sql(const fs::path &directory, const char *text) {
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open((directory / "state.sqlite3").string().c_str(), &db) == SQLITE_OK);
    const auto status = sqlite3_exec(db, text, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    REQUIRE(status == SQLITE_OK);
}
struct ModelState {
    std::atomic<unsigned> calls{0};
    std::atomic<int> mode{0};
    std::atomic<bool> slow{false};
};
class ModelBackend final : public GenerationBackend {
    std::shared_ptr<ModelState> state_;

  public:
    explicit ModelBackend(std::shared_ptr<ModelState> state) : state_(std::move(state)) {}
    json::array models(const Account &account, std::stop_token token) override {
        check_cancel(token);
        ++state_->calls;
        while (state_->slow) {
            check_cancel(token);
            std::this_thread::sleep_for(5ms);
        }
        const auto mode = state_->mode.load();
        if (mode == 1)
            throw Error(502, "network_failed", "Synthetic discovery failure");
        if (mode == 2)
            return {};
        if (mode == 3)
            return {json::object{{"name", "models/not-a-discovery"}, {"source", "configured"}}};
        return ::models("gemini-" + account.auth_user + (mode == 4 ? "-new" : "-old"));
    }
    void generate(const Request &, const Account &, const EventSink &, std::stop_token) override {}
    json::object probe(const Account &, std::stop_token) override { return {}; }
    json::object diagnostics() override { return {{"synthetic", true}}; }
    void close() override {}
};
struct ModelServer {
    Directory directory;
    Config config;
    std::shared_ptr<ModelState> state = std::make_shared<ModelState>();
    std::jthread thread;
    std::exception_ptr failure;
    ModelServer() {
        net::io_context io;
        tcp::acceptor port(io, {tcp::v4(), 0});
        config.port = port.local_endpoint().port();
        port.close();
        config.data_dir = directory.path / "data";
        config.config_file = directory.path / "config.json";
        config.admin_key = "synthetic-model-admin";
        config.api_key = "synthetic-model-api";
        config.configured_models = {"explicit-backup"};
        fs::create_directories(directory.path);
        atomic_write(config.config_file, json::serialize(config.to_json(true)), true);
    }
    void start() {
        failure = nullptr;
        thread = std::jthread([this] {
            try {
                serve_with_backend(config, std::make_unique<ModelBackend>(state));
            } catch (...) {
                failure = std::current_exception();
            }
        });
        for (const auto until = Clock::now() + 30s; Clock::now() < until;) {
            try {
                local_call(config, {{"command", "status"}});
                return;
            } catch (const Error &) {
            }
            std::this_thread::sleep_for(10ms);
        }
        stop();
        if (failure)
            std::rethrow_exception(failure);
        throw std::runtime_error("Model test service did not start");
    }
    void stop() {
        if (!thread.joinable())
            return;
        try {
            local_call(config, {{"command", "stop"}});
        } catch (...) {
        }
        thread.join();
    }
    ~ModelServer() {
        state->slow = false;
        stop();
    }
    http::response<http::string_body> call(std::string path, http::verb method = http::verb::get) {
        net::io_context io;
        tcp::socket socket(io);
        socket.connect({net::ip::make_address("127.0.0.1"), config.port});
        http::request<http::string_body> request(method, path, 11);
        request.set(http::field::host, "127.0.0.1");
        request.set(http::field::authorization,
                    "Bearer " +
                        (path.starts_with("/api/admin/") ? config.admin_key : config.api_key));
        request.prepare_payload();
        http::write(socket, request);
        boost::beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);
        return response;
    }
    json::object catalog() {
        const auto response = call("/api/admin/v1/models");
        REQUIRE(response.result_int() == 200);
        return json::parse(response.body()).as_object();
    }
    json::object refresh() {
        const auto response = call("/api/admin/v1/models/refresh", http::verb::post);
        REQUIRE(response.result_int() == 202);
        const auto id = string_or(json::parse(response.body()).as_object(), "operation_id");
        for (const auto until = Clock::now() + 5s; Clock::now() < until;) {
            auto result = local_call(config, {{"command", "operation.status"}, {"id", id}});
            const auto status = string_or(result, "status");
            if (status == "done" || status == "failed") {
                REQUIRE(call("/api/admin/v1/operations/" + id, http::verb::delete_).result_int() ==
                        200);
                return result;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("Model test refresh did not finish");
    }
};
} // namespace

TEST_CASE("model snapshots survive reopen and failed replacements without crossing accounts",
          "[models][store]") {
    Directory directory;
    {
        Store store(directory.path);
        store.put_account(account("a"));
        store.put_account(account("b", "1"));
        CHECK(store.model_catalog().models.empty());
        const auto saved = store.save_models("a", models());
        CHECK(saved.saved_at > 0);
        CHECK(string_or(saved.models[0].as_object(), "source") == "saved");
        for (auto invalid : std::vector<json::array>{
                 {},
                 {"bad"},
                 {json::object{{"name", "models/"}}},
                 {json::object{{"name", "models/a"}, {"source", "configured"}}},
                 {json::object{{"name", "models/a"}, {"description", 42}}},
                 {json::object{{"name", "models/a"}, {"id", 42}}},
                 {json::object{{"name", "models/a"}, {"inputTokenLimit", -1}}},
                 {json::object{{"name", "models/a"}}, json::object{{"name", "models/a"}}}}) {
            CHECK_THROWS_AS(store.save_models("a", std::move(invalid)), Error);
            CHECK(store.model_catalog().models == saved.models);
            CHECK(store.model_catalog().saved_at == saved.saved_at);
        }
        sql(directory.path, "CREATE TRIGGER fail_models BEFORE INSERT ON model_catalog BEGIN "
                            "SELECT RAISE(ABORT,'synthetic storage failure'); END");
        CHECK_THROWS_AS(store.save_models("a", models("gemini-replacement")), Error);
        CHECK(store.model_catalog().models == saved.models);
        sql(directory.path, "DROP TRIGGER fail_models");
        store.select_account("b");
        CHECK(store.model_catalog().account_id == "b");
        CHECK(store.model_catalog().models.empty());
        store.save_models("b", models("gemini-second"));
        CHECK(string_or(store.model_catalog().models[0].as_object(), "id") == "gemini-second");
        store.select_account("a");
        store.failure("a", "*", "login_expired");
        CHECK(store.model_catalog().models == saved.models);
    }
    {
        Store reopened(directory.path);
        CHECK(reopened.model_catalog().account_id == "a");
        CHECK(string_or(reopened.model_catalog().models[0].as_object(), "id") == "gemini-first");
        reopened.remove_account("a");
        CHECK(reopened.model_catalog().account_id == "b");
        CHECK(string_or(reopened.model_catalog().models[0].as_object(), "id") == "gemini-second");
        reopened.put_account(account("a"));
        reopened.select_account("a");
        CHECK(reopened.model_catalog().models.empty());
    }
}

TEST_CASE("old stores gain model snapshots without changing the database version",
          "[models][store]") {
    Directory directory;
    {
        Store store(directory.path);
        store.put_account(account("a"));
    }
    sql(directory.path, "DROP TABLE model_catalog");
    Store reopened(directory.path);
    CHECK_NOTHROW(reopened.save_models("a", models()));
    CHECK(reopened.model_catalog().models.size() == 1);
}

TEST_CASE(
    "model HTTP reads are local and successful manual discoveries persist across service restarts",
    "[models][http]") {
    ModelServer server;
    {
        Store store(server.config.data_dir);
        store.put_account(account("a"));
        store.put_account(account("b", "1"));
    }
    server.start();
    CHECK(string_or(server.catalog(), "source") == "configured");
    CHECK(server.catalog().at("saved_at").is_null());
    CHECK(server.call("/v1/models").result_int() == 200);
    CHECK(server.state->calls == 0);
    auto operation = server.refresh();
    REQUIRE(string_or(operation, "status") == "done");
    const auto saved = server.catalog();
    CHECK(string_or(saved, "source") == "saved");
    CHECK(saved.at("saved_at").as_int64() > 0);
    CHECK(server.state->calls == 1);
    CHECK(server.call("/v1/models/gemini-0-old").result_int() == 200);
    CHECK(server.call("/v1beta/models/gemini-0-old").result_int() == 200);
    const auto listed = server.call("/v1/models");
    CHECK(listed.body().find("gemini-0-old-search") != std::string::npos);
    CHECK(server.state->calls == 1);
    for (int mode : {1, 2, 3}) {
        server.state->mode = mode;
        CHECK(string_or(server.refresh(), "status") == "failed");
        CHECK(server.catalog() == saved);
    }
    server.state->mode = 4;
    sql(server.config.data_dir, "CREATE TRIGGER fail_models BEFORE INSERT ON model_catalog BEGIN "
                                "SELECT RAISE(ABORT,'synthetic storage failure'); END");
    CHECK(string_or(server.refresh(), "status") == "failed");
    CHECK(server.catalog() == saved);
    sql(server.config.data_dir, "DROP TRIGGER fail_models");
    CHECK(string_or(server.refresh(), "status") == "done");
    CHECK(server.call("/v1/models/gemini-0-old").result_int() == 404);
    CHECK(server.call("/v1beta/models/gemini-0-new").result_int() == 200);
    const auto calls = server.state->calls.load();
    server.stop();
    server.start();
    CHECK(server.call("/v1/models/gemini-0-new").result_int() == 200);
    CHECK(server.state->calls == calls);
    {
        Store store(server.config.data_dir);
        store.select_account("b");
    }
    CHECK(string_or(server.catalog(), "source") == "configured");
    CHECK(server.call("/v1/models/gemini-0-new").result_int() == 404);
    CHECK(string_or(server.refresh(), "status") == "done");
    CHECK(server.call("/v1/models/gemini-1-new").result_int() == 200);
    REQUIRE_FALSE(server.failure);
}

TEST_CASE("model discovery fails honestly without an account while configured reads remain local",
          "[models][http]") {
    ModelServer server;
    server.start();
    const auto original = server.catalog();
    CHECK(string_or(server.refresh(), "status") == "failed");
    CHECK(server.catalog() == original);
    CHECK(server.state->calls == 0);
    CHECK(server.call("/v1/models/explicit-backup").result_int() == 200);
    auto patched = local_call(server.config, {{"command", "config.patch"},
                                              {"patch", {{"configured_models", json::array{}}}}});
    REQUIRE(bool_or(patched, "applied"));
    CHECK(string_or(server.catalog(), "source") == "none");
    CHECK(server.catalog().at("models").as_array().empty());
    CHECK(server.state->calls == 0);
}

TEST_CASE("saved model reads remain available while a browser discovery is busy",
          "[models][http]") {
    ModelServer server;
    {
        Store store(server.config.data_dir);
        store.put_account(account("a"));
        store.save_models("a", models());
    }
    server.state->slow = true;
    server.start();
    const auto response = server.call("/api/admin/v1/models/refresh", http::verb::post);
    REQUIRE(response.result_int() == 202);
    for (const auto until = Clock::now() + 5s; server.state->calls == 0 && Clock::now() < until;)
        std::this_thread::sleep_for(5ms);
    REQUIRE(server.state->calls == 1);
    // Bound a regression's wait instead of leaving the fixture blocked forever.
    std::jthread release([state = server.state] {
        std::this_thread::sleep_for(1s);
        state->slow = false;
    });
    const auto started = Clock::now();
    CHECK(server.call("/v1/models/gemini-first").result_int() == 200);
    CHECK(Clock::now() - started < 500ms);
    CHECK(string_or(server.catalog(), "source") == "saved");
    CHECK(server.state->slow);
}
