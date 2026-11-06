// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../browser/bidi.hpp"
#include <catch2/catch_test_macros.hpp>
#include <future>

using namespace ais2api;
using browser_detail::Bidi;
using browser_detail::remote_value_json;
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

// A real loopback websocket peer that runs a scripted conversation.
struct Peer {
    struct Channel {
        websocket::stream<tcp::socket>& ws;
        std::string raw;
        json::object read() {
            beast::flat_buffer buffer;
            ws.read(buffer);
            raw = beast::buffers_to_string(buffer.data());
            return json::parse(raw).as_object();
        }
        void send(const json::object& message) { send_text(json::serialize(message)); }
        void send_text(std::string_view text) { ws.write(asio::buffer(text)); }
    };
    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    unsigned short port = acceptor.local_endpoint().port();
    std::jthread worker;
    template<class Script> explicit Peer(Script script) : worker([this, script] {
        try {
            websocket::stream<tcp::socket> ws{io};
            acceptor.accept(ws.next_layer());
            ws.accept();
            Channel channel{ws, {}};
            script(channel);
        } catch (...) {}
    }) {}
};
json::object success(const json::value& id, json::object result) {
    return {{"type", "success"}, {"id", id}, {"result", std::move(result)}};
}
json::object script_result(json::object remote) {
    return {{"type", "success"}, {"result", std::move(remote)}, {"realm", "realm-1"}};
}
}

TEST_CASE("BiDi remote values become plain JSON like returnByValue", "[bidi]") {
    CHECK(remote_value_json(json::parse(R"({"type":"string","value":"text"})")) == "text");
    CHECK(remote_value_json(json::parse(R"({"type":"boolean","value":true})")) == true);
    CHECK(remote_value_json(json::parse(R"({"type":"number","value":42})")) == 42);
    CHECK(remote_value_json(json::parse(R"({"type":"number","value":1.5})")) == 1.5);
    CHECK(remote_value_json(json::parse(R"({"type":"number","value":"NaN"})")).is_null());
    CHECK(remote_value_json(json::parse(R"({"type":"number","value":"-Infinity"})")).is_null());
    CHECK(remote_value_json(json::parse(R"({"type":"number","value":"-0"})")) == 0);
    CHECK(remote_value_json(json::parse(R"({"type":"bigint","value":"12345678901234567890"})")) == "12345678901234567890");
    CHECK(remote_value_json(json::parse(R"({"type":"undefined"})")).is_null());
    CHECK(remote_value_json(json::parse(R"({"type":"null"})")).is_null());
    CHECK(remote_value_json(json::parse(R"({"type":"function","handle":"h"})")).is_null());
    CHECK(remote_value_json(json::parse(R"({"type":"node","sharedId":"n"})")).is_null());
    const auto nested = remote_value_json(json::parse(R"({"type":"object","value":[
        ["pending",{"type":"object","value":[["sequence",{"type":"number","value":3}],["bytes",{"type":"string","value":"QUJD"}]]}],
        ["done",{"type":"boolean","value":false}],
        ["list",{"type":"array","value":[{"type":"number","value":1},{"type":"undefined"},{"type":"set","value":[{"type":"string","value":"s"}]}]}],
        [{"type":"number","value":1},{"type":"string","value":"non-string key"}],
        ["map",{"type":"map","value":[["k",{"type":"string","value":"v"}]]}]]})"));
    CHECK(nested == json::parse(R"({"pending":{"sequence":3,"bytes":"QUJD"},"done":false,"list":[1,null,["s"]],"map":{"k":"v"}})"));
}

