// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The embedded SpiderMonkey, the Firefox-shaped host and the BotGuard
// lifecycle, offline: a stand-in interpreter takes the place of Google's.
#ifdef AIS2API_HAS_SPIDERMONKEY
#include "../waa/src/host.hpp"
#include "ais2api/waa/runtime.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <future>
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

waa::Profile page_profile() {
    waa::Profile profile;
    profile.page_url = "https://aistudio.google.com/prompts/new_chat?model=test#top";
    profile.locale = "en-US";
    profile.timezone = "Asia/Tokyo";
    profile.window = {{"innerWidth", 1280}, {"innerHeight", 720}, {"outerWidth", 1296}, {"outerHeight", 808}, {"devicePixelRatio", 1.25}};
    profile.navigator = {{"userAgent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:152.0) Gecko/20100101 Firefox/152.0"},
                         {"hardwareConcurrency", 8}};
    profile.screen = {{"width", 1920}, {"height", 1080}};
    return profile;
}

// The Firefox host with its top realm, without a BotGuard program.
struct Page : Thread {
    std::unique_ptr<waa::Agent> agent;
    std::mutex mutex;
    std::vector<std::string> image_loads;
    explicit Page(waa::Profile profile = page_profile()) {
        run([&] {
            const auto load_image = [this](std::string_view url, std::stop_token) {
                std::lock_guard lock(mutex);
                image_loads.emplace_back(url);
                return true;
            };
            agent = std::make_unique<waa::Agent>(loop, std::move(profile), load_image, nullptr, loop.closed());
            loop.cleanup = [this] { agent->close(); };
            loop.after_task = [this] { agent->after_task(); };
            try {
                agent->top = &agent->install_realm("top", 0);
            } catch (const js::Exception &exception) {
                agent->release(exception);
                throw std::runtime_error(exception.what());
            }
        });
    }
    ~Page() {
        loop.join();
        agent.reset();
    }
    // Evaluates a page script as one task.
    std::string text(std::string_view source) {
        return run([&] {
            try {
                const auto value = agent->evaluate(agent->top->global, source, "https://example.test/page.js");
                agent->context().release(value);
                return text_of(value);
            } catch (const js::Exception &exception) {
                agent->release(exception);
                throw std::runtime_error(exception.what());
            }
        });
    }
    // Lets the timers and tasks due within `wait` run.
    void idle(std::chrono::milliseconds wait) {
        std::promise<void> done;
        loop.schedule(wait, [&] { done.set_value(); });
        done.get_future().wait();
    }
    void settle() {
        std::promise<void> done;
        loop.post([&] { agent->when_settled([&] { done.set_value(); }); });
        REQUIRE(done.get_future().wait_for(10s) == std::future_status::ready);
    }
};

// A stand-in for Google's interpreter: the same entry point and callbacks,
// with a snapshot that reports what it was given and what the page shows.
constexpr std::string_view interpreter = R"js(
globalThis.standIn = {
  a(program, ready, flag, unused, callback, signals, stored, second, loggers) {
    globalThis.started = JSON.stringify([program, flag, typeof callback, signals, stored === undefined ? null : stored, second, loggers.length]);
    const snapshot = (deliver, options) => {
      const box = document.querySelector('ms-prompt-box');
      const bound = options[0] === undefined ? 'unbound' : options[0].content;
      const reply = () => deliver('!' + bound + '|' + (box ? box.querySelector('textarea').value : 'no box') + '|' + (globalThis.clicks || 0));
      if (program === 'slow') setTimeout(reply, 20);
      else if (program !== 'silent') reply();
    };
    document.addEventListener('click', () => { globalThis.clicks = (globalThis.clicks || 0) + 1; }, true);
    ready(snapshot, () => { globalThis.shutDown = true; });
  },
};
)js";

