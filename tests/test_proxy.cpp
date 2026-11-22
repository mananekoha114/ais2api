// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/config.hpp"
#include "ais2api/proxy.hpp"
#include "ais2api/retry_policy.hpp"
#include "../browser/proxy_relay.hpp"
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <future>
#include <thread>

using namespace ais2api;
namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

// A blocking loopback server that runs `script` on its first connection.
struct FakeUpstream {
    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    unsigned short port = acceptor.local_endpoint().port();
    std::atomic<bool> contacted = false;
    std::jthread worker;
    explicit FakeUpstream(std::function<void(tcp::socket&)> script) : worker([this, script = std::move(script)] {
        try {
            tcp::socket socket{io};
            acceptor.accept(socket);
            contacted = true;
            script(socket);
        } catch (...) {}
    }) {}
    ~FakeUpstream() {
        // Closing from another thread does not wake a blocking accept on
        // every platform; a connection does.
        if (contacted) return;
        try { asio::io_context local; tcp::socket wake{local}; wake.connect({asio::ip::address_v4::loopback(), port}); } catch (...) {}
    }
};
// Reads one header block; bytes that arrived after it stay in `buffered`.
std::string read_head(tcp::socket& socket, std::string& buffered) {
    const auto size = asio::read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n");
    auto head = buffered.substr(0, size);
    buffered.erase(0, size);
    return head;
}
std::string read_head(tcp::socket& socket) {
    std::string buffered;
    return read_head(socket, buffered);
}
std::string read_exactly(tcp::socket& socket, std::size_t size, std::string& buffered) {
    if (buffered.size() < size)
        asio::read(socket, asio::dynamic_buffer(buffered), asio::transfer_exactly(size - buffered.size()));
    auto data = buffered.substr(0, size);
    buffered.erase(0, size);
    return data;
}
std::string read_exactly(tcp::socket& socket, std::size_t size) {
    std::string buffered;
    return read_exactly(socket, size, buffered);
}
// Echoes everything after greeting the client with `hello`.
void echo(tcp::socket& socket, std::string_view hello) {
    asio::write(socket, asio::buffer(hello));
    std::array<char, 8192> buffer;
    boost::system::error_code ec;
    for (;;) {
        const auto size = socket.read_some(asio::buffer(buffer), ec);
        if (ec) return;
        asio::write(socket, asio::buffer(buffer.data(), size), ec);
        if (ec) return;
    }
}
struct Browser {
    asio::io_context io;
    tcp::socket socket{io};
    // Tunnel bytes that arrived together with the CONNECT reply.
    std::string buffered;
    explicit Browser(unsigned short port) { socket.connect({asio::ip::address_v4::loopback(), port}); }
    std::string connect(std::string_view target) {
        const auto request = "CONNECT " + std::string(target) + " HTTP/1.1\r\nHost: " + std::string(target) + "\r\n\r\n";
        asio::write(socket, asio::buffer(request));
        return read_head(socket, buffered);
    }
    std::string read(std::size_t size) { return read_exactly(socket, size, buffered); }
};
ProxyEndpoint endpoint(const std::string& url) {
    auto parsed = parse_proxy(url);
    REQUIRE(parsed);
    return *parsed;
}
}

