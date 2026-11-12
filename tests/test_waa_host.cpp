// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The embedded SpiderMonkey and the loop thread that owns it, offline.
#ifdef AIS2API_HAS_SPIDERMONKEY
#include "../waa/src/js.hpp"
#include "../waa/src/loop.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <future>
#include <memory>
#include <thread>

using namespace ais2api;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
namespace js = waa::js;
namespace {
std::string text_of(const js::Value &value) {
    switch (value.kind) {
    case js::Kind::undefined: return "undefined";
    case js::Kind::null: return "null";
    case js::Kind::boolean: return value.boolean ? "true" : "false";
    case js::Kind::number: {
        const auto whole = static_cast<long long>(value.number);
        return static_cast<double>(whole) == value.number ? std::to_string(whole) : std::to_string(value.number);
    }
    case js::Kind::string: return value.string;
    case js::Kind::handle: return "[object]";
    }
    return {};
}

// A loop thread to run engine code on, as the runtime does.
struct Thread {
    waa::EventLoop loop;
    // Runs `body` on the loop's thread and returns, or rethrows, its outcome.
    template <typename Body> auto run(Body body) -> decltype(body()) {
        auto task = std::make_shared<std::packaged_task<decltype(body())()>>(std::move(body));
        auto outcome = task->get_future();
        REQUIRE(loop.post([task] { (*task)(); }));
        return outcome.get();
    }
};

// A bare context with one global.
struct Engine : Thread {
    std::unique_ptr<js::Context> context;
    js::Handle global = 0;
    explicit Engine(std::string_view timezone = "UTC") {
        run([&] {
            js::set_locale_override("en", "US", "en-US");
            context = std::make_unique<js::Context>(64u << 20, "en-US");
            loop.cleanup = [this] { context.reset(); };
            global = context->new_global(0, timezone);
        });
    }
    ~Engine() { loop.join(); }
    js::Value evaluate(std::string_view source, std::string_view name = "https://example.test/page.js") {
        return run([&] { return context->evaluate(global, source, name); });
    }
    std::string text(std::string_view source) { return text_of(evaluate(source)); }
};
} // namespace

