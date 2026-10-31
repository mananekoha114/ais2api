// Copyright (c) 2026 Mana Nekoha (mananekoha114)
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
#include <future>
#include <thread>
using namespace ais2api;
namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;
namespace {
struct PushState {
    std::atomic<unsigned> probes{0};
    std::atomic<bool> slow{false}, cancelled{false};
    std::atomic<int> failure{0};
    std::atomic<int> delay_ms{0};
};
class PushBackend final : public GenerationBackend {
    std::shared_ptr<PushState> state;
public:
    explicit PushBackend(std::shared_ptr<PushState> s) : state(std::move(s)) {}
    void generate(const Request &, const Account &, const EventSink &, std::stop_token t) override { check_cancel(t); }
    json::array models(const Account &, std::stop_token = {}) override { return {}; }
    json::object probe(const Account &, std::stop_token = {}) override { return {}; }
    json::object probe_import(Account &a, std::stop_token token = {}) override {
        ++state->probes;
        while (state->slow) {
            if (token.stop_requested()) { state->cancelled = true; check_cancel(token); }
            std::this_thread::sleep_for(5ms);
        }
        for (const auto until = Clock::now() + std::chrono::milliseconds(state->delay_ms.load());
             Clock::now() < until && !token.stop_requested();)
            std::this_thread::sleep_for(5ms);
        check_cancel(token);
        if (state->failure == 1) throw Error(503, "transport", "secret-cookie-http");
        if (state->failure == 2) throw Error(409, "identity_unverified", "secret-cookie-http");
        if (state->failure == 3) throw Error(403, "region_blocked", "secret-cookie-http");
        if (state->failure == 4) throw Error(502, "browser_incompatible", "secret-cookie-http");
        if (state->failure == 5) throw Error(504, "browser_timeout", "secret-cookie-http");
        if (a.auth_user != "0" && a.auth_user != "4") throw Error(401, "login", "secret-cookie-http");
        return {{"ready", true}, {"login", false}, {"identityAmbiguous", false}, {"email", "http@example.com"}};
    }
    json::object diagnostics() override { return {}; }
    void close() override {}
};
struct PushServer {
    fs::path directory = fs::temp_directory_path() / ("ais-cookie-http-" + random_hex(8));
    Config config;
    std::shared_ptr<PushState> state = std::make_shared<PushState>();
    std::exception_ptr failure;
    std::jthread thread;
    explicit PushServer(bool enabled = true, unsigned timeout = 15, std::size_t capacity = 8) {
        net::io_context io; tcp::acceptor port(io, {tcp::v4(), 0});
        config.port = port.local_endpoint().port(); port.close();
        config.data_dir = directory / "data"; config.config_file = directory / "config.json";
        config.api_key = "generation-key"; config.cookie_import_key = enabled ? "import-key" : "";
        config.cookie_import_timeout_seconds = timeout; config.limits.queue_size = capacity;
        config.configured_models = {"synthetic"};
        fs::create_directories(directory);
        atomic_write(config.config_file, json::serialize(config.to_json()), true);
        thread = std::jthread([this] { try { serve_with_backend(config, std::make_unique<PushBackend>(state)); }
                                      catch (...) { failure = std::current_exception(); } });
        // Sanitizer builds on shared runners can take seconds to start.
        for (const auto deadline = Clock::now() + 30s; Clock::now() < deadline;) {
            try { local_call(config, {{"command", "status"}}); return; } catch (const Error &) {}
            std::this_thread::sleep_for(20ms);
        }
        // A late start would otherwise block the jthread join forever.
        try { local_call(config, {{"command", "stop"}}); } catch (...) {}
        throw std::runtime_error("Cookie HTTP test service did not start");
    }
    ~PushServer() {
        state->slow = false;
        try { local_call(config, {{"command", "stop"}}); } catch (...) {}
        if (thread.joinable()) thread.join();
        std::error_code ec; fs::remove_all(directory, ec);
    }
    http::request<http::string_body> request(std::string body, std::string key = "import-key",
                                           std::string origin = "https://aistudio.google.com") {
        http::request<http::string_body> r(http::verb::post, "/api/accounts/probe-import", 11);
        r.set(http::field::host, "127.0.0.1"); r.set(http::field::content_type, "application/json");
        if (!key.empty()) r.set(http::field::authorization, "Bearer " + key);
        if (!origin.empty()) r.set(http::field::origin, origin);
        r.body() = std::move(body); r.prepare_payload(); return r;
    }
    http::response<http::string_body> exchange(http::request<http::string_body> r) {
        net::io_context io; boost::beast::tcp_stream socket(io);
        socket.expires_after(10s); socket.connect({net::ip::make_address("127.0.0.1"), config.port});
        http::write(socket, r); boost::beast::flat_buffer b; http::response<http::string_body> response;
        http::read(socket, b, response); return response;
    }
    http::response<http::string_body> push(std::string body = R"({"cookies":"SID=secret-cookie-http"})") { return exchange(request(std::move(body))); }
    json::array accounts() { return local_call(config, {{"command", "accounts.list"}}).at("accounts").as_array(); }
    bool wait_probes(unsigned n) {
        for (unsigned i = 0; i < 200; ++i) { if (state->probes >= n) return true; std::this_thread::sleep_for(5ms); }
        return false;
    }
};
}
TEST_CASE("cookie HTTP import key is isolated and revocable", "[http][cookie-import]") {
    PushServer s;
    for (auto key : {"", "wrong-key", "generation-key"})
        CHECK(s.exchange(s.request(R"({"cookies":"SID=secret-cookie-http"})", key)).result_int() == 401);
    CHECK(s.exchange(s.request(R"({"cookies":"SID=secret-cookie-http"})", "import-key", "https://evil.example")).result_int() == 403);
    auto get = s.request(""); get.method(http::verb::get); get.target("/api/accounts");
    CHECK(s.exchange(get).result_int() == 404);
    get.target("/v1/models"); get.set(http::field::authorization, "Bearer generation-key");
    CHECK(s.exchange(get).result_int() == 200);
    auto config = s.config.to_json(); config["cookie_import_key"] = "";
    atomic_write(s.config.config_file, json::serialize(config), true);
    local_call(s.config, {{"command", "config.reload"}});
    auto disabled = s.push(); CHECK(disabled.result_int() == 403);
    CHECK(disabled.body().find("cookie_import_disabled") != std::string::npos);
    CHECK(s.exchange(get).result_int() == 200);
}
TEST_CASE("cookie HTTP accepts legacy and structured input only after verification", "[http][cookie-import]") {
    PushServer s;
    auto first = s.push(); REQUIRE(first.result_int() == 200);
    auto body = json::parse(first.body()).as_object();
    CHECK(body.at("imported_count") == 1);
    CHECK(body.at("verification") == "browser_identity_only");
    CHECK(body.at("generation_verified") == false);
    CHECK(first.body().find("secret-cookie-http") == std::string::npos);
    // Index 0 verifies; indices 1-3 are unauthenticated, so the scan stops after
    // three consecutive failures.
    CHECK(s.state->probes == 4);
    auto id = body.at("accounts").as_array()[0].as_object().at("id");
    auto second = s.push(R"({"cookies":[{"name":"SID","value":"secret-cookie-http","domain":".google.com","path":"/","secure":true,"httpOnly":true}],"auth_user":"4"})");
    REQUIRE(second.result_int() == 200);
    CHECK(json::parse(second.body()).as_object().at("accounts").as_array()[0].as_object().at("id") == id);
    CHECK(s.state->probes == 5);
    CHECK(s.accounts().size() == 1);
}
TEST_CASE("cookie HTTP rejects invalid input and preserves store on probe failure", "[http][cookie-import]") {
    PushServer s;
    for (auto body : {"{", R"({"cookies":"SID=x","unknown":true})", R"({"cookies":"SID=x","auth_user":"-1"})"})
        CHECK(s.push(body).result_int() == 400);
    auto content = s.request(R"({"cookies":"SID=x"})"); content.set(http::field::content_type, "text/plain");
    CHECK(s.exchange(content).result_int() == 415);
    CHECK(s.push(std::string(1024 * 1024 + 1, 'x')).result_int() == 413);
    for (auto [failure, expected] : {std::pair{1, 502}, std::pair{2, 409}, std::pair{3, 403}, std::pair{4, 502}, std::pair{5, 504}}) {
        s.state->failure = failure;
        auto response = s.push(); CHECK(response.result_int() == expected);
        CHECK(response.body().find("secret-cookie-http") == std::string::npos);
        if (failure == 4) CHECK(response.body().find("cookie_browser_incompatible") != std::string::npos);
        if (failure == 5) CHECK(response.body().find("cookie_browser_timeout") != std::string::npos);
        CHECK(s.accounts().empty());
    }
}
TEST_CASE("cookie HTTP worker leaves control responsive and cancels disconnected clients", "[http][cookie-import]") {
    PushServer s; s.state->slow = true;
    net::io_context io; tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), s.config.port});
    auto request = s.request(R"({"cookies":"SID=secret-cookie-http"})"); http::write(socket, request);
    REQUIRE(s.wait_probes(1));
    auto status = std::async(std::launch::async, [&] { return local_call(s.config, {{"command", "status"}}); });
    CHECK(status.wait_for(1s) == std::future_status::ready);
    boost::system::error_code ec; socket.shutdown(tcp::socket::shutdown_both, ec); socket.close(ec);
    for (unsigned i = 0; i < 200 && !s.state->cancelled; ++i) std::this_thread::sleep_for(5ms);
    CHECK(s.state->cancelled);
    CHECK(s.accounts().empty());
}
TEST_CASE("cookie HTTP probe deadline returns gateway timeout without importing", "[http][cookie-import]") {
    PushServer s(true, 1); s.state->slow = true;
    auto response = s.push(); CHECK(response.result_int() == 504);
    CHECK(s.accounts().empty());
    for (unsigned i = 0; i < 200 && !s.state->cancelled; ++i) std::this_thread::sleep_for(5ms);
    CHECK(s.state->cancelled);
}
TEST_CASE("cookie HTTP scan deadline saves verified accounts before the request timeout", "[http][cookie-import]") {
    // An 8 s timeout leaves a 3 s scan budget; after a 2 s probe another one
    // cannot fit, so the verified account is saved instead of timing out.
    PushServer s(true, 8); s.state->delay_ms = 2000;
    auto response = s.push();
    REQUIRE(response.result_int() == 200);
    auto details = json::parse(response.body()).as_object().at("details").as_object();
    CHECK(details.at("deadline_reached") == true);
    CHECK(details.at("truncated") == true);
    CHECK(details.at("attempted") == 1);
    CHECK(s.state->probes == 1);
    CHECK(s.accounts().size() == 1);
}
TEST_CASE("cookie HTTP import jobs share bounded worker capacity", "[http][cookie-import]") {
    PushServer s(true, 15, 1); s.state->slow = true;
    auto first = std::async(std::launch::async, [&] { return s.push(); });
    REQUIRE(s.wait_probes(1));
    auto second = std::async(std::launch::async, [&] { return s.push(); });
    // One active plus one queued job exhausts the shared admission capacity.
    std::this_thread::sleep_for(100ms);
    auto generation = s.request(R"({"model":"synthetic","messages":[{"role":"user","content":"test"}]})");
    generation.target("/v1/chat/completions");
    generation.set(http::field::authorization, "Bearer generation-key");
    auto third = s.exchange(generation); CHECK(third.result_int() == 429);
    s.state->slow = false;
    CHECK(first.get().result_int() == 200);
    CHECK(second.get().result_int() == 200);
}