TEST_CASE("proxy URLs parse like curl and Chromium read them", "[proxy]") {
    auto plain = parse_proxy("http://proxy.example:8080");
    REQUIRE(plain);
    CHECK(plain->scheme == "http"); CHECK(plain->host == "proxy.example"); CHECK(plain->port == 8080); CHECK_FALSE(plain->credentials);
    auto login = parse_proxy("SOCKS5H://user%40mail:p%3Ass%25@[::1]:1081/");
    REQUIRE(login);
    CHECK(login->scheme == "socks5h"); CHECK(login->host == "::1"); CHECK(login->port == 1081);
    CHECK(login->credentials); CHECK(login->username == "user@mail"); CHECK(login->password == "p:ss%");
    auto bare = parse_proxy("127.0.0.1:3128");
    REQUIRE(bare); CHECK(bare->scheme == "http"); CHECK(bare->port == 3128);
    CHECK(parse_proxy("http://h")->port == 80);
    CHECK(parse_proxy("socks5://h")->port == 1080);
    CHECK(parse_proxy("https://h")->port == 443);
    auto empty_password = parse_proxy("http://user@h:1");
    REQUIRE(empty_password); CHECK(empty_password->credentials); CHECK(empty_password->password.empty());
    for (auto invalid : {"", "ftp://h:1", "http://", "http://h:0", "http://h:65536", "http://h:x", "http://h:1/path",
                         "http://h:1?q", "http://[::1", "http://bad%zz@h:1", "http=foo:80;https=bar:80", "http://h:"})
        CHECK_FALSE(parse_proxy(invalid));
    CHECK(relay_supports(endpoint("http://u:p@h:1")));
    CHECK(relay_supports(endpoint("socks5://u:p@h:1")));
    CHECK(relay_supports(endpoint("socks5h://h:1")));
    CHECK_FALSE(relay_supports(endpoint("https://h:1")));
    CHECK_FALSE(relay_supports(endpoint("socks4://h:1")));
    CHECK_FALSE(relay_supports(endpoint("socks5://:p@h:1")));
    CHECK_FALSE(relay_supports(endpoint("socks5://" + std::string(256, 'u') + ":p@h:1")));
}

TEST_CASE("the relay authenticates CONNECT to an HTTP proxy and carries both directions", "[proxy][relay]") {
    std::promise<std::string> seen;
    FakeUpstream upstream([&](tcp::socket& socket) {
        seen.set_value(read_head(socket));
        asio::write(socket, asio::buffer(std::string_view("HTTP/1.1 200 Connection established\r\nProxy-Agent: fake\r\n\r\n")));
        echo(socket, "hello-from-upstream");
    });
    browser_detail::ProxyRelay relay;
    const auto port = relay.start(endpoint("http://user%40x:p%3Ass@127.0.0.1:" + std::to_string(upstream.port)));
    Browser browser(port);
    CHECK(browser.connect("aistudio.google.com:443").starts_with("HTTP/1.1 200"));
    const auto head = seen.get_future().get();
    CHECK(head.starts_with("CONNECT aistudio.google.com:443 HTTP/1.1\r\n"));
    // base64("user@x:p:ss")
    CHECK(head.find("\r\nProxy-Authorization: Basic dXNlckB4OnA6c3M=\r\n") != std::string::npos);
    CHECK(browser.read(19) == "hello-from-upstream");
    std::string payload(1024 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i * 31 % 251);
    std::jthread writer([&] { asio::write(browser.socket, asio::buffer(payload)); });
    CHECK(browser.read(payload.size()) == payload);
    CHECK(relay.tunnels() == 1);
    writer.join();
    browser.socket.close();
    for (int i = 0; i < 100 && relay.tunnels(); ++i) std::this_thread::sleep_for(10ms);
    CHECK(relay.tunnels() == 0);
    CHECK(relay.last_error().empty());
}

TEST_CASE("the relay reports a rejected login without revealing it", "[proxy][relay]") {
    FakeUpstream upstream([](tcp::socket& socket) {
        read_head(socket);
        asio::write(socket, asio::buffer(std::string_view("HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"x\"\r\nContent-Length: 0\r\n\r\n")));
        std::this_thread::sleep_for(200ms);
    });
    browser_detail::ProxyRelay relay;
    Browser browser(relay.start(endpoint("http://user:hunter2@127.0.0.1:" + std::to_string(upstream.port))));
    CHECK(browser.connect("aistudio.google.com:443").starts_with("HTTP/1.1 502"));
    CHECK(relay.last_error() == "The upstream proxy rejected the credentials (HTTP 407)");
    CHECK(relay.last_error().find("hunter2") == std::string::npos);
}

TEST_CASE("the relay only opens tunnels to port 443", "[proxy][relay]") {
    FakeUpstream upstream([](tcp::socket&) {});
    browser_detail::ProxyRelay relay;
    const auto port = relay.start(endpoint("http://127.0.0.1:" + std::to_string(upstream.port)));
    CHECK(Browser(port).connect("aistudio.google.com:80").starts_with("HTTP/1.1 403"));
    CHECK(Browser(port).connect("aistudio.google.com:4430").starts_with("HTTP/1.1 403"));
    {
        Browser plain(port);
        asio::write(plain.socket, asio::buffer(std::string_view("GET http://aistudio.google.com/ HTTP/1.1\r\nHost: aistudio.google.com\r\n\r\n")));
        CHECK(read_head(plain.socket).starts_with("HTTP/1.1 405"));
    }
    CHECK(Browser(port).connect("bad!host:443").starts_with("HTTP/1.1 400"));
    CHECK_FALSE(upstream.contacted);
    // Client mistakes are not upstream failures.
    CHECK(relay.last_error().empty());
}