TEST_CASE("SpiderMonkey evaluates scripts and hands values across", "[waa][spidermonkey]") {
    Engine engine;
    CHECK(engine.text("1 + 1") == "2");
    CHECK(engine.text("'a' + 'b'") == "ab");
    CHECK(engine.text("0.5") == std::to_string(0.5));
    CHECK(engine.text("true") == "true");
    CHECK(engine.text("null") == "null");
    CHECK(engine.text("undefined") == "undefined");
    CHECK(engine.text("({})") == "[object]");
    // UTF-8 on this side, UTF-16 in the script.
    CHECK(engine.text("'\xC3\xA9\xF0\x9F\x98\x80'.length") == "3");
    CHECK(engine.text("'\\u00e9\\u{1F600}'") == "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(engine.text("typeof Intl.DateTimeFormat") == "function");

    engine.run([&] {
        auto &context = *engine.context;
        const auto object = context.new_object(engine.global);
        context.set(object, "name", js::Value::of(std::string("value")));
        context.set(object, "count", js::Value::of(3.0));
        CHECK(text_of(context.get(object, "name")) == "value");
        CHECK(text_of(context.get(object, "count")) == "3");
        CHECK(text_of(context.get(object, "missing")) == "undefined");
        const auto parsed = context.parse_json(engine.global, R"({"list":[1,2,3]})");
        REQUIRE(parsed.kind == js::Kind::handle);
        context.set(engine.global, "parsed", parsed);
        context.gc();
        context.gc(true);
        // Rooted handles survive a collection.
        CHECK(text_of(context.get(object, "name")) == "value");
        context.release(object);
        context.release(parsed);
    });
    CHECK(engine.text("parsed.list.length") == "3");
}

TEST_CASE("A script's exception arrives with its message, stack and place", "[waa][spidermonkey]") {
    Engine engine;
    engine.run([&] {
        try {
            engine.context->evaluate(engine.global, "\nfunction fail() { throw new TypeError('boom'); }\nfail();", "https://example.test/fail.js");
            FAIL("no exception");
        } catch (const js::Exception &exception) {
            CHECK_THAT(exception.message, ContainsSubstring("boom"));
            CHECK_THAT(exception.stack, ContainsSubstring("fail@https://example.test/fail.js:2:"));
            CHECK(exception.filename == "https://example.test/fail.js");
            CHECK(exception.line == 2);
            CHECK(exception.value != 0);
            engine.context->release(exception.value);
        }
    });
    // The context goes on working.
    CHECK(engine.text("new Error('x').stack.split('\\n')[0]") == "@https://example.test/page.js:1:1");
}

TEST_CASE("Native functions take arguments, return values and throw", "[waa][spidermonkey]") {
    Engine engine;
    engine.run([&] {
        auto &context = *engine.context;
        const auto define = [&](std::string_view name, unsigned arity, js::Native native) {
            const auto function = context.new_function(engine.global, name, arity, std::move(native));
            context.set(engine.global, name, js::Value::object(function));
            context.release(function);
        };
        define("add", 2, [](js::Call &call) { call.set_return(js::Value::of(call.arg_number(0) + call.arg_number(1))); });
        define("greet", 1, [](js::Call &call) { call.set_return(js::Value::of("hello " + call.arg_string(0))); });
        define("reversed", 1, [](js::Call &call) {
            auto bytes = call.arg_bytes(0);
            if (!bytes) throw js::Throw{"TypeError", "not bytes"};
            call.return_bytes(std::string(bytes->rbegin(), bytes->rend()));
        });
        define("refuse", 0, [](js::Call &) { throw js::Throw{"RangeError", "out of range"}; });
        define("broken", 0, [](js::Call &) { throw std::runtime_error("host failure"); });
    });
    CHECK(engine.text("add(2, 3)") == "5");
    CHECK(engine.text("add.length + ':' + add.name") == "2:add");
    CHECK(engine.text("greet('w\\u00f6rld')") == "hello w\xC3\xB6rld");
    CHECK(engine.text("Array.from(new Uint8Array(reversed(new Uint8Array([1, 2, 3])))).join()") == "3,2,1");
    CHECK(engine.text("Array.from(new Uint8Array(reversed(new Uint8Array([1, 2, 3, 4]).subarray(1, 3)))).join()") == "3,2");
    CHECK(engine.text("try { reversed('text') } catch (error) { (error instanceof TypeError) + ':' + error.message }") == "true:not bytes");
    CHECK(engine.text("try { refuse() } catch (error) { (error instanceof RangeError) + ':' + error.message }") == "true:out of range");
    CHECK(engine.text("try { broken() } catch (error) { error.constructor.name + ':' + error.message }") == "Error:host failure");
}

TEST_CASE("Microtasks wait for a checkpoint", "[waa][spidermonkey]") {
    Engine engine;
    engine.evaluate("globalThis.order = ['script']; Promise.resolve().then(() => order.push('microtask')); order.push('end');");
    CHECK(engine.text("order.join()") == "script,end");
    engine.run([&] { engine.context->run_jobs(); });
    CHECK(engine.text("order.join()") == "script,end,microtask");
}

TEST_CASE("A running script is interrupted from another thread", "[waa][spidermonkey]") {
    Engine engine;
    std::jthread interrupter([&] {
        std::this_thread::sleep_for(200ms);
        engine.context->interrupt();
    });
    CHECK_THROWS(engine.evaluate("for (;;) {}"));
    CHECK(engine.text("1 + 1") == "2");
}

TEST_CASE("A realm has its own time zone, and the context its locale", "[waa][spidermonkey]") {
    Engine engine("Asia/Tokyo");
    CHECK(engine.text("Intl.DateTimeFormat().resolvedOptions().timeZone") == "Asia/Tokyo");
    CHECK(engine.text("new Date(0).getHours()") == "9");
    CHECK(engine.text("Intl.DateTimeFormat().resolvedOptions().locale") == "en-US");
    CHECK(engine.text("(1234.5).toLocaleString()") == "1,234.5");
}
#endif
