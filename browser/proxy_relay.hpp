// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/media.hpp"
#include "ais2api/proxy.hpp"
#include <boost/asio.hpp>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ais2api::browser_detail {
// A loopback HTTP proxy for the browser that opens CONNECT tunnels through an
// upstream proxy, authenticating there. Chromium cannot send proxy
// credentials itself, and never supports SOCKS authentication; with this
// relay the credentials stay in this process and off the browser's command
// line. Only CONNECT to port 443 is accepted, which limits what other local
// processes could do through the listener. Target names are resolved by the
// upstream proxy, never locally.
class ProxyRelay {
public:
    static constexpr std::size_t max_tunnels = 128;
    explicit ProxyRelay(std::chrono::milliseconds half_close_timeout = std::chrono::seconds(30))
        : half_close_timeout_(half_close_timeout) {}
    ProxyRelay(const ProxyRelay&) = delete;
    ProxyRelay& operator=(const ProxyRelay&) = delete;
    ~ProxyRelay() { stop(); }
    // Starts listening on an ephemeral loopback port and returns it.
    unsigned short start(ProxyEndpoint upstream) {
        upstream_ = std::move(upstream);
        acceptor_.open(tcp::v4());
        acceptor_.bind({boost::asio::ip::address_v4::loopback(), 0});
        acceptor_.listen();
        const auto port = acceptor_.local_endpoint().port();
        boost::asio::co_spawn(io_, accept(), boost::asio::detached);
        thread_ = std::thread([this] {
            try {
                io_.run();
            } catch (const std::exception& error) {
                record(error.what());
            } catch (...) {
                record("Proxy relay stopped unexpectedly");
            }
        });
        return port;
    }
    void stop() noexcept {
        io_.stop();
        if (thread_.joinable()) thread_.join();
        error_code ec;
        acceptor_.close(ec);
    }
    // The most recent tunnel failure, for diagnostics; never contains credentials.
    std::string last_error() const {
        std::lock_guard lock(mutex_);
        return last_error_;
    }
    std::size_t tunnels() const { return tunnels_.load(); }

private:
    using tcp = boost::asio::ip::tcp;
    using error_code = boost::system::error_code;
    struct Failure {
        const char* status;  // Reply line for the browser.
        std::string reason;  // Recorded as last_error(); empty for client mistakes.
    };
    struct Tunnel {
        tcp::socket client, upstream;
        tcp::resolver resolver;
        boost::asio::steady_timer deadline;
        boost::asio::steady_timer half_close_deadline;
        std::chrono::milliseconds half_close_timeout;
        bool established = false;
        bool half_closed = false;
        std::atomic<std::size_t>& count;
        Tunnel(tcp::socket socket, std::atomic<std::size_t>& counter, std::chrono::milliseconds half_close)
            : client(std::move(socket)),
              upstream(client.get_executor()),
              resolver(client.get_executor()),
              deadline(client.get_executor()),
              half_close_deadline(client.get_executor()),
              half_close_timeout(half_close),
              count(counter) { ++count; }
        ~Tunnel() { --count; }
        void close() noexcept {
            error_code ec;
            try {
                deadline.cancel();
                half_close_deadline.cancel();
                resolver.cancel();
            } catch (...) {}
            client.close(ec);
            upstream.close(ec);
        }
    };
    std::chrono::milliseconds half_close_timeout_;
    // Destroyed after io_, whose pending coroutines still decrement it.
    std::atomic<std::size_t> tunnels_{0};
    mutable std::mutex mutex_;
    std::string last_error_;
    ProxyEndpoint upstream_;
    boost::asio::io_context io_;
    tcp::acceptor acceptor_{io_};
    std::thread thread_;