waa::Options options_for(std::string program, std::string_view source = interpreter) {
    waa::Options options;
    options.challenge.program = std::move(program);
    options.challenge.global_name = "standIn";
    options.challenge.interpreter_hash = waa::interpreter_hash(source);
    options.interpreter = std::string(source);
    options.profile = page_profile();
    return options;
}
std::string error_of(const std::function<void()> &body) {
    try {
        body();
    } catch (const Error &error) {
        return std::to_string(error.status) + " " + error.code + ": " + error.what();
    }
    return "no error";
}
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

TEST_CASE("The host looks like a Firefox page", "[waa][spidermonkey]") {
    Page page;
    CHECK(page.text("[typeof window, typeof document, typeof navigator, window === globalThis, window.top === window].join()") ==
          "object,object,object,true,true");
    CHECK(page.text("location.href") == "https://aistudio.google.com/prompts/new_chat?model=test#top");
    CHECK(page.text("[location.origin, location.pathname, location.search, location.hash].join(' ')") ==
          "https://aistudio.google.com /prompts/new_chat ?model=test #top");
    CHECK(page.text("navigator.userAgent") == "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:152.0) Gecko/20100101 Firefox/152.0");
    CHECK(page.text("[navigator.hardwareConcurrency, screen.width, screen.height, innerWidth, innerHeight, devicePixelRatio].join()") ==
          "8,1920,1080,1280,720,1.25");
    CHECK(page.text("Intl.DateTimeFormat().resolvedOptions().timeZone") == "Asia/Tokyo");
    CHECK(page.text("document.hasFocus()") == "true");

    SECTION("with functions that read as native code") {
        CHECK(page.text("Function.prototype.toString.call(setTimeout)") == "function setTimeout() {\n    [native code]\n}");
        CHECK(page.text("document.createElement.toString()") == "function createElement() {\n    [native code]\n}");
        CHECK(page.text("Function.prototype.toString.toString()") == "function toString() {\n    [native code]\n}");
        CHECK(page.text("(function own() { return 1; }).toString()") == "function own() { return 1; }");
    }
    SECTION("with host scripts kept out of stacks") {
        // A listener called by the host's dispatchEvent: no host frame between.
        const auto stack = page.text("let stack; const target = document.createElement('div');"
                                     "target.addEventListener('ping', function heard() { stack = new Error('x').stack; });"
                                     "(function send() { target.dispatchEvent(new Event('ping')); })(); stack");
        CHECK_THAT(stack, ContainsSubstring("heard@https://example.test/page.js"));
        CHECK_THAT(stack, ContainsSubstring("send@https://example.test/page.js"));
        CHECK_THAT(stack, !ContainsSubstring("self-hosted"));
    }
    SECTION("with an iframe that is another realm") {
        CHECK(page.text("const frame = document.createElement('iframe'); document.body.appendChild(frame);"
                        "const inner = frame.contentWindow;"
                        "[inner !== window, inner.Object !== Object, inner.parent === window, inner.Array.isArray(new inner.Array())].join()") ==
              "true,true,true,true");
    }
}

TEST_CASE("Host tasks run as a browser orders them", "[waa][spidermonkey]") {
    Page page;
    page.text("globalThis.order = [];"
              "setTimeout(() => order.push('timeout 10'), 10);"
              "setTimeout(() => { order.push('timeout 0'); Promise.resolve().then(() => order.push('microtask in task')); }, 0);"
              "const cancelled = setTimeout(() => order.push('cancelled'), 0); clearTimeout(cancelled);"
              "Promise.resolve().then(() => order.push('microtask'));"
              "queueMicrotask(() => order.push('queued'));"
              "queueMicrotask(() => { globalThis.checkpoint = order.join(); });"
              "order.push('script');");
    // The checkpoint follows the script; the timers are tasks of their own.
    // (A later evaluation is a task too, which a due timer may precede.)
    CHECK(page.text("checkpoint") == "script,microtask,queued");
    page.idle(100ms);
    CHECK(page.text("order.join()") == "script,microtask,queued,timeout 0,microtask in task,timeout 10");
}