TEST_CASE("BiDi correlates replies by id through events and dropped replies", "[bidi]") {
    std::promise<json::object> received;
    Peer peer([&](Peer::Channel& channel) {
        auto request = channel.read();
        received.set_value(request);
        channel.send({{"type", "event"}, {"method", "log.entryAdded"}, {"params", json::object{}}});
        channel.send({{"type", "event"}, {"method", "browsingContext.contextDestroyed"}, {"params", {{"context", "other-context"}}}});
        channel.send(success(999, {{"ignored", true}}));
        channel.send(success(request.at("id"), script_result({{"type", "string"}, {"value", "bidi-reply"}})));
        std::this_thread::sleep_for(200ms);
    });
    Bidi client;
    client.connect(peer.port, "/session", {});
    const auto value = client.evaluate("1 + 1", "context-1", {});
    REQUIRE(value.is_string());
    CHECK(value.as_string() == "bidi-reply");
    auto request = received.get_future().get();
    CHECK(string_or(request, "method") == "script.evaluate");
    const auto& params = request.at("params").as_object();
    CHECK(string_or(params, "expression") == "1 + 1");
    CHECK(string_or(params.at("target").as_object(), "context") == "context-1");
    CHECK(bool_or(params, "awaitPromise"));
    CHECK(string_or(params, "resultOwnership") == "none");
}

TEST_CASE("BiDi escapes scripts and string arguments exactly like the JSON serializer", "[bidi]") {
    const std::string text = "quote\" backslash\\ newline\n tab\t nul" + std::string(1, '\0') + " \x01 unicode \xE4\xB8\xAD </script>";
    std::promise<std::pair<std::string, std::string>> received;
    Peer peer([&](Peer::Channel& channel) {
        auto first = channel.read();
        const auto evaluate = channel.raw;
        channel.send(success(first.at("id"), script_result({{"type", "undefined"}})));
        auto second = channel.read();
        received.set_value({evaluate, channel.raw});
        channel.send(success(second.at("id"), script_result({{"type", "number"}, {"value", 2}})));
        std::this_thread::sleep_for(200ms);
    });
    Bidi client;
    client.connect(peer.port, "/session", {});
    CHECK(client.evaluate(text, "context-\"1\"", {}).is_null());
    CHECK(client.call_function("function(id, chunk) { return this.append(id, chunk); }", {"upload-1", text}, "context-\"1\"", {}) == 2);
    auto [evaluate, call] = received.get_future().get();
    auto evaluated = json::parse(evaluate).as_object();
    CHECK(evaluated.at("params").as_object().at("expression") == json::value(text));
    CHECK(evaluate.find(json::serialize(json::value(text))) != std::string::npos);
    auto called = json::parse(call).as_object();
    const auto& params = called.at("params").as_object();
    CHECK(string_or(params, "functionDeclaration") == "function(id, chunk) { return this.append(id, chunk); }");
    CHECK(params.at("arguments") == json::array{json::object{{"type", "string"}, {"value", "upload-1"}}, json::object{{"type", "string"}, {"value", text}}});
    CHECK(string_or(params.at("target").as_object(), "context") == "context-\"1\"");
}

TEST_CASE("BiDi errors keep their code and map to gateway errors", "[bidi]") {
    Peer peer([&](Peer::Channel& channel) {
        auto request = channel.read();
        channel.send({{"type", "error"}, {"id", request.at("id")}, {"error", "unknown error"}, {"message", "NS_ERROR_UNKNOWN_HOST"}});
        request = channel.read();
        channel.send({{"type", "error"}, {"id", request.at("id")}, {"error", "no such frame"}, {"message", "gone"}});
        request = channel.read();
        channel.send({{"type", "error"}, {"id", request.at("id")}, {"error", "unknown error"}, {"message", "Browsing context has been discarded"}});
        request = channel.read();
        channel.send({{"type", "error"}, {"id", request.at("id")}, {"error", "invalid argument"}, {"message", "bad"}});
        request = channel.read();
        channel.send(success(request.at("id"), {{"type", "exception"}, {"exceptionDetails", {{"text", "Error: boom"}}}, {"realm", "r"}}));
        std::this_thread::sleep_for(200ms);
    });
    Bidi client;
    client.connect(peer.port, "/session", {});
    auto reply = client.exchange("browsingContext.navigate", {{"context", "c"}, {"url", "https://example.invalid/"}}, {});
    CHECK(reply.error == "unknown error");
    CHECK(reply.message == "NS_ERROR_UNKNOWN_HOST");
    auto code = [&](auto&& call) {
        try { call(); } catch (const Error& error) { return error.code; }
        return std::string("no error");
    };
    CHECK(code([&] { client.call("script.evaluate", {}, {}); }) == "browser_disconnected");
    // A navigation that replaced the realm is retried like CDP's lost
    // execution context, not treated as a closed page.
    CHECK(code([&] { client.call("script.evaluate", {}, {}); }) == "browser_incompatible");
    CHECK(code([&] { client.call("storage.setCookie", {}, {}); }) == "browser_incompatible");
    CHECK(code([&] { client.evaluate("throw new Error('boom')", "c", {}); }) == "browser_incompatible");
    CHECK(client.open());
}