TEST_CASE("the relay logs in to a SOCKS5 proxy and leaves name resolution to it", "[proxy][relay]") {
    std::promise<std::string> target;
    FakeUpstream upstream([&](tcp::socket& socket) {
        if (read_exactly(socket, 3) != std::string("\x05\x01\x02", 3)) return;
        asio::write(socket, asio::buffer(std::string("\x05\x02", 2)));
        auto header = read_exactly(socket, 2);
        auto user = read_exactly(socket, static_cast<unsigned char>(header[1]));
        auto length = read_exactly(socket, 1);
        auto password = read_exactly(socket, static_cast<unsigned char>(length[0]));
        const bool accepted = user == "alice" && password == "s3cret";
        asio::write(socket, asio::buffer(std::string(accepted ? "\x01\x00" : "\x01\x01", 2)));
        if (!accepted) return;
        auto request = read_exactly(socket, 5);
        auto rest = read_exactly(socket, static_cast<unsigned char>(request[4]) + 2);
        target.set_value(request.substr(0, 4) + rest);
        asio::write(socket, asio::buffer(std::string("\x05\x00\x00\x01\x7f\x00\x00\x01\x01\xbb", 10)));
        echo(socket, "socks-ready");
    });
    browser_detail::ProxyRelay relay;
    Browser browser(relay.start(endpoint("socks5://alice:s3cret@127.0.0.1:" + std::to_string(upstream.port))));
    CHECK(browser.connect("aistudio.google.com:443").starts_with("HTTP/1.1 200"));
    CHECK(target.get_future().get() == std::string("\x05\x01\x00\x03", 4) + "aistudio.google.com" + std::string("\x01\xbb", 2));
    CHECK(browser.read(11) == "socks-ready");
    asio::write(browser.socket, asio::buffer(std::string_view("ping")));
    CHECK(browser.read(4) == "ping");
}

TEST_CASE("the relay reports a SOCKS5 login failure", "[proxy][relay]") {
    FakeUpstream upstream([](tcp::socket& socket) {
        read_exactly(socket, 3);
        asio::write(socket, asio::buffer(std::string("\x05\x02", 2)));
        auto header = read_exactly(socket, 2);
        read_exactly(socket, static_cast<unsigned char>(header[1]));
        auto length = read_exactly(socket, 1);
        read_exactly(socket, static_cast<unsigned char>(length[0]));
        asio::write(socket, asio::buffer(std::string("\x01\x01", 2)));
        std::this_thread::sleep_for(200ms);
    });
    browser_detail::ProxyRelay relay;
    Browser browser(relay.start(endpoint("socks5://alice:wrong@127.0.0.1:" + std::to_string(upstream.port))));
    CHECK(browser.connect("aistudio.google.com:443").starts_with("HTTP/1.1 502"));
    CHECK(relay.last_error() == "The SOCKS5 proxy rejected the credentials");
}

TEST_CASE("the relay reports an unreachable upstream proxy", "[proxy][relay]") {
    // Bound but not listening: connections are refused, and the port cannot
    // be handed to the relay's own listener while the test runs.
    asio::io_context io;
    tcp::socket reserved(io);
    reserved.open(tcp::v4());
    reserved.bind({asio::ip::address_v4::loopback(), 0});
    const auto closed = reserved.local_endpoint().port();
    browser_detail::ProxyRelay relay;
    Browser browser(relay.start(endpoint("http://127.0.0.1:" + std::to_string(closed))));
    CHECK(browser.connect("aistudio.google.com:443").starts_with("HTTP/1.1 502"));
    CHECK(relay.last_error() == "The connection to the upstream proxy failed");
}

TEST_CASE("unsupported proxy authentication fails without retrying", "[proxy]") {
    CHECK(classify_failure(Error(503, "proxy_auth_unsupported", "x"), 0, false).action == FailureAction::fail);
}