TEST_CASE("The host's natives back its web interfaces", "[waa][spidermonkey]") {
    Page page;
    page.text("globalThis.digest = 'pending';"
              "crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc')).then(buffer => {"
              "  digest = Array.from(new Uint8Array(buffer), byte => byte.toString(16).padStart(2, '0')).join('');"
              "});");
    page.idle(50ms);
    CHECK(page.text("digest") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(page.text("new URL('../a?b#c', 'https://example.test/x/y').href") == "https://example.test/a?b#c");
    CHECK(page.text("const url = new URL('https://example.test/'); url.pathname = '/p q'; url.href") == "https://example.test/p%20q");
    CHECK(page.text("try { new URL('nonsense'); 'parsed' } catch (error) { error.constructor.name }") == "TypeError");
    CHECK(page.text("new TextDecoder('latin1').encoding") == "windows-1252");
    CHECK(page.text("new TextDecoder().decode(new Uint8Array([0xe2, 0x82, 0xac]))") == "\xE2\x82\xAC");
    CHECK(page.text("try { new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xff])); 'decoded' } catch (error) { error.constructor.name }") ==
          "TypeError");
    CHECK(page.text("crypto.randomUUID().length") == "36");
    CHECK(page.text("crypto.getRandomValues(new Uint8Array(16)).some(byte => byte !== 0)") == "true");
}

TEST_CASE("An image load goes to the caller and settles", "[waa][spidermonkey]") {
    Page page;
    page.text("globalThis.loaded = 'pending'; const image = new Image();"
              "image.onload = () => { loaded = 'load'; }; image.onerror = () => { loaded = 'error'; };"
              "image.src = 'https://example.test/generate_204?x=1';");
    page.settle();
    CHECK(page.text("loaded") == "load");
    std::lock_guard lock(page.mutex);
    CHECK(page.image_loads == std::vector<std::string>{"https://example.test/generate_204?x=1"});
}

TEST_CASE("A runtime starts the program and binds proofs to the page", "[waa][spidermonkey]") {
    auto options = options_for("PROGRAM");
    options.challenge.client_experiments_state_blob = R"([null,null,null,null,"stored",[[11,1],[22,54],[33,53],["bad",1],[44]]])";
    waa::Runtime runtime(std::move(options));
    runtime.settle();
    // The prompt is typed and Run clicked before the snapshot; a textarea
    // reads CRLF as LF.
    CHECK(runtime.proof("digest-1", "line one\r\nline two", true) == "!digest-1|line one\nline two|1");
    // The box was cleared in between; without Run nothing is clicked.
    CHECK(runtime.proof("digest-2", "second", false) == "!digest-2|second|1");
    CHECK(runtime.refresh_snapshot() == "!unbound||1");
}

TEST_CASE("A runtime passes the page's arguments to the program", "[waa][spidermonkey]") {
    // The snapshot reports what the initialisation function was called with.
    constexpr std::string_view reporting = R"js(
globalThis.standIn = { a(...given) {
  const [program, ready, flag, unused, callback, signals, stored, second, loggers] = given;
  const seen = JSON.stringify([given.length, program, flag, unused === undefined, typeof callback, signals, stored === undefined ? null : stored,
                               second, loggers.map(logger => typeof logger)]);
  ready(deliver => deliver(seen));
} };
)js";
    auto options = options_for("PROGRAM", reporting);
    SECTION("with the experiment signals split at key 53") {
        options.challenge.client_experiments_state_blob = R"([null,null,null,null,"stored",[[11,1],[22,54],[33,53],["bad",1],[44]]])";
        waa::Runtime runtime(std::move(options));
        CHECK(runtime.refresh_snapshot() ==
              R"([9,"PROGRAM",true,true,"function",[[11,33,22],[1,1,2]],"stored",false,["function","function","function","function"]])");
    }
    SECTION("without experiments") {
        waa::Runtime runtime(std::move(options));
        CHECK(runtime.refresh_snapshot() == R"([9,"PROGRAM",true,true,"function",[[],[]],null,false,["function","function","function","function"]])");
    }
}

