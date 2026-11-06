// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/common.hpp"
#include "json_text.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
namespace ais2api::browser_detail {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
// A reader thread owns the websocket and drains every message, so events never
// queue up in Chrome between calls. Calls come from one thread at a time.
class Cdp {
public:
    Cdp() = default;
    Cdp(const Cdp&) = delete;
    Cdp& operator=(const Cdp&) = delete;
    ~Cdp() {
        close();
        io_.stop();
        if (reader_.joinable()) reader_.join();
        shutdown_socket();
    }
    void connect(unsigned short port, std::string target, std::stop_token token) {
        perform([&](auto callback) { beast::get_lowest_layer(socket_).async_connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), callback); }, token, 5s);
        socket_.read_message_max(2 * 1024 * 1024);
        perform([&](auto callback) { socket_.async_handshake("127.0.0.1:" + std::to_string(port), target, callback); }, token, 5s);
        io_.restart();
        work_.emplace(asio::make_work_guard(io_));
        read();
        reader_ = std::thread([this] { io_.run(); });
    }
    bool open() {
        std::lock_guard lock(mutex_);
        return reader_.joinable() && !failure_;
    }
    json::object call(std::string method, json::object params, std::string session, std::stop_token token, std::chrono::seconds timeout = 30s) {
        const auto id = next_id();
        json::object request{{"id", id}, {"method", method}, {"params", std::move(params)}};
        if (!session.empty()) request["sessionId"] = session;
        return exchange(json::serialize(request), id, method, session, token, timeout);
    }
    // Sends a command without waiting; its reply is dropped.
    void notify(std::string method, json::object params, std::string session) {
        json::object request{{"id", next_id()}, {"method", std::move(method)}, {"params", std::move(params)}};
        if (!session.empty()) request["sessionId"] = std::move(session);
        {
            std::lock_guard lock(mutex_);
            if (failure_) return;
        }
        send(json::serialize(request));
    }
    json::value evaluate(std::string expression, const std::string& session, std::stop_token token, std::chrono::seconds timeout = 30s) {
        const auto id = next_id();
        // Built as text so a large expression is escaped once instead of being
        // copied into a JSON tree and serialized.
        std::string message = "{\"id\":" + std::to_string(id) + ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":";
        message.reserve(message.size() + json_size(json::string_view(expression)) + json_size(json::string_view(session)) + 96);
        append_quoted(message, expression);
        std::string().swap(expression);
        message += ",\"awaitPromise\":true,\"returnByValue\":true}";
        if (!session.empty()) { message += ",\"sessionId\":"; append_quoted(message, session); }
        message += '}';
        return remote_value(exchange(std::move(message), id, "Runtime.evaluate", session, token, timeout));
    }
    // Calls `declaration` with `this` bound to a remote object. The arguments
    // travel as protocol string values, never as script source, so the page
    // holds a single copy of each.
    json::value call_function(std::string_view object, std::string_view declaration, std::initializer_list<std::string_view> arguments,
                              const std::string& session, std::stop_token token, std::chrono::seconds timeout = 30s) {
        const auto id = next_id();
        auto size = json_size(json::string_view(object)) + json_size(json::string_view(declaration)) + json_size(json::string_view(session)) + 160;
        for (auto argument : arguments) size += json_size(json::string_view(argument)) + 12;
        std::string message = "{\"id\":" + std::to_string(id) + ",\"method\":\"Runtime.callFunctionOn\",\"params\":{\"functionDeclaration\":";
        message.reserve(message.size() + size);
        append_quoted(message, declaration);
        message += ",\"objectId\":";
        append_quoted(message, object);
        message += ",\"arguments\":[";
        bool first = true;
        for (auto argument : arguments) {
            if (!first) message += ',';
            first = false;
            message += "{\"value\":";
            append_quoted(message, argument);
            message += '}';
        }
        message += "],\"awaitPromise\":true,\"returnByValue\":true}";
        if (!session.empty()) { message += ",\"sessionId\":"; append_quoted(message, session); }
        message += '}';
        return remote_value(exchange(std::move(message), id, "Runtime.callFunctionOn", session, token, timeout));
    }
    // Fails waiting and later calls at once; the reader stops with the socket.
    void close() noexcept {
        try {
            {
                std::lock_guard lock(mutex_);
                if (!failure_) failure_ = Error(502, "browser_disconnected", "Browser CDP connection closed");
            }
            cv_.notify_all();
            if (reader_.joinable()) asio::post(io_, [this] { shutdown_socket(); });
            else shutdown_socket();
        } catch (...) {}
    }