TEST_CASE("BiDi keeps the connection after a cancelled or timed-out call", "[bidi]") {
    Peer peer([&](Peer::Channel& channel) {
        auto abandoned = channel.read();
        auto cancelled = channel.read();
        auto request = channel.read();
        // Late replies to abandoned calls are dropped.
        channel.send(success(abandoned.at("id"), script_result({{"type", "string"}, {"value", "late"}})));
        channel.send(success(cancelled.at("id"), script_result({{"type", "string"}, {"value", "late"}})));
        channel.send(success(request.at("id"), script_result({{"type", "string"}, {"value", "fresh"}})));
        std::this_thread::sleep_for(200ms);
    });
    Bidi client;
    client.connect(peer.port, "/session", {});
    try {
        client.evaluate("slow()", "c", {}, 1s);
        FAIL("expected a timeout");
    } catch (const Error& error) { CHECK(error.code == "browser_timeout"); }
    std::stop_source source;
    std::jthread canceller([&] { std::this_thread::sleep_for(200ms); source.request_stop(); });
    const auto started = Clock::now();
    try {
        client.evaluate("slow()", "c", source.get_token(), 30s);
        FAIL("expected cancellation");
    } catch (const Error& error) { CHECK(error.code == "cancelled"); }
    CHECK(Clock::now() - started < 5s);
    CHECK(client.evaluate("fresh()", "c", {}) == "fresh");
}

TEST_CASE("BiDi fails a waiting call at once when the browser disconnects", "[bidi]") {
    Peer peer([&](Peer::Channel& channel) {
        channel.read();
        channel.ws.next_layer().close();
    });
    Bidi client;
    client.connect(peer.port, "/session", {});
    const auto started = Clock::now();
    try {
        client.evaluate("never()", "c", {}, 30s);
        FAIL("expected a disconnect");
    } catch (const Error& error) { CHECK(error.code == "browser_disconnected"); }
    CHECK(Clock::now() - started < 5s);
    CHECK_FALSE(client.open());
}

TEST_CASE("BiDi fails calls on a destroyed browsing context at once", "[bidi]") {
    Peer peer([&](Peer::Channel& channel) {
        channel.read();
        channel.send({{"type", "event"}, {"method", "browsingContext.contextDestroyed"}, {"params", {{"context", "other-context"}}}});
        channel.send({{"type", "event"}, {"method", "browsingContext.contextDestroyed"}, {"params", {{"context", "context-1"}}}});
        auto request = channel.read();
        channel.send(success(request.at("id"), {{"userContext", "user-1"}}));
        std::this_thread::sleep_for(500ms);
    });
    Bidi client;
    client.connect(peer.port, "/session", {});
    const auto started = Clock::now();
    try {
        client.evaluate("never()", "context-1", {}, 30s);
        FAIL("expected a lost page");
    } catch (const Error& error) { CHECK(error.code == "browser_disconnected"); }
    CHECK(Clock::now() - started < 5s);
    // Later calls on that page fail without a round trip; others still work.
    try {
        client.evaluate("again()", "context-1", {}, 30s);
        FAIL("expected a lost page");
    } catch (const Error& error) { CHECK(error.code == "browser_disconnected"); }
    CHECK(string_or(client.call("browser.createUserContext", {}, {}), "userContext") == "user-1");
}