TEST_CASE("the proxy relay switch is off by default and checked only when on", "[proxy][config]") {
    const auto directory = fs::temp_directory_path() / ("ais2api-proxy-config-" + random_hex(8));
    fs::create_directories(directory);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{directory};
    auto cfg = Config::load(directory / "config.json");
    CHECK_FALSE(Config{}.browser_proxy_relay);
    CHECK(cfg.to_json().at("browser_proxy_relay") == false);
    // The direct path keeps accepting what it accepted before.
    CHECK_NOTHROW(cfg.patched({{"proxy", "http=foo:80;https=bar:80"}}));
    CHECK_NOTHROW(cfg.patched({{"proxy", "http://user:pass@proxy:8080"}}));
    auto relayed = cfg.patched({{"browser_proxy_relay", true}, {"proxy", "socks5://user:pass@proxy:1080"}});
    CHECK(relayed.browser_proxy_relay);
    CHECK(restart_required(cfg, relayed));
    CHECK_NOTHROW(cfg.patched({{"browser_proxy_relay", true}}));
    CHECK_NOTHROW(cfg.patched({{"browser_proxy_relay", true}, {"proxy", "http://proxy:3128"}}));
    for (auto invalid : {"https://proxy:443", "socks4://proxy:1080", "http=foo:80;https=bar:80", "proxy:x"})
        CHECK_THROWS_AS(cfg.patched({{"browser_proxy_relay", true}, {"proxy", invalid}}), Error);
    CHECK_THROWS_AS(cfg.patched({{"browser_proxy_relay", "true"}}), Error);
    relayed.save();
    CHECK(Config::load(cfg.config_file).browser_proxy_relay);
    CHECK_FALSE(Config::load(cfg.config_file, {{"browser_proxy_relay", false}}).browser_proxy_relay);
}

TEST_CASE("the relay rejects literal non-public IP targets without contacting upstream", "[proxy][relay]") {
    FakeUpstream upstream([](tcp::socket&) {});
    browser_detail::ProxyRelay relay;
    const auto port = relay.start(endpoint("http://127.0.0.1:" + std::to_string(upstream.port)));
    for (const auto* target : {"127.0.0.1:443", "10.0.0.1:443", "172.16.0.1:443", "192.168.1.1:443",
                               "169.254.169.254:443", "0.0.0.0:443", "[::1]:443", "[::]:443", "[fc00::1]:443",
                               "[fd12::1]:443", "[fe80::1]:443", "[::ffff:127.0.0.1]:443"}) {
        CAPTURE(target);
        CHECK(Browser(port).connect(target).starts_with("HTTP/1.1 403"));
    }
    CHECK_FALSE(upstream.contacted);
    CHECK(relay.last_error().empty());
}

TEST_CASE("the relay closes half-closed tunnels after the half-close timeout", "[proxy][relay]") {
    std::atomic<bool> done = false;
    FakeUpstream upstream([&](tcp::socket& socket) {
        read_head(socket);
        asio::write(socket, asio::buffer(std::string_view("HTTP/1.1 200 Connection established\r\n\r\n")));
        while (!done.load()) std::this_thread::sleep_for(10ms);
    });
    struct Finish {
        std::atomic<bool>& done;
        ~Finish() { done.store(true); }
    } finish{done};
    browser_detail::ProxyRelay relay(std::chrono::milliseconds(150));
    const auto port = relay.start(endpoint("http://127.0.0.1:" + std::to_string(upstream.port)));
    Browser browser(port);
    REQUIRE(browser.connect("aistudio.google.com:443").starts_with("HTTP/1.1 200"));
    CHECK(relay.tunnels() == 1);
    browser.socket.shutdown(tcp::socket::shutdown_send);
    for (int i = 0; i < 200 && relay.tunnels(); ++i) std::this_thread::sleep_for(25ms);
    CHECK(relay.tunnels() == 0);
}

TEST_CASE("stopping the relay closes its listening acceptor", "[proxy][relay]") {
    browser_detail::ProxyRelay relay;
    const auto port = relay.start(endpoint("http://127.0.0.1:1"));
    relay.stop();
    asio::io_context io;
    tcp::socket probe(io);
    boost::system::error_code ec;
    probe.connect({asio::ip::address_v4::loopback(), port}, ec);
    CHECK(static_cast<bool>(ec));
}