TEST_CASE("cookie HTTP revocation during probe prevents persistence", "[http][cookie-import]") {
    PushServer s; s.state->slow = true;
    auto pending = std::async(std::launch::async, [&] { return s.push(); });
    REQUIRE(s.wait_probes(1));
    auto config = s.config.to_json(); config["cookie_import_key"] = "";
    atomic_write(s.config.config_file, json::serialize(config), true);
    local_call(s.config, {{"command", "config.reload"}});
    local_call(s.config, {{"command", "status"}});
    s.state->slow = false;
    auto response = pending.get();
    CHECK(response.result_int() == 403);
    CHECK(s.accounts().empty());
    CHECK(s.push().result_int() == 403);
}
TEST_CASE("cookie userscript is public static content without configured secrets", "[http][cookie-import]") {
    PushServer s;
    auto get = s.request(""); get.method(http::verb::get);
    get.target("/userscript/ais2api-cookie.user.js"); get.erase(http::field::authorization);
    auto response = s.exchange(get);
    REQUIRE(response.result_int() == 200);
    CHECK(response.body().find("==UserScript==") != std::string::npos);
    CHECK(response.body().find("import-key") == std::string::npos);
    CHECK(response.body().find("generation-key") == std::string::npos);
}
TEST_CASE("Cookie requests without Origin work and key rotation rejects in-flight uploads", "[http][cookie-import]") {
    PushServer s;
    auto first = s.exchange(s.request(R"({"cookies":"SID=secret-cookie-http","auth_user":"0"})", "import-key", ""));
    REQUIRE(first.result_int() == 200);
    const auto original = s.accounts();
    s.state->slow = true;
    const auto next_probe = s.state->probes.load() + 1;
    auto pending = std::async(std::launch::async, [&] {
        return s.push(R"({"cookies":"SID=revoked-cookie-http","auth_user":"4"})");
    });
    REQUIRE(s.wait_probes(next_probe));
    auto config = s.config.to_json(); config["cookie_import_key"] = "rotated-import-key";
    atomic_write(s.config.config_file, json::serialize(config), true);
    local_call(s.config, {{"command", "config.reload"}});
    s.state->slow = false;
    CHECK(pending.get().result_int() == 401);
    CHECK(s.accounts() == original);
    CHECK(s.push().result_int() == 401);
    CHECK(s.exchange(s.request(R"({"cookies":"SID=secret-cookie-http","auth_user":"0"})", "rotated-import-key")).result_int() == 200);
}
