// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/backend.hpp"
#include "ais2api/region.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/store.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <thread>
using namespace ais2api;
namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;
namespace {
class AdminBackend final : public GenerationBackend {
 public:
    void generate(const Request&, const Account&, const EventSink&, std::stop_token) override {}
    json::array models(const Account &, std::stop_token = {}) override { return {}; }
    json::object probe(const Account&, std::stop_token = {}) override { return {{"ok", true}}; }
    json::object diagnostics() override { return {{"synthetic", true}}; }
    void close() override {}
};
struct AdminServer {
    fs::path directory = fs::temp_directory_path() / ("ais2api-admin-http-" + random_hex(8));
    Config config;
    std::string admin = "synthetic-admin-" + random_hex(12);
    std::string api = "synthetic-api-" + random_hex(12);
    std::string importer = "synthetic-import-" + random_hex(12);
    std::jthread thread;
    std::exception_ptr failure;
    explicit AdminServer(bool enabled = true) {
        net::io_context io; tcp::acceptor port(io, {tcp::v4(), 0});
        config.port = port.local_endpoint().port(); port.close();
        config.data_dir = directory / "data"; config.config_file = directory / "config.json";
        config.api_key = api; config.admin_key = enabled ? admin : "";
        config.cookie_import_key = importer; config.configured_models = {"synthetic"};
        config.admin_origins = {"https://allowed.example"};
        fs::create_directories(directory);
        atomic_write(config.config_file, json::serialize(config.to_json(true)), true);
        thread = std::jthread([this] {
            try { serve_with_backend(config, std::make_unique<AdminBackend>()); }
            catch (...) { failure = std::current_exception(); }
        });
        bool ready = false;
        // Sanitizer builds on shared runners can take seconds to start.
        for (const auto deadline = Clock::now() + 30s; Clock::now() < deadline;) {
            try { local_call(config, {{"command", "status"}}); ready = true; break; }
            catch (...) { std::this_thread::sleep_for(20ms); }
        }
        if (!ready) {
            // A late start would otherwise block the join forever.
            try { local_call(config, {{"command", "stop"}}); } catch (...) {}
            if (thread.joinable()) thread.join();
            if (failure) std::rethrow_exception(failure);
            throw std::runtime_error("Admin test server failed to start");
        }
    }
    ~AdminServer() {
        try { local_call(config, {{"command", "stop"}}); } catch (...) {}
        if (thread.joinable()) thread.join();
        std::error_code ec; fs::remove_all(directory, ec);
    }
    http::response<http::string_body> call(http::verb method, std::string path,
        std::string body = {}, std::string token = {}, std::string origin = {}) {
        net::io_context io; tcp::socket socket(io);
        socket.connect({net::ip::make_address("127.0.0.1"), config.port});
        http::request<http::string_body> request(method, path, 11);
        request.set(http::field::host, "127.0.0.1:" + std::to_string(config.port));
        request.set(http::field::content_type, "application/json");
        if (!token.empty()) request.set(http::field::authorization, "Bearer " + token);
        if (!origin.empty()) request.set(http::field::origin, origin);
        request.body() = std::move(body); request.prepare_payload(); http::write(socket, request);
        boost::beast::flat_buffer input; http::response<http::string_body> response;
        http::read(socket, input, response); return response;
    }
    http::response<http::string_body> get(std::string route) {
        return call(http::verb::get, "/api/admin/v1" + route, {}, admin);
    }
    http::response<http::string_body> send(http::verb method, std::string route, json::object body = {}) {
        return call(method, "/api/admin/v1" + route, json::serialize(body), admin);
    }
    bool drain_logs() {
        for (int i = 0; i < 200; ++i) {
            auto status = local_call(config, {{"command", "status"}});
            auto pending = status.if_contains("pending_logs");
            if (pending && ((pending->is_uint64() && pending->as_uint64() == 0) ||
                            (pending->is_int64() && pending->as_int64() == 0))) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }
};
json::object object(const http::response<http::string_body>& response) { return json::parse(response.body()).as_object(); }
struct AdminStream {
    net::io_context io;
    tcp::socket socket{io};
    std::string received;
    bool ended = false;
    AdminStream(AdminServer& server, std::string query = {}, std::string resume = {}) {
        socket.connect({net::ip::make_address("127.0.0.1"), server.config.port});
        http::request<http::empty_body> request(http::verb::get, "/api/admin/v1/logs/stream" + query, 11);
        request.set(http::field::host, "127.0.0.1");
        request.set(http::field::authorization, "Bearer " + server.admin);
        if (!resume.empty()) request.set("Last-Event-ID", resume);
        http::write(socket, request); socket.non_blocking(true);
    }
    bool until(std::string_view marker, std::chrono::milliseconds timeout = 4s) {
        auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (received.find(marker) != std::string::npos) return true;
            std::array<char, 8192> bytes{}; boost::system::error_code ec;
            auto n = socket.read_some(net::buffer(bytes), ec); received.append(bytes.data(), n);
            if (ec == net::error::eof || ec == net::error::connection_reset) { ended = true; break; }
            if (ec && ec != net::error::would_block && ec != net::error::try_again) break;
            std::this_thread::sleep_for(10ms);
        }
        return received.find(marker) != std::string::npos;
    }
};
}
TEST_CASE("management HTTP authentication origins and disabled configuration", "[http][admin]") {
    AdminServer server;
    for (const auto& key : {std::string{}, server.api, server.importer})
        CHECK(server.call(http::verb::get, "/api/admin/v1/status", {}, key).result_int() == 401);
    CHECK(server.get("/status").result_int() == 200);
    auto diagnostics = server.get("/diagnostics");
    REQUIRE(diagnostics.result_int() == 200);
    CHECK(object(diagnostics).at("browser").as_object().at("synthetic") == true);
    CHECK(server.call(http::verb::get, "/v1/models", {}, server.admin).result_int() == 401);
    CHECK(server.call(http::verb::get, "/api/admin/v1/status", {}, server.admin, "https://denied.example").result_int() == 403);
    auto allowed = server.call(http::verb::get, "/api/admin/v1/status", {}, server.admin, "https://allowed.example");
    CHECK(allowed.result_int() == 200);
    CHECK(allowed[http::field::access_control_allow_origin] == "https://allowed.example");
    auto options = server.call(http::verb::options, "/api/admin/v1/config", {}, {}, "https://allowed.example");
    CHECK(options.result_int() == 204);
    CHECK(options[http::field::access_control_allow_headers].find("Last-Event-ID") != std::string_view::npos);
    CHECK(server.call(http::verb::options, "/api/admin/v1/config", {}, {}, "https://denied.example").result_int() == 403);
    AdminServer disabled(false);
    CHECK(disabled.get("/status").result_int() == 403);
    CHECK(disabled.call(http::verb::options, "/api/admin/v1/status").result_int() == 403);
}
TEST_CASE("management HTTP config and keys validate redact apply and persist", "[http][admin]") {
    AdminServer server;
    auto shown = server.get("/config"); REQUIRE(shown.result_int() == 200);
    CHECK(shown.body().find(server.admin) == std::string::npos);
    CHECK(shown.body().find(server.api) == std::string::npos);
    CHECK(shown.body().find(server.importer) == std::string::npos);
    auto before = read_file(server.config.config_file);
    CHECK(server.send(http::verb::post, "/config/validate", {{"headless", false}}).result_int() == 200);
    CHECK(read_file(server.config.config_file) == before);
    CHECK(server.send(http::verb::patch, "/config", {{"unknown", true}}).result_int() == 400);
    CHECK(read_file(server.config.config_file) == before);
    REQUIRE(server.send(http::verb::patch, "/config", {{"limits", json::object{{"queue_size", 12}}}}).result_int() == 200);
    const auto saved_text = read_file(server.config.config_file);
    CHECK(saved_text.starts_with("{\n  \"version\": 1,\n"));
    CHECK(saved_text.find("\n    \"queue_size\": 12,") != std::string::npos);
    CHECK(Config::load(server.config.config_file).limits.queue_size == 12);
    const auto replacement = "synthetic-rotated-api-" + random_hex(12);
    REQUIRE(server.send(http::verb::put, "/keys/config", {{"api_key", replacement}}).result_int() == 200);
    CHECK(server.call(http::verb::get, "/v1/models", {}, server.api).result_int() == 401);
    CHECK(server.call(http::verb::get, "/v1/models", {}, replacement).result_int() == 200);
    CHECK(server.get("/keys").body().find(replacement) == std::string::npos);
    auto restart = object(server.send(http::verb::patch, "/config", {{"port", server.config.port == 19000 ? 19001 : 19000}}));
    CHECK(bool_or(restart, "saved")); CHECK_FALSE(bool_or(restart, "applied")); CHECK(bool_or(restart, "restart_required"));
    CHECK(server.get("/status").result_int() == 200);
    CHECK(server.send(http::verb::post, "/config/reload").result_int() == 409);
    auto second = server.send(http::verb::patch, "/config", {{"limits", json::object{{"queue_size", 13}}}});
    REQUIRE(second.result_int() == 200);
    CHECK_FALSE(bool_or(object(second), "applied"));
    auto saved = Config::load(server.config.config_file);
    CHECK(saved.port == (server.config.port == 19000 ? 19001 : 19000));
    CHECK(saved.limits.queue_size == 13);
    auto visible = object(server.get("/config"));
    REQUIRE(visible.at("saved_config").is_object());
    CHECK(visible.at("saved_config").as_object().at("port") == saved.port);
    CHECK(visible.at("saved_config").as_object().at("limits").as_object().at("queue_size") == 13);
    CHECK(visible.at("config").as_object().at("limits").as_object().at("queue_size") == 12);
}
TEST_CASE("management HTTP accounts expose summaries and queued probes", "[http][admin]") {
    AdminServer server;
    json::object state{{"cookies", json::array{json::object{{"name", "SID"}, {"value", "synthetic-cookie"}, {"domain", ".google.com"}, {"path", "/"}}}}, {"origins", json::array{}}};
    auto imported = server.send(http::verb::post, "/accounts/import", {{"input", state}, {"name", "Synthetic admin account"}});
    REQUIRE(imported.result_int() == 200);
    auto accounts = object(server.get("/accounts")).at("accounts").as_array(); REQUIRE(accounts.size() == 1);
    CHECK(json::serialize(accounts).find("synthetic-cookie") == std::string::npos);
    auto id = string_or(accounts[0].as_object(), "id"); REQUIRE_FALSE(id.empty());
    REQUIRE(server.send(http::verb::patch, "/accounts/" + id, {{"name", "Renamed"}, {"disabled", false}}).result_int() == 200);
    CHECK(server.send(http::verb::post, "/accounts/" + id + "/activate").result_int() == 200);
    auto probe = server.send(http::verb::post, "/accounts/" + id + "/probe"); REQUIRE(probe.result_int() == 202);
    auto operation = string_or(object(probe), "operation_id"); REQUIRE_FALSE(operation.empty());
    std::string status;
    for (int i = 0; i < 100; ++i) {
        status = string_or(object(server.get("/operations/" + operation)), "status");
        if (status != "queued") break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(status == "done");
    CHECK(server.send(http::verb::delete_, "/operations/" + operation).result_int() == 200);
    CHECK(server.get("/operations/" + operation).result_int() == 404);
    CHECK(server.get("/operations/missing").result_int() == 404);
    CHECK(server.send(http::verb::delete_, "/accounts/" + id).result_int() == 200);
    CHECK(object(server.get("/accounts")).at("accounts").as_array().empty());
}
TEST_CASE("management HTTP log pages SSE resumption raw sources and rotation", "[http][admin]") {
    AdminServer server;
    server.get("/status"); server.get("/status");
    REQUIRE(server.drain_logs());
    auto page = object(server.get("/logs?limit=1"));
    REQUIRE(page.at("entries").as_array().size() == 1); CHECK(bool_or(page, "has_more"));
    auto cursor = string_or(page, "next_cursor");
    auto next = object(server.get("/logs?limit=1&cursor=" + cursor));
    REQUIRE(next.at("entries").as_array().size() == 1);
    CHECK(string_or(next.at("entries").as_array()[0].as_object(), "cursor") == cursor);
    CHECK(server.get("/logs?cursor=1").result_int() == 400);
    CHECK(server.get("/logs?limit=501").result_int() == 400);
    AdminStream history(server); REQUIRE(history.until("event: logs"));
    auto data = history.received.find("data: "); REQUIRE(data != std::string::npos);
    REQUIRE(history.until("\n\n"));
    auto end = history.received.find('\n', data);
    auto event = json::parse(history.received.substr(data + 6, end - data - 6)).as_object();
    auto resume = string_or(event, "next_cursor"); REQUIRE_FALSE(resume.empty());
    server.send(http::verb::patch, "/config", {{"limits", json::object{{"queue_size", 11}}}});
    REQUIRE(history.until("config_saved"));
    AdminStream resumed(server, {}, resume); REQUIRE(resumed.until("config_saved")); REQUIRE(resumed.until("\n\n"));
    auto resumed_data = resumed.received.find("data: "); REQUIRE(resumed_data != std::string::npos);
    auto resumed_end = resumed.received.find('\n', resumed_data);
    auto resumed_event = json::parse(resumed.received.substr(resumed_data + 6, resumed_end - resumed_data - 6)).as_object();
    CHECK(string_or(resumed_event.at("entries").as_array()[0].as_object(), "cursor") == resume);
    atomic_write(server.config.data_dir / "service.log", "", true);
    CHECK(server.get("/logs/sources").result_int() == 200);
    auto empty = object(server.get("/logs?source=service"));
    CHECK(string_or(empty, "data").empty()); CHECK(string_or(empty, "next_cursor") == "0");
    AdminStream raw(server, "?source=service"); REQUIRE(raw.until("text/event-stream"));
    atomic_write(server.config.data_dir / "service.log", "synthetic raw line\n", true);
    REQUIRE(raw.until("event: logs"));
    CHECK(string_or(object(server.get("/logs?source=service")), "data") == "c3ludGhldGljIHJhdyBsaW5lCg==");
    CHECK(server.get("/logs?source=..%2Fconfig.json").result_int() == 404);
    const auto rotated = "synthetic-rotated-admin-" + random_hex(12);
    REQUIRE(server.send(http::verb::patch, "/config", {{"admin_key", rotated}}).result_int() == 200);
    CHECK(server.get("/status").result_int() == 401);
    history.until("__never_present__", 2s); CHECK(history.ended);
    resumed.until("__never_present__", 2s); CHECK(resumed.ended);
    raw.until("__never_present__", 2s); CHECK(raw.ended);
    server.admin = rotated; CHECK(server.get("/status").result_int() == 200);
}
TEST_CASE("reading model lists does not consume maintenance operation slots", "[http][admin]") {
    AdminServer server;
    for (int i = 0; i < 40; ++i) {
        INFO("model request index " << i);
        REQUIRE(server.call(http::verb::get, "/v1/models", {}, server.api).result_int() == 200);
    }
    auto probe = server.send(http::verb::post, "/accounts/probe");
    CHECK(probe.result_int() == 202);
    CHECK_FALSE(string_or(object(probe), "operation_id").empty());
}
TEST_CASE("journal persistence failure preserves committed config changes", "[http][admin]") {
    AdminServer server;
    REQUIRE(server.drain_logs());
    fs::remove(server.config.data_dir / "management.log");
    fs::create_directory(server.config.data_dir / "management.log");
    auto patched = server.send(http::verb::patch, "/config", {{"limits", json::object{{"queue_size", 17}}}});
    REQUIRE(patched.result_int() == 200);
    CHECK(bool_or(object(patched), "saved"));
    CHECK(bool_or(object(patched), "applied"));
    CHECK(Config::load(server.config.config_file).limits.queue_size == 17);
    REQUIRE(server.drain_logs());
    auto status = object(server.get("/status"));
    CHECK_FALSE(bool_or(status, "logging_healthy", true));
    CHECK(object(server.get("/config")).at("config").as_object().at("limits").as_object().at("queue_size") == 17);
}
TEST_CASE("an unread management SSE stream does not block ordinary requests", "[http][admin]") {
    AdminServer server;
    REQUIRE(server.drain_logs());
    auto baseline = object(server.get("/status")).at("connections");
    {
        AdminStream unread(server);
        // Leave response bytes unread while the service continues accepting work.
        for (int i = 0; i < 10; ++i) CHECK(server.get("/status").result_int() == 200);
    }
    // The next heartbeat/write observes the disconnect and releases the slot.
    auto until = Clock::now() + 7s;
    bool reclaimed = false;
    while (Clock::now() < until) {
        auto connections = object(server.get("/status")).at("connections");
        if (connections == baseline) { reclaimed = true; break; }
        std::this_thread::sleep_for(50ms);
    }
    CHECK(reclaimed);
}
TEST_CASE("managed API keys support CRUD independent of management and configured keys", "[http][admin]") {
    AdminServer server;
    std::string legacy;
    { Store store(server.config.data_dir); legacy = string_or(store.create_key("Legacy synthetic"), "secret"); }
    REQUIRE_FALSE(legacy.empty());
    auto created = server.send(http::verb::post, "/keys", {{"name", "Synthetic first"}});
    REQUIRE(created.result_int() == 201);
    auto key = object(created);
    auto id = string_or(key, "id"), secret = string_or(key, "secret");
    REQUIRE_FALSE(id.empty()); REQUIRE_FALSE(secret.empty());
    CHECK(key.contains("created"));
    CHECK(server.call(http::verb::get, "/v1/models", {}, secret).result_int() == 200);
    CHECK(server.call(http::verb::get, "/api/admin/v1/status", {}, secret).result_int() == 401);
    CHECK(server.call(http::verb::post, "/api/admin/v1/accounts/probe-import", R"({"cookies":"SID=synthetic"})", secret).result_int() == 401);
    CHECK(server.call(http::verb::get, "/v1/models", {}, legacy).result_int() == 401);
    CHECK(server.send(http::verb::patch, "/keys/" + id, {{"name", "Synthetic renamed"}}).result_int() == 200);
    auto listed = server.get("/keys"); REQUIRE(listed.result_int() == 200);
    CHECK(listed.body().find(secret) == std::string::npos);
    CHECK(listed.body().find(legacy) == std::string::npos);
    CHECK(listed.body().find("Synthetic renamed") != std::string::npos);
    const auto listed_object = object(listed);
    for (const auto& item : listed_object.at("keys").as_array()) CHECK_FALSE(item.as_object().contains("secret"));
    auto second_response = server.send(http::verb::post, "/keys", {{"name", "Synthetic second"}});
    REQUIRE(second_response.result_int() == 201);
    auto second = object(second_response);
    auto second_id = string_or(second, "id"), second_secret = string_or(second, "secret");
    REQUIRE(server.send(http::verb::delete_, "/keys/config").result_int() == 200);
    CHECK(Config::load(server.config.config_file).api_key.empty());
    CHECK(server.call(http::verb::get, "/v1/models", {}, server.api).result_int() == 401);
    CHECK(server.call(http::verb::get, "/v1/models", {}, legacy).result_int() == 401);
    CHECK(server.call(http::verb::get, "/v1/models", {}, secret).result_int() == 200);
    CHECK(server.call(http::verb::get, "/v1/models", {}, second_secret).result_int() == 200);
    REQUIRE(server.send(http::verb::delete_, "/keys/" + id).result_int() == 200);
    CHECK(server.call(http::verb::get, "/v1/models", {}, secret).result_int() == 401);
    CHECK(server.call(http::verb::get, "/v1/models", {}, second_secret).result_int() == 200);
    CHECK(server.send(http::verb::patch, "/keys/missing", {{"name", "Missing"}}).result_int() == 404);
    CHECK(server.send(http::verb::delete_, "/keys/" + second_id).result_int() == 200);
    CHECK(server.call(http::verb::get, "/v1/models", {}, second_secret).result_int() == 401);
    CHECK(server.get("/status").result_int() == 200);
}
TEST_CASE("management key lists track configured and managed key availability", "[http][admin]") {
    AdminServer server;
    auto check_list = [&](bool config_key, std::size_t count) {
        auto response = server.get("/keys");
        REQUIRE(response.result_int() == 200);
        const auto listed = object(response);
        CHECK(string_or(listed, "source") == "config_and_managed");
        CHECK(listed.at("config_key_configured").as_bool() == config_key);
        CHECK(listed.at("configured").as_bool() == (count != 0));
        CHECK(listed.at("keys").as_array().size() == count);
        CHECK(response.body().find(server.api) == std::string::npos);
        for (const auto& key : listed.at("keys").as_array())
            CHECK_FALSE(key.as_object().contains("secret"));
    };
    check_list(true, 1);
    auto created = server.send(http::verb::post, "/keys", {{"name", "Synthetic list regression"}});
    REQUIRE(created.result_int() == 201);
    auto id = string_or(object(created), "id");
    REQUIRE_FALSE(id.empty());
    check_list(true, 2);
    REQUIRE(server.send(http::verb::delete_, "/keys/config").result_int() == 200);
    check_list(false, 1);
    REQUIRE(server.send(http::verb::delete_, "/keys/" + id).result_int() == 200);
    check_list(false, 0);
}
TEST_CASE("management probe import enforces admin authentication and verified identity", "[http][admin]") {
    AdminServer server;
    const std::string body = R"({"cookies":"SID=synthetic-unverified","auth_user":"0"})";
    for (const auto& key : {server.api, server.importer})
        CHECK(server.call(http::verb::post, "/api/admin/v1/accounts/probe-import", body, key).result_int() == 401);
    CHECK(server.call(http::verb::post, "/api/admin/v1/accounts/probe-import", body, server.admin, "https://denied.example").result_int() == 403);
    auto failure = server.call(http::verb::post, "/api/admin/v1/accounts/probe-import", body, server.admin, "https://allowed.example");
    CHECK(failure.result_int() == 409);
    CHECK(failure.body().find("synthetic-unverified") == std::string::npos);
    CHECK(object(server.get("/accounts")).at("accounts").as_array().empty());
}
TEST_CASE("the default management password only permits replacing itself", "[http][admin]") {
    AdminServer server;
    server.config.admin_key = std::string(default_admin_key);
    atomic_write(server.config.config_file, json::serialize(server.config.to_json(true)), true);
    local_call(server.config, {{"command", "config.reload"}});
    const std::string initial(default_admin_key);
    auto check = server.call(http::verb::get, "/api/admin/v1/auth/check", {}, initial);
    REQUIRE(check.result_int() == 200);
    CHECK(object(check).at("password_change_required") == true);
    for (const auto* route : {"/api/admin/v1/status", "/api/admin/v1/config", "/api/admin/v1/logs",
                              "/api/admin/v1/logs/stream", "/api/admin/v1/keys"}) {
        auto blocked = server.call(http::verb::get, route, {}, initial);
        CHECK(blocked.result_int() == 403);
        CHECK(blocked.body().find("admin_password_change_required") != std::string::npos);
    }
    CHECK(server.call(http::verb::patch, "/api/admin/v1/config", R"({"admin_key":"other-password"})", initial).result_int() == 403);
    CHECK(server.call(http::verb::post, "/api/admin/v1/accounts/probe-import",
                      R"({"cookies":"SID=synthetic","auth_user":"0"})", initial).result_int() == 403);
    CHECK(server.call(http::verb::get, "/api/admin/v1/auth/check", {}, "wrong-password").result_int() == 401);
    for (const auto* body : {R"({"password":"short"})", R"({"password":"admin"})", R"({"password":7})",
                             R"({"password":"long-enough-1","extra":true})", R"({})"})
        CHECK(server.call(http::verb::post, "/api/admin/v1/auth/password", body, initial).result_int() == 400);
    CHECK(Config::load(server.config.config_file).admin_key_is_default());

    const std::string replacement = "synthetic-replacement-" + random_hex(8);
    auto changed = server.call(http::verb::post, "/api/admin/v1/auth/password",
                               json::serialize(json::object{{"password", replacement}}), initial);
    REQUIRE(changed.result_int() == 200);
    CHECK(object(changed).at("changed") == true);
    CHECK(server.call(http::verb::get, "/api/admin/v1/auth/check", {}, initial).result_int() == 401);
    auto after = server.call(http::verb::get, "/api/admin/v1/auth/check", {}, replacement);
    REQUIRE(after.result_int() == 200);
    CHECK(object(after).at("password_change_required") == false);
    CHECK(server.call(http::verb::get, "/api/admin/v1/status", {}, replacement).result_int() == 200);
    CHECK(Config::load(server.config.config_file).admin_key == replacement);
    auto reuse = server.call(http::verb::patch, "/api/admin/v1/config", R"({"admin_key":"admin"})", replacement);
    CHECK(reuse.result_int() == 400);
    CHECK(reuse.body().find("admin_password_invalid") != std::string::npos);
}
TEST_CASE("management password changes apply while restart-only changes are pending", "[http][admin]") {
    AdminServer server;
    const auto port = server.config.port == 19000 ? 19001 : 19000;
    auto pending = object(server.send(http::verb::patch, "/config", {{"port", port}}));
    REQUIRE(bool_or(pending, "restart_required"));
    const std::string replacement = "synthetic-pending-" + random_hex(8);
    REQUIRE(server.send(http::verb::post, "/auth/password", {{"password", replacement}}).result_int() == 200);
    CHECK(server.call(http::verb::get, "/api/admin/v1/status", {}, server.admin).result_int() == 401);
    auto view = server.call(http::verb::get, "/api/admin/v1/config", {}, replacement);
    REQUIRE(view.result_int() == 200);
    CHECK(object(view).at("restart_required") == true);
    auto saved = Config::load(server.config.config_file);
    CHECK(saved.admin_key == replacement);
    CHECK(saved.port == port);
}
TEST_CASE("management HTTP exposes and edits per-account browser fingerprints", "[http][admin][fingerprint]") {
    AdminServer server;
    auto imported = server.send(http::verb::post, "/accounts/import", {{"input", "SAPISID=synthetic-fingerprint"}});
    REQUIRE(imported.result_int() == 200);
    const auto id = string_or(object(imported).at("accounts").as_array()[0].as_object(), "id");
    const auto route = "/accounts/" + id + "/fingerprint";

    auto shown = server.get(route);
    REQUIRE(shown.result_int() == 200);
    auto view = object(shown);
    const auto seed = view.at("seed").to_number<unsigned>();
    CHECK(seed >= 10000);
    CHECK(string_or(view, "platform") == fingerprint_platform());
    CHECK(string_or(view, "timezone").empty());
    CHECK(string_or(view, "effective_timezone") == host_timezone());
    CHECK(string_or(view, "effective_locale") == host_locale());
    auto listed = object(server.get("/accounts")).at("accounts").as_array()[0].as_object();
    CHECK(listed.at("fingerprint").as_object().at("seed").to_number<unsigned>() == seed);

    auto updated = server.send(http::verb::patch, route, {{"timezone", "America/New_York"}, {"locale", "en-US"}});
    REQUIRE(updated.result_int() == 200);
    CHECK(string_or(object(updated), "effective_timezone") == "America/New_York");
    CHECK(object(updated).at("seed").to_number<unsigned>() == seed);
    auto reset = object(server.send(http::verb::patch, route, {{"timezone", nullptr}}));
    CHECK(string_or(reset, "timezone").empty());
    CHECK(string_or(reset, "locale") == "en-US");
    for (const auto& body : std::vector<json::object>{{{"timezone", "Bad Zone"}}, {{"locale", "en_US"}},
                                                     {{"seed", 12345}}, {{"timezone", 7}}, {}})
        CHECK(server.send(http::verb::patch, route, body).result_int() == 400);

    auto regenerated = server.send(http::verb::post, route + "/regenerate");
    REQUIRE(regenerated.result_int() == 200);
    CHECK(object(regenerated).at("seed").to_number<unsigned>() != seed);
    CHECK(string_or(object(regenerated), "locale") == "en-US");
    CHECK(server.get("/accounts/acc_missing/fingerprint").result_int() == 404);
    CHECK(server.send(http::verb::post, "/accounts/acc_missing/fingerprint/regenerate").result_int() == 404);
}
