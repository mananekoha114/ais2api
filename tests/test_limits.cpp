// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include "ais2api/store.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <sstream>
#include <thread>
using namespace ais2api;
namespace limit_net = boost::asio;
namespace limit_http = boost::beast::http;
using LimitTcp = limit_net::ip::tcp;
using namespace std::chrono_literals;
namespace {
struct LimitsDirectory {
    fs::path path = fs::temp_directory_path() / ("ais2api-limits-" + random_hex(8));
    LimitsDirectory() { fs::create_directories(path); }
    ~LimitsDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
struct LimitsServer {
    LimitsDirectory directory;
    Config config;
    std::string key;
    std::future<int> running;
    explicit LimitsServer(std::size_t request_bytes = 4096) {
        limit_net::io_context io;
        LimitTcp::acceptor reserve(io, {LimitTcp::v4(), 0});
        config.port = reserve.local_endpoint().port();
        reserve.close();
        config.data_dir = directory.path / "data";
        config.config_file = directory.path / "config.json";
        config.limits.request_bytes = request_bytes;
        config.limits.object_bytes = std::max<std::size_t>(8192, request_bytes);
        config.limits.relay_bytes = 1024;
        key = "limits-test-" + random_hex(24);
        config.api_key = key;
        atomic_write(config.config_file, json::serialize(config.to_json()), true);
        running = std::async(std::launch::async, [this] { return serve(config); });
        // Sanitizer builds on shared runners can take seconds to start.
        for (const auto deadline = Clock::now() + 30s; Clock::now() < deadline;) {
            if (running.wait_for(0ms) == std::future_status::ready) {
                running.get();
                throw std::runtime_error("Test service exited during startup");
            }
            try {
                local_call(config, {{"command", "status"}});
                return;
            } catch (const Error &) {
            }
            std::this_thread::sleep_for(20ms);
        }
        // A late start would otherwise block the future's destructor forever.
        try {
            local_call(config, {{"command", "stop"}});
        } catch (...) {
        }
        throw std::runtime_error("Test service failed to open its control endpoint");
    }
    ~LimitsServer() {
        if (running.valid()) {
            try {
                local_call(config, {{"command", "stop"}});
            } catch (...) {
            }
            running.wait();
        }
    }
    unsigned connections() {
        auto status = local_call(config, {{"command", "status"}});
        return json::value_to<unsigned>(status.at("connections"));
    }
    limit_http::response<limit_http::string_body> request(const json::object &body) {
        limit_net::io_context io;
        LimitTcp::socket socket(io);
        socket.connect({limit_net::ip::make_address("127.0.0.1"), config.port});
        limit_http::request<limit_http::string_body> request(limit_http::verb::post,
                                                             "/v1/chat/completions", 11);
        request.set(limit_http::field::host, "127.0.0.1");
        request.set(limit_http::field::authorization, "Bearer " + key);
        request.set(limit_http::field::content_type, "application/json");
        request.body() = json::serialize(body);
        request.prepare_payload();
        limit_http::write(socket, request);
        boost::beast::flat_buffer buffer;
        limit_http::response<limit_http::string_body> response;
        limit_http::read(socket, buffer, response);
        return response;
    }
};
} // namespace
TEST_CASE("oversized partial downloads are rejected before any network access", "[limits][setup]") {
    LimitsDirectory directory;
    auto target = directory.path / "browser.zip";
    auto partial = fs::path(target.string() + ".part");
    atomic_write(partial, "123456789");
    try {
        download_verified("https://not-contacted.invalid/browser", target, std::string(64, '0'), {},
                          8);
        FAIL("Oversized partial was accepted");
    } catch (const Error &error) {
        CHECK(error.status == 413);
        CHECK(error.code == "download_limit");
    }
    CHECK_FALSE(fs::exists(partial));
    CHECK_FALSE(fs::exists(target));
}
TEST_CASE("verified complete partial downloads are promoted without contacting a server",
          "[limits][setup]") {
    LimitsDirectory directory;
    auto target = directory.path / "browser.zip";
    auto partial = fs::path(target.string() + ".part");
    std::string payload = "locked runtime archive";
    atomic_write(partial, payload);
    download_verified("https://not-contacted.invalid/browser", target, sha256(payload), {},
                      payload.size());
    CHECK(read_file(target) == payload);
    CHECK_FALSE(fs::exists(partial));
}
TEST_CASE("verified partial replaces an invalid old browser cache on Windows too",
          "[limits][setup]") {
    LimitsDirectory directory;
    auto target = directory.path / "browser.zip";
    auto partial = fs::path(target.string() + ".part");
    std::string payload = "replacement locked artifact";
    atomic_write(target, "invalid previous cache");
    atomic_write(partial, payload);
    REQUIRE_NOTHROW(download_verified("https://not-contacted.invalid/browser", target,
                                      sha256(payload), {}, payload.size()));
    CHECK(read_file(target) == payload);
    CHECK_FALSE(fs::exists(partial));
}
TEST_CASE("HTTP bounds half-header connections before request admission", "[limits][http]") {
    LimitsServer server;
    limit_net::io_context io;
    std::vector<std::unique_ptr<LimitTcp::socket>> held;
    for (unsigned i = 0; i < 32; ++i) {
        auto socket = std::make_unique<LimitTcp::socket>(io);
        socket->connect({limit_net::ip::make_address("127.0.0.1"), server.config.port});
        limit_net::write(*socket, limit_net::buffer("GET /health HTTP/1.1\r\nHost:", 25));
        held.push_back(std::move(socket));
    }
    auto deadline = Clock::now() + 3s;
    while (server.connections() != 32 && Clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(server.connections() == 32);
    LimitTcp::socket extra(io);
    extra.connect({limit_net::ip::make_address("127.0.0.1"), server.config.port});
    extra.non_blocking(true);
    bool closed = false;
    std::array<char, 1> byte{};
    deadline = Clock::now() + 3s;
    while (Clock::now() < deadline) {
        boost::system::error_code ec;
        extra.read_some(limit_net::buffer(byte), ec);
        if (ec == limit_net::error::eof || ec == limit_net::error::connection_reset) {
            closed = true;
            break;
        }
        if (ec && ec != limit_net::error::would_block && ec != limit_net::error::try_again)
            break;
        std::this_thread::sleep_for(10ms);
    }
    CHECK(closed);
    CHECK(server.connections() == 32);
    for (auto &socket : held) {
        boost::system::error_code ec;
        socket->close(ec);
    }
    deadline = Clock::now() + 3s;
    while (server.connections() != 0 && Clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    CHECK(server.connections() == 0);
}
TEST_CASE("media cumulative budget is checked before fetching another image", "[limits][http]") {
    LimitsServer server;
    json::object body{
        {"model", "test"},
        {"messages",
         json::array{json::object{
             {"role", "user"},
             {"content",
              json::array{
                  json::object{
                      {"type", "image_url"},
                      {"image_url", {{"url", "data:image/png;base64," + std::string(100, 'A')}}}},
                  json::object{{"type", "image_url"},
                               {"image_url", {{"url", "https://127.0.0.1/not-contacted"}}}}}}}}}};
    auto &content = body.at("messages").as_array()[0].as_object().at("content").as_array();
    auto &inline_url = content[0].as_object().at("image_url").as_object().at("url");
    auto initial = json::serialize(body).size();
    auto target = server.config.limits.request_bytes - 60;
    REQUIRE(initial < target);
    inline_url =
        json::string(std::string(inline_url.as_string()) + std::string(target - initial, 'A'));
    REQUIRE(json::serialize(body).size() == target);
    auto response = server.request(body);
    CHECK(response.result_int() == 413);
    CHECK(response.body().find("media_limit") != std::string::npos);
}
TEST_CASE("concurrent small chunked requests succeed and unauthenticated large requests fail before admission",
          "[limits][http]") {
    LimitsServer server(32 * 1024 * 1024);
    limit_net::io_context io;

    // Previously each chunked request reserved 32 MiB * 4 = 128 MiB, causing 429 on 2+ concurrent streams.
    constexpr std::size_t concurrent = 10;
    std::vector<std::unique_ptr<LimitTcp::socket>> sockets;
    for (std::size_t i = 0; i < concurrent; ++i) {
        auto s = std::make_unique<LimitTcp::socket>(io);
        s->connect({limit_net::ip::make_address("127.0.0.1"), server.config.port});
        const std::string head =
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Authorization: Bearer " + server.key + "\r\n"
            "Content-Type: application/json\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "1\r\n{\r\n";
        limit_net::write(*s, limit_net::buffer(head));
        sockets.push_back(std::move(s));
    }
    const auto wait_deadline = Clock::now() + 3s;
    while (server.connections() != concurrent && Clock::now() < wait_deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(server.connections() == concurrent);

    const std::string tail_json = R"("model":"m","messages":[{"role":"user","content":"hi"}]})";
    std::ostringstream tail_stream;
    tail_stream << std::hex << tail_json.size() << "\r\n" << tail_json << "\r\n0\r\n\r\n";
    const std::string tail = tail_stream.str();
    for (auto& s : sockets) {
        limit_net::write(*s, limit_net::buffer(tail));
        boost::beast::flat_buffer buffer;
        limit_http::response<limit_http::string_body> response;
        limit_http::read(*s, buffer, response);
        CHECK(response.result_int() == 503);
        CHECK(response.body().find("no_account") != std::string::npos);
    }

    auto read_header_only_rejection = [&](std::string raw_head) {
        LimitTcp::socket s(io);
        s.connect({limit_net::ip::make_address("127.0.0.1"), server.config.port});
        limit_net::write(s, limit_net::buffer(raw_head));
        boost::beast::flat_buffer buffer;
        limit_http::response<limit_http::string_body> response;
        limit_http::read(s, buffer, response);
        return response;
    };
    const std::string large_unauth =
        "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\nContent-Length: 33554432\r\n\r\n";
    const auto unauth = read_header_only_rejection(large_unauth);
    CHECK(unauth.result_int() == 401);
    const std::string large_unknown =
        "POST /v1/unknown HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + server.key +
        "\r\nContent-Type: application/json\r\nContent-Length: 33554432\r\n\r\n";
    const auto unknown = read_header_only_rejection(large_unknown);
    CHECK(unknown.result_int() == 404);
    const std::string large_get =
        "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 33554432\r\n\r\n";
    const auto get_res = read_header_only_rejection(large_get);
    CHECK(get_res.result_int() == 413);
}
