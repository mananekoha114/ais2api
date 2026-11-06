// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
// A WebDriver BiDi client for a browser this process launched. It depends on
// Boost and ais2api/common.hpp only, never on the server, configuration or
// account store.
#include "ais2api/common.hpp"
#include "json_text.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
namespace ais2api::browser_detail {
namespace bidi_net {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
} // namespace bidi_net

// A BiDi RemoteValue as plain JSON, like CDP's returnByValue: arrays and sets
// become arrays, objects and maps objects (string keys only), bigints strings,
// non-finite numbers null and -0 zero. Other values (nodes, functions, ...)
// become null.
inline json::value remote_value_json(const json::value& remote) {
    if (!remote.is_object()) return nullptr;
    const auto& object = remote.as_object();
    const auto type = string_or(object, "type");
    const auto* value = object.if_contains("value");
    if (type == "string" || type == "boolean") return value ? *value : json::value{};
    if (type == "bigint") return value && value->is_string() ? *value : json::value{};
    if (type == "number") {
        if (!value) return nullptr;
        if (value->is_string()) return value->as_string() == "-0" ? json::value(0) : json::value{};
        if (value->is_double() && !std::isfinite(value->as_double())) return nullptr;
        return *value;
    }
    if (type == "array" || type == "set") {
        json::array result;
        if (value && value->is_array()) for (const auto& item : value->as_array()) result.push_back(remote_value_json(item));
        return result;
    }
    if (type == "object" || type == "map") {
        json::object result;
        if (value && value->is_array()) for (const auto& entry : value->as_array()) {
            if (!entry.is_array() || entry.as_array().size() != 2 || !entry.as_array()[0].is_string()) continue;
            result[entry.as_array()[0].as_string()] = remote_value_json(entry.as_array()[1]);
        }
        return result;
    }
    return nullptr;
}

// A reader thread owns the websocket and drains every message; replies are
// matched by id. Calls come from one thread at a time.
class Bidi {
public:
    // A command's outcome: `result` on success, otherwise the BiDi error code
    // and message.
    struct Reply {
        json::object result;
        std::string error, message;
    };
    Bidi() = default;
    Bidi(const Bidi&) = delete;
    Bidi& operator=(const Bidi&) = delete;
    ~Bidi() {
        close();
        io_.stop();
        if (reader_.joinable()) reader_.join();
        shutdown_socket();
    }
    void connect(unsigned short port, std::string path, std::stop_token token) {
        using namespace bidi_net;
        perform([&](auto callback) { beast::get_lowest_layer(socket_).async_connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), callback); }, token, std::chrono::seconds(5));
        socket_.read_message_max(4 * 1024 * 1024);
        perform([&](auto callback) { socket_.async_handshake("127.0.0.1:" + std::to_string(port), path, callback); }, token, std::chrono::seconds(5));
        io_.restart();
        work_.emplace(asio::make_work_guard(io_));
        read();
        reader_ = std::thread([this] { io_.run(); });
    }
    bool open() {
        std::lock_guard lock(mutex_);
        return reader_.joinable() && !failure_;
    }
    // Sends a command that concerns `context` (empty: none). A BiDi error is
    // returned, not thrown; a lost connection, a destroyed context, a timeout
    // and cancellation throw.
    Reply exchange(std::string method, json::object params, std::stop_token token, std::chrono::seconds timeout = std::chrono::seconds(30),
                   const std::string& context = {}) {
        const auto id = next_id();
        auto message = json::serialize(json::object{{"id", id}, {"method", method}, {"params", std::move(params)}});
        return transact(std::move(message), id, token, timeout, context);
    }
    json::object call(std::string method, json::object params, std::stop_token token, std::chrono::seconds timeout = std::chrono::seconds(30),
                      const std::string& context = {}) {
        auto reply = exchange(method, std::move(params), token, timeout, context);
        if (!reply.error.empty()) throw command_error(method, reply);
        return std::move(reply.result);
    }
    // Evaluates `expression` in the context's page realm and returns its
    // awaited value as plain JSON.
    json::value evaluate(std::string expression, const std::string& context, std::stop_token token,
                         std::chrono::seconds timeout = std::chrono::seconds(30)) {
        const auto id = next_id();
        // Built as text so a large expression is escaped once instead of being
        // copied into a JSON tree and serialized.
        std::string message = "{\"id\":" + std::to_string(id) + ",\"method\":\"script.evaluate\",\"params\":{\"expression\":";
        message.reserve(message.size() + json_size(json::string_view(expression)) + json_size(json::string_view(context)) + 96);
        append_quoted(message, expression);
        std::string().swap(expression);
        message += ",\"target\":{\"context\":";
        append_quoted(message, context);
        message += "},\"awaitPromise\":true,\"resultOwnership\":\"none\"}}";
        return script_value("script.evaluate", transact(std::move(message), id, token, timeout, context));
    }
    // Calls `declaration` in the context's page realm. The arguments travel as
    // protocol string values, never as script source.
    json::value call_function(std::string_view declaration, std::initializer_list<std::string_view> arguments, const std::string& context,
                              std::stop_token token, std::chrono::seconds timeout = std::chrono::seconds(30)) {
        const auto id = next_id();
        auto size = json_size(json::string_view(declaration)) + json_size(json::string_view(context)) + 160;
        for (auto argument : arguments) size += json_size(json::string_view(argument)) + 28;
        std::string message = "{\"id\":" + std::to_string(id) + ",\"method\":\"script.callFunction\",\"params\":{\"functionDeclaration\":";
        message.reserve(message.size() + size);
        append_quoted(message, declaration);
        message += ",\"arguments\":[";
        bool first = true;
        for (auto argument : arguments) {
            if (!first) message += ',';
            first = false;
            message += "{\"type\":\"string\",\"value\":";
            append_quoted(message, argument);
            message += '}';
        }
        message += "],\"target\":{\"context\":";
        append_quoted(message, context);
        message += "},\"awaitPromise\":true,\"resultOwnership\":\"none\"}}";
        return script_value("script.callFunction", transact(std::move(message), id, token, timeout, context));
    }
    // A page or user context that no longer exists is a lost browser page.
    // Anything else means the browser did not do what was asked, including a
    // script whose realm a navigation replaced ("unknown error" naming a
    // destroyed actor or discarded context), which CDP reports the same way.
    static Error command_error(std::string_view method, const Reply& reply) {
        if (reply.error == "no such frame" || reply.error == "no such browsing context" || reply.error == "no such user context")
            return Error(502, "browser_disconnected", "Browser page was closed or discarded");
        return Error(502, "browser_incompatible", "Browser rejected BiDi method " + std::string(method) + " (" + reply.error + ")");
    }
    // Fails waiting and later calls at once; the reader stops with the socket.
    void close() noexcept {
        try {
            {
                std::lock_guard lock(mutex_);
                if (!failure_) failure_ = Error(502, "browser_disconnected", "Browser BiDi connection closed");
            }
            cv_.notify_all();
            if (reader_.joinable()) bidi_net::asio::post(io_, [this] { shutdown_socket(); });
            else shutdown_socket();
        } catch (...) {}
    }