private:
    asio::io_context io_;
    websocket::stream<beast::tcp_stream> socket_{io_};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::thread reader_;
    // Used only on the reader thread.
    beast::flat_buffer buffer_;
    std::deque<std::shared_ptr<std::string>> outbox_;
    // Shared with callers.
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::uint64_t next_ = 0;
    std::set<std::uint64_t> waiting_;
    std::map<std::uint64_t, json::object> replies_;
    std::set<std::string> crashed_;
    std::optional<Error> failure_;
    static Error crashed() { return Error(502, "browser_disconnected", "Browser page crashed or detached"); }
    std::uint64_t next_id() { std::lock_guard lock(mutex_); return ++next_; }
    void shutdown_socket() noexcept { beast::error_code ec; beast::get_lowest_layer(socket_).socket().close(ec); }
    void fail(Error error) {
        {
            std::lock_guard lock(mutex_);
            if (!failure_) failure_ = std::move(error);
        }
        cv_.notify_all();
        shutdown_socket();
    }
    static json::value remote_value(const json::object& result) {
        if (result.if_contains("exceptionDetails")) throw Error(502, "browser_incompatible", "Page bridge failed or current AI Studio page is unsupported");
        auto remote = result.if_contains("result");
        if (!remote || !remote->is_object()) throw Error(502, "browser_incompatible", "Missing Runtime result");
        auto value = remote->as_object().if_contains("value");
        return value ? *value : json::value{};
    }
    // Reader thread: exactly one read is outstanding until the socket fails.
    void read() {
        socket_.async_read(buffer_, [this](beast::error_code ec, std::size_t) {
            if (ec) { fail(Error(502, "browser_disconnected", "Browser CDP connection failed")); return; }
            const bool valid = dispatch();
            buffer_.consume(buffer_.size());
            if (buffer_.capacity() > 64 * 1024) buffer_.shrink_to_fit();
            if (valid) read();
        });
    }
    bool dispatch() {
        const auto data = buffer_.cdata();
        const std::string_view text(static_cast<const char*>(data.data()), data.size());
        // Chrome writes "method" first in events; skip the ones this client
        // ignores without parsing them.
        if (constexpr std::string_view prefix = "{\"method\":\""; text.starts_with(prefix)) {
            const auto name = text.substr(prefix.size(), text.find('"', prefix.size()) - prefix.size());
            if (name != "Inspector.targetCrashed" && name != "Inspector.detached") return true;
        }
        beast::error_code parse_error;
        auto value = json::parse(text, parse_error);
        if (parse_error || !value.is_object()) { fail(Error(502, "browser_incompatible", "Invalid CDP reply")); return false; }
        auto& object = value.as_object();
        if (auto event = object.if_contains("method"); event && event->is_string()) {
            // Inspector reports a renderer crash or a lost page session.
            if (event->as_string() == "Inspector.targetCrashed" || event->as_string() == "Inspector.detached") {
                if (const auto lost = string_or(object, "sessionId"); !lost.empty()) {
                    { std::lock_guard lock(mutex_); crashed_.insert(lost); }
                    cv_.notify_all();
                }
            }
            return true;
        }
        auto id = object.if_contains("id");
        if (!id || !id->is_int64() || id->as_int64() <= 0) return true;
        const auto number = static_cast<std::uint64_t>(id->as_int64());
        {
            std::lock_guard lock(mutex_);
            // Replies nobody waits for (abandoned or fire-and-forget) are dropped.
            if (!waiting_.erase(number)) return true;
            replies_.emplace(number, std::move(object));
        }
        cv_.notify_all();
        return true;
    }
    void send(std::string message) {
        asio::post(io_, [this, message = std::make_shared<std::string>(std::move(message))]() mutable {
            outbox_.push_back(std::move(message));
            if (outbox_.size() == 1) write();
        });
    }
    void write() {
        socket_.async_write(asio::buffer(*outbox_.front()), [this](beast::error_code ec, std::size_t) {
            outbox_.pop_front();
            if (ec) { outbox_.clear(); fail(Error(502, "browser_disconnected", "Browser CDP connection failed")); return; }
            if (!outbox_.empty()) write();
        });
    }
    json::object exchange(std::string message, std::uint64_t id, std::string_view method, const std::string& session, std::stop_token token, std::chrono::seconds timeout) {
        check_cancel(token);
        {
            std::lock_guard lock(mutex_);
            if (failure_) throw *failure_;
            // A crashed page never answers; fail at once instead of timing out.
            if (!session.empty() && crashed_.contains(session)) throw crashed();
            waiting_.insert(id);
        }
        send(std::move(message));
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, token, Clock::now() + timeout, [&] {
            return replies_.contains(id) || failure_ || (!session.empty() && crashed_.contains(session));
        });
        // Cancelling or timing out abandons only this call: the connection
        // stays usable and a late reply is dropped.
        waiting_.erase(id);
        auto node = replies_.extract(id);
        if (!node) {
            if (failure_) throw *failure_;
            if (!session.empty() && crashed_.contains(session)) throw crashed();
            lock.unlock();
            check_cancel(token);
            throw Error(504, "browser_timeout", "CDP command timed out");
        }
        lock.unlock();
        auto& object = node.mapped();
        if (!session.empty() && string_or(object, "sessionId") != session) { close(); throw Error(502, "browser_incompatible", "CDP reply belongs to a different page session"); }
        if (object.if_contains("error")) throw Error(502, "browser_incompatible", "Browser rejected CDP method " + std::string(method));
        auto result = object.if_contains("result");
        return result && result->is_object() ? std::move(result->as_object()) : json::object{};
    }
    // Runs one operation on the calling thread; only used before the reader starts.
    template<class Begin> void perform(Begin begin, std::stop_token token, std::chrono::seconds timeout) {
        check_cancel(token);
        bool done = false;
        beast::error_code error;
        io_.restart();
        begin([&](beast::error_code ec, auto...) { error = ec; done = true; });
        const auto deadline = Clock::now() + timeout;
        while (!done && !token.stop_requested() && Clock::now() < deadline) { io_.run_for(25ms); io_.restart(); }
        if (!done) {
            shutdown_socket();
            while (!done) { io_.run_for(25ms); io_.restart(); }
            check_cancel(token);
            throw Error(504, "browser_timeout", "Browser communication timed out");
        }
        if (error) { shutdown_socket(); throw Error(502, "browser_disconnected", "Browser CDP connection failed"); }
    }
};


} // namespace ais2api::browser_detail