    void record(std::string reason) {
        std::lock_guard lock(mutex_);
        last_error_ = std::move(reason);
    }
    static std::string base64(std::string_view input) {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        unsigned bits = 0, count = 0;
        for (unsigned char c : input) {
            bits = (bits << 8) | c; count += 8;
            while (count >= 6) { count -= 6; out += alphabet[(bits >> count) & 63]; }
        }
        if (count) out += alphabet[(bits << (6 - count)) & 63];
        while (out.size() % 4) out += '=';
        return out;
    }
    boost::asio::awaitable<void> accept() {
        for (;;) {
            error_code ec;
            auto socket = co_await acceptor_.async_accept(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec == boost::asio::error::operation_aborted) co_return;
            if (ec) {
                // Out of descriptors and similar: back off instead of spinning.
                boost::asio::steady_timer pause(io_, std::chrono::milliseconds(100));
                error_code wait_ec;
                co_await pause.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_ec));
                if (wait_ec == boost::asio::error::operation_aborted) co_return;
                continue;
            }
            if (tunnels_.load() >= max_tunnels) { socket.close(ec); continue; }
            boost::asio::co_spawn(
                io_, serve(std::make_shared<Tunnel>(std::move(socket), tunnels_, half_close_timeout_)),
                boost::asio::detached);
        }
    }
    // Reads one header block (through the blank line) into `head`; returns the
    // bytes that followed it.
    static boost::asio::awaitable<std::string> read_head(tcp::socket& socket, std::string& head, std::size_t limit) {
        auto buffer = boost::asio::dynamic_buffer(head, limit);
        const auto size = co_await boost::asio::async_read_until(socket, buffer, "\r\n\r\n", boost::asio::use_awaitable);
        std::string rest = head.substr(size);
        head.resize(size);
        co_return rest;
    }
    // "CONNECT host:443 HTTP/1.1" -> host. Throws Failure otherwise.
    static std::string connect_target(std::string_view head) {
        const auto line = head.substr(0, head.find("\r\n"));
        if (!line.starts_with("CONNECT ")) throw Failure{"405 Method Not Allowed", {}};
        auto target = line.substr(8);
        target = target.substr(0, target.find(' '));
        const auto colon = target.rfind(':');
        if (colon == std::string_view::npos || target.substr(colon + 1) != "443") throw Failure{"403 Forbidden", {}};
        std::string host(target.substr(0, colon));
        if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
        if (host.empty() || host.size() > 255 ||
            !std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':'; }))
            throw Failure{"400 Bad Request", {}};
        error_code literal_error;
        const auto address = boost::asio::ip::make_address(host, literal_error);
        if (!literal_error && !media_detail::public_address(address))
            throw Failure{"403 Forbidden", {}};
        return host;
    }
    boost::asio::awaitable<void> serve(std::shared_ptr<Tunnel> tunnel) {
        // Setting up a tunnel is bounded; an established one lasts until a
        // side closes it.
        tunnel->deadline.expires_after(std::chrono::seconds(20));
        tunnel->deadline.async_wait([weak = std::weak_ptr<Tunnel>(tunnel)](error_code ec) {
            if (auto t = weak.lock(); t && !ec && !t->established) t->close();
        });
        std::optional<Failure> failure;
        std::string from_client, from_upstream;
        try {
            std::string head;
            from_client = co_await read_head(tunnel->client, head, 8192);
            const auto host = connect_target(head);
            try {
                from_upstream = co_await open(*tunnel, host);
            } catch (const Failure&) {
                throw;
            } catch (const std::exception&) {
                throw Failure{"502 Bad Gateway", "The connection to the upstream proxy failed"};
            }
            static constexpr std::string_view established = "HTTP/1.1 200 Connection established\r\n\r\n";
            co_await boost::asio::async_write(tunnel->client, boost::asio::buffer(established), boost::asio::use_awaitable);
        } catch (const Failure& error) {
            failure = error;
        } catch (const std::exception&) {
            // The browser hung up or sent an oversized or partial request.
            tunnel->close();
            co_return;
        }
        if (failure) {
            if (!failure->reason.empty()) record(failure->reason);
            const auto reply = std::string("HTTP/1.1 ") + failure->status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            error_code ec;
            co_await boost::asio::async_write(tunnel->client, boost::asio::buffer(reply), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            tunnel->close();
            co_return;
        }
        tunnel->established = true;
        tunnel->deadline.cancel();
        boost::asio::co_spawn(io_, pump(tunnel, tunnel->client, tunnel->upstream, std::move(from_client)), boost::asio::detached);
        co_await pump(tunnel, tunnel->upstream, tunnel->client, std::move(from_upstream));
    }
    // Connects to the upstream proxy and asks it for a tunnel to host:443.
    // Returns tunnel bytes the upstream already sent.
    boost::asio::awaitable<std::string> open(Tunnel& tunnel, const std::string& host) {
        const auto endpoints = co_await tunnel.resolver.async_resolve(
            upstream_.host, std::to_string(upstream_.port), boost::asio::use_awaitable);
        co_await boost::asio::async_connect(tunnel.upstream, endpoints, boost::asio::use_awaitable);
        error_code ignored;
        tunnel.upstream.set_option(tcp::no_delay(true), ignored);
        tunnel.client.set_option(tcp::no_delay(true), ignored);
        tunnel.upstream.set_option(tcp::socket::keep_alive(true), ignored);
        tunnel.client.set_option(tcp::socket::keep_alive(true), ignored);
        if (upstream_.scheme == "http") co_return co_await http_connect(tunnel.upstream, host);
        co_await socks5_connect(tunnel.upstream, host);
        co_return std::string{};
    }
    boost::asio::awaitable<std::string> http_connect(tcp::socket& socket, const std::string& host) {
        const auto authority = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":443";
        std::string request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
        if (upstream_.credentials) request += "Proxy-Authorization: Basic " + base64(upstream_.username + ":" + upstream_.password) + "\r\n";
        request += "\r\n";
        co_await boost::asio::async_write(socket, boost::asio::buffer(request), boost::asio::use_awaitable);
        std::string head;
        auto rest = co_await read_head(socket, head, 16384);
        // "HTTP/1.1 200 ..."
        const auto space = head.find(' ');
        const auto status = space == std::string::npos ? std::string{} : head.substr(space + 1, 3);
        if (status.size() == 3 && status[0] == '2') co_return rest;
        if (status == "407") throw Failure{"502 Bad Gateway", upstream_.credentials ? "The upstream proxy rejected the credentials (HTTP 407)" : "The upstream proxy requires credentials (HTTP 407)"};
        throw Failure{"502 Bad Gateway", "The upstream proxy refused the tunnel (HTTP " + (status.empty() ? std::string("?") : status) + ")"};
    }
    boost::asio::awaitable<void> socks5_connect(tcp::socket& socket, const std::string& host) {
        using boost::asio::buffer;
        using boost::asio::use_awaitable;
        const unsigned char method = upstream_.credentials ? 0x02 : 0x00;
        const std::array<unsigned char, 3> greeting{0x05, 0x01, method};
        co_await boost::asio::async_write(socket, buffer(greeting), use_awaitable);
        std::array<unsigned char, 2> choice{};
        co_await boost::asio::async_read(socket, buffer(choice), use_awaitable);
        if (choice[0] != 0x05 || choice[1] != method)
            throw Failure{"502 Bad Gateway", upstream_.credentials ? "The SOCKS5 proxy does not accept username/password authentication" : "The SOCKS5 proxy requires credentials"};
        if (upstream_.credentials) {
            std::string login{'\x01'};
            login += static_cast<char>(upstream_.username.size()); login += upstream_.username;
            login += static_cast<char>(upstream_.password.size()); login += upstream_.password;
            co_await boost::asio::async_write(socket, buffer(login), use_awaitable);
            std::array<unsigned char, 2> verdict{};
            co_await boost::asio::async_read(socket, buffer(verdict), use_awaitable);
            if (verdict[1] != 0x00) throw Failure{"502 Bad Gateway", "The SOCKS5 proxy rejected the credentials"};
        }
        std::string request{'\x05', '\x01', '\x00'};
        error_code literal;
        const auto address = boost::asio::ip::make_address(host, literal);
        if (!literal && address.is_v4()) {
            request += '\x01';
            const auto bytes = address.to_v4().to_bytes();
            request.append(bytes.begin(), bytes.end());
        } else if (!literal) {
            request += '\x04';
            const auto bytes = address.to_v6().to_bytes();
            request.append(bytes.begin(), bytes.end());
        } else {
            request += '\x03';
            request += static_cast<char>(host.size());
            request += host;
        }
        request += '\x01'; request += '\xbb';  // Port 443.
        co_await boost::asio::async_write(socket, buffer(request), use_awaitable);
        std::array<unsigned char, 5> reply{};
        co_await boost::asio::async_read(socket, buffer(reply), use_awaitable);
        if (reply[0] != 0x05 || reply[1] != 0x00)
            throw Failure{"502 Bad Gateway", "The SOCKS5 proxy refused the connection (reply " + std::to_string(reply[1]) + ")"};
        // Skip the bound address: 5 bytes are read, the rest of it plus the port remain.
        const std::size_t remaining = reply[3] == 0x01 ? 4 - 1 + 2 : reply[3] == 0x04 ? 16 - 1 + 2 : reply[3] == 0x03 ? reply[4] + 2 : 0;
        if (!remaining) throw Failure{"502 Bad Gateway", "The SOCKS5 proxy sent an invalid reply"};
        std::array<unsigned char, 257> bound{};
        co_await boost::asio::async_read(socket, buffer(bound.data(), remaining), use_awaitable);
    }
    static boost::asio::awaitable<void> pump(std::shared_ptr<Tunnel> tunnel, tcp::socket& from, tcp::socket& to, std::string pending) {
        try {
            if (!pending.empty()) co_await boost::asio::async_write(to, boost::asio::buffer(pending), boost::asio::use_awaitable);
            std::string().swap(pending);
            std::array<char, 16384> data;
            for (;;) {
                const auto size = co_await from.async_read_some(boost::asio::buffer(data), boost::asio::use_awaitable);
                co_await boost::asio::async_write(to, boost::asio::buffer(data.data(), size), boost::asio::use_awaitable);
            }
        } catch (const boost::system::system_error& error) {
            error_code ec;
            // Pass a clean end on; anything else ends both directions.
            if (error.code() == boost::asio::error::eof) {
                to.shutdown(tcp::socket::shutdown_send, ec);
                if (!tunnel->half_closed) {
                    tunnel->half_closed = true;
                    tunnel->half_close_deadline.expires_after(tunnel->half_close_timeout);
                    tunnel->half_close_deadline.async_wait([weak = std::weak_ptr<Tunnel>(tunnel)](error_code wait_ec) {
                        if (auto t = weak.lock(); t && !wait_ec) t->close();
                    });
                } else {
                    tunnel->close();
                }
            } else {
                tunnel->close();
            }
        }
    }
};
} // namespace ais2api::browser_detail