TEST_CASE("A runtime reports what stops it", "[waa][spidermonkey]") {
    SECTION("an interpreter that throws") {
        const auto error = error_of([] { waa::Runtime runtime(options_for("PROGRAM", "throw new Error('interpreter refused');")); });
        CHECK_THAT(error, ContainsSubstring("502 waa_runtime_failed: Running the WAA interpreter: "));
        CHECK_THAT(error, ContainsSubstring("interpreter refused"));
    }
    SECTION("an interpreter that defines nothing") {
        CHECK(error_of([] { waa::Runtime runtime(options_for("PROGRAM", "1;")); }) ==
              "502 waa_runtime_failed: The WAA interpreter defined no global object");
    }
    SECTION("a program that hands over no snapshot function") {
        CHECK(error_of([] { waa::Runtime runtime(options_for("PROGRAM", "globalThis.standIn = { a(program, ready) { ready(null); } };")); }) ==
              "502 waa_runtime_failed: The WAA interpreter returned no snapshot function");
    }
    SECTION("a challenge without a program") {
        CHECK_THAT(error_of([] { waa::Runtime runtime(options_for("")); }), ContainsSubstring("502 waa_challenge_invalid"));
    }
    SECTION("a snapshot that throws") {
        waa::Runtime runtime(options_for("PROGRAM", "globalThis.standIn = { a(program, ready) { ready(() => { throw new Error('no snapshot today'); }); } };"));
        const auto error = error_of([&] { runtime.refresh_snapshot(); });
        CHECK_THAT(error, ContainsSubstring("502 waa_runtime_failed: Running the WAA snapshot: "));
        CHECK_THAT(error, ContainsSubstring("no snapshot today"));
        // The runtime stays usable.
        CHECK_THAT(error_of([&] { runtime.refresh_snapshot(); }), ContainsSubstring("no snapshot today"));
    }
}

TEST_CASE("A runtime gives up when its caller cancels", "[waa][spidermonkey]") {
    SECTION("a snapshot that never answers") {
        waa::Runtime runtime(options_for("silent"));
        std::stop_source source;
        std::jthread canceller([&] {
            std::this_thread::sleep_for(100ms);
            source.request_stop();
        });
        CHECK_THAT(error_of([&] { runtime.proof("digest", "prompt", true, source.get_token()); }), ContainsSubstring("499 cancelled"));
        // A later answer finds its own request.
    }
    SECTION("an interpreter that never returns") {
        std::stop_source source;
        std::jthread canceller([&] {
            std::this_thread::sleep_for(200ms);
            source.request_stop();
        });
        const auto started = std::chrono::steady_clock::now();
        CHECK_THAT(error_of([&] { waa::Runtime runtime(options_for("PROGRAM", "for (;;) {}"), source.get_token()); }),
                   ContainsSubstring("499 cancelled"));
        // Closing interrupts the script after its grace periods.
        CHECK(std::chrono::steady_clock::now() - started < 10s);
    }
}

TEST_CASE("A snapshot may answer in a later task", "[waa][spidermonkey]") {
    waa::Runtime runtime(options_for("slow"));
    CHECK(runtime.proof("digest", "prompt", true) == "!digest|prompt|1");
    CHECK(runtime.proof("digest", "again", true) == "!digest|again|2");
}

TEST_CASE("The embedded host files are identified for diagnostics", "[waa][spidermonkey]") {
    const auto versions = waa::host_versions();
    for (const std::string_view name : {"dom", "host", "shape"}) {
        REQUIRE(versions.contains(name));
        CHECK(versions.at(name).as_string().size() == 64);
    }
    CHECK(versions.at("spidermonkey").as_string() == "152.0.4");
}
#endif