private:
    bidi_net::asio::io_context io_;
    bidi_net::websocket::stream<bidi_net::beast::tcp_stream> socket_{io_};
    std::optional<bidi_net::asio::executor_work_guard<bidi_net::asio::io_context::executor_type>> work_;
    std::thread reader_;
    // Used only on the reader thread.
    bidi_net::beast::flat_buffer buffer_;
    std::deque<std::shared_ptr<std::string>> outbox_;
    // Shared with callers.
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::uint64_t next_ = 0;
    std::set<std::uint64_t> waiting_;
    std::map<std::uint64_t, json::object> replies_;
    std::set<std::string> destroyed_;
    std::optional<Error> failure_;
    static Error lost() { return Error(502, "browser_disconnected", "Browser page was closed or crashed"); }
    std::uint64_t next_id() { std::lock_guard lock(mutex_); return ++next_; }
    void shutdown_socket() noexcept { bidi_net::beast::error_code ec; bidi_net::beast::get_lowest_layer(socket_).socket().close(ec); }
    void fail(Error error) {
        {
            std::lock_guard lock(mutex_);
            if (!failure_) failure_ = std::move(error);
        }
        cv_.notify_all();
        shutdown_socket();
    }
    // script.evaluate and script.callFunction succeed as commands even when
    // the script throws.
    static json::value script_value(std::string_view method, const Reply& reply) {
        if (!reply.error.empty()) throw command_error(method, reply);
        if (string_or(reply.result, "type") == "exception") throw Error(502, "browser_incompatible", "Page bridge failed or current AI Studio page is unsupported");
        const auto* remote = reply.result.if_contains("result");
        if (!remote || !remote->is_object()) throw Error(502, "browser_incompatible", "Missing BiDi script result");
        return remote_value_json(*remote);
    }
    // Reader thread: exactly one read is outstanding until the socket fails.
    void read() {
        socket_.async_read(buffer_, [this](bidi_net::beast::error_code ec, std::size_t) {
            if (ec) { fail(Error(502, "browser_disconnected", "Browser BiDi connection failed")); return; }
            const bool valid = dispatch();
            buffer_.consume(buffer_.size());
            if (buffer_.capacity() > 64 * 1024) buffer_.shrink_to_fit();
            if (valid) read();
        });
    }
    bool dispatch() {
        const auto data = buffer_.cdata();
        bidi_net::beast::error_code parse_error;
        auto value = json::parse(std::string_view(static_cast<const char*>(data.data()), data.size()), parse_error);
        if (parse_error || !value.is_object()) { fail(Error(502, "browser_incompatible", "Invalid BiDi message")); return false; }
        auto& object = value.as_object();
        const auto type = string_or(object, "type");
        if (type == "event") {
            if (string_or(object, "method") == "browsingContext.contextDestroyed") {
                const auto* params = object.if_contains("params");
                const auto context = params && params->is_object() ? string_or(params->as_object(), "context") : std::string{};
                if (!context.empty()) {
                    { std::lock_guard lock(mutex_); destroyed_.insert(context); }
                    cv_.notify_all();
                }
            }
            return true;
        }
        auto id = object.if_contains("id");
        if (!id || !id->is_int64() || id->as_int64() <= 0) {
            // An error without an id concerns no command: the browser could
            // not parse what it was sent.
            if (type == "error") { fail(Error(502, "browser_incompatible", "Browser rejected a BiDi message")); return false; }
            return true;
        }
        const auto number = static_cast<std::uint64_t>(id->as_int64());
        {
            std::lock_guard lock(mutex_);
            // Replies nobody waits for (abandoned calls) are dropped.
            if (!waiting_.erase(number)) return true;
            replies_.emplace(number, std::move(object));
        }
        cv_.notify_all();
        return true;
    }
    void send(std::string message) {
        bidi_net::asio::post(io_, [this, message = std::make_shared<std::string>(std::move(message))]() mutable {
            outbox_.push_back(std::move(message));
            if (outbox_.size() == 1) write();
        });
    }
    void write() {
        socket_.async_write(bidi_net::asio::buffer(*outbox_.front()), [this](bidi_net::beast::error_code ec, std::size_t) {
            outbox_.pop_front();
            if (ec) { outbox_.clear(); fail(Error(502, "browser_disconnected", "Browser BiDi connection failed")); return; }
            if (!outbox_.empty()) write();
        });
    }
    Reply transact(std::string message, std::uint64_t id, std::stop_token token, std::chrono::seconds timeout, const std::string& context) {
        check_cancel(token);
        {
            std::lock_guard lock(mutex_);
            if (failure_) throw *failure_;
            // A destroyed page never answers; fail at once instead of timing out.
            if (!context.empty() && destroyed_.contains(context)) throw lost();
            waiting_.insert(id);
        }
        send(std::move(message));
        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, token, Clock::now() + timeout, [&] {
            return replies_.contains(id) || failure_ || (!context.empty() && destroyed_.contains(context));
        });
        // Cancelling or timing out abandons only this call: the connection
        // stays usable and a late reply is dropped.
        waiting_.erase(id);
        auto node = replies_.extract(id);
        if (!node) {
            if (failure_) throw *failure_;
            if (!context.empty() && destroyed_.contains(context)) throw lost();
            lock.unlock();
            check_cancel(token);
            throw Error(504, "browser_timeout", "BiDi command timed out");
        }
        lock.unlock();
        auto& object = node.mapped();
        Reply reply;
        if (string_or(object, "type") == "success") {
            if (auto* result = object.if_contains("result"); result && result->is_object()) reply.result = std::move(result->as_object());
        } else {
            reply.error = string_or(object, "error");
            reply.message = string_or(object, "message");
            if (reply.error.empty()) reply.error = "unknown error";
        }
        return reply;
    }
    // Runs one operation on the calling thread; only used before the reader starts.
    template<class Begin> void perform(Begin begin, std::stop_token token, std::chrono::seconds timeout) {
        check_cancel(token);
        bool done = false;
        bidi_net::beast::error_code error;
        io_.restart();
        begin([&](bidi_net::beast::error_code ec, auto...) { error = ec; done = true; });
        const auto deadline = Clock::now() + timeout;
        while (!done && !token.stop_requested() && Clock::now() < deadline) { io_.run_for(std::chrono::milliseconds(25)); io_.restart(); }
        if (!done) {
            shutdown_socket();
            while (!done) { io_.run_for(std::chrono::milliseconds(25)); io_.restart(); }
            check_cancel(token);
            throw Error(504, "browser_timeout", "Browser communication timed out");
        }
        if (error) { shutdown_socket(); throw Error(502, "browser_disconnected", "Browser BiDi connection failed"); }
    }
};

} // namespace ais2api::browser_detail
