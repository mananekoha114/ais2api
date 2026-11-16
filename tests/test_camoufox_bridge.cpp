// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../browser/bidi.hpp"
#include "../browser/process.hpp"
#include "ais2api/browser.hpp"
#include "ais2api/camoufox.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/store.hpp"
#ifdef AIS2API_HAS_SPIDERMONKEY
#include "../waa/src/host.hpp"
#endif
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

using namespace ais2api;
using browser_detail::Bidi;

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("ais2api-camoufox-bridge-" + random_hex(8));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

struct Peer {
    struct Channel {
        websocket::stream<tcp::socket> &ws;
        std::string raw;
        json::object read() {
            beast::flat_buffer buffer;
            ws.read(buffer);
            raw = beast::buffers_to_string(buffer.data());
            return json::parse(raw).as_object();
        }
        void send(const json::object &message) {
            const auto text = json::serialize(message);
            ws.write(asio::buffer(text));
        }
    };
    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    unsigned short port = acceptor.local_endpoint().port();
    std::jthread worker;
    template <class Script>
    explicit Peer(Script script)
        : worker([this, script] {
              try {
                  websocket::stream<tcp::socket> ws{io};
                  acceptor.accept(ws.next_layer());
                  ws.accept();
                  Channel channel{ws, {}};
                  script(channel);
              } catch (...) {
              }
          }) {}
};

json::object success(const json::value &id, json::object result) {
    return {{"type", "success"}, {"id", id}, {"result", std::move(result)}};
}

json::object script_result(json::object remote) {
    return {{"type", "success"}, {"result", std::move(remote)}, {"realm", "realm-1"}};
}
} // namespace

TEST_CASE("BiDi captures preload channel handles per browsing context and passes RemoteReference arguments", "[bidi][camoufox]") {
    std::promise<json::object> call_received;
    std::promise<void> send_nav_handle;
    std::promise<void> destroy_tab;

    Peer peer([&](Peer::Channel &channel) {
        // Initial preload handles from top tab and a subframe in the same tab.
        channel.send({{"type", "event"},
                      {"method", "script.message"},
                      {"params",
                       {{"channel", "ais2api-bridge"},
                        {"data", {{"type", "object"}, {"handle", "handle-top-1"}}},
                        {"source", {{"realm", "realm-top-1"}, {"context", "tab-1"}}}}}});
        channel.send({{"type", "event"},
                      {"method", "script.message"},
                      {"params",
                       {{"channel", "ais2api-bridge"},
                        {"data", {{"type", "object"}, {"handle", "handle-subframe-1"}}},
                        {"source", {{"realm", "realm-sub-1"}, {"context", "iframe-1"}}}}}});

        auto call = channel.read();
        call_received.set_value(call);
        channel.send(success(call.at("id"), script_result({{"type", "number"}, {"value", 2}})));

        send_nav_handle.get_future().wait();
        channel.send({{"type", "event"},
                      {"method", "script.message"},
                      {"params",
                       {{"channel", "ais2api-bridge"},
                        {"data", {{"type", "object"}, {"handle", "handle-top-2"}}},
                        {"source", {{"realm", "realm-top-2"}, {"context", "tab-1"}}}}}});

        destroy_tab.get_future().wait();
        channel.send({{"type", "event"}, {"method", "browsingContext.contextDestroyed"}, {"params", {{"context", "tab-1"}}}});
        std::this_thread::sleep_for(200ms);
    });

    Bidi client;
    client.connect(peer.port, "/session", {});

    const auto handle = client.channel_handle("ais2api-bridge", "tab-1", {}, 5s);
    CHECK(handle == "handle-top-1");
    CHECK(client.channel_handle("ais2api-bridge", "iframe-1", {}, 5s) == "handle-subframe-1");

    const auto version = client.call_function(
        "function(bridge, id, count, ready) { return bridge.version; }",
        {json::object{{"handle", handle}}, "upload-1", 7, true}, "tab-1", {});
    CHECK(version == 2);

    const auto recorded = call_received.get_future().get();
    const auto &params = recorded.at("params").as_object();
    CHECK(string_or(params, "method").empty());
    CHECK(string_or(recorded, "method") == "script.callFunction");
    const auto expected_args = json::array{
        json::object{{"handle", "handle-top-1"}},
        json::object{{"type", "string"}, {"value", "upload-1"}},
        json::object{{"type", "number"}, {"value", 7}},
        json::object{{"type", "boolean"}, {"value", true}},
    };
    CHECK(params.at("arguments") == expected_args);

    // Clearing before navigation waits for the next document's handle.
    client.clear_channel("ais2api-bridge", "tab-1");
    std::jthread publisher([&] {
        std::this_thread::sleep_for(50ms);
        send_nav_handle.set_value();
    });
    CHECK(client.channel_handle("ais2api-bridge", "tab-1", {}, 5s) == "handle-top-2");
    publisher.join();

    // Destroying the browsing context drops its channel handle and fails callers.
    client.clear_channel("ais2api-bridge", "tab-1");
    std::jthread destroyer([&] {
        std::this_thread::sleep_for(50ms);
        destroy_tab.set_value();
    });
    try {
        (void)client.channel_handle("ais2api-bridge", "tab-1", {}, 5s);
        FAIL("expected browser_disconnected");
    } catch (const Error &error) {
        CHECK(error.code == "browser_disconnected");
    }
    destroyer.join();
}

#ifdef AIS2API_HAS_SPIDERMONKEY
namespace {
struct HostPage {
    waa::EventLoop loop;
    std::unique_ptr<waa::Agent> agent;
    template <typename Body>
    auto run(Body body) -> decltype(body()) {
        auto task = std::make_shared<std::packaged_task<decltype(body())()>>(std::move(body));
        auto future = task->get_future();
        if (!loop.post([task] { (*task)(); })) throw std::runtime_error("EventLoop closed");
        return future.get();
    }
    HostPage() {
        waa::Profile profile;
        profile.page_url = "https://aistudio.google.com/prompts/new_chat";
        profile.locale = "en-US";
        profile.timezone = "UTC";
        profile.window = {{"innerWidth", 1280}, {"innerHeight", 720}, {"outerWidth", 1296}, {"outerHeight", 808}, {"devicePixelRatio", 1.0}};
        profile.navigator = {{"userAgent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0"},
                             {"hardwareConcurrency", 8}};
        profile.screen = {{"width", 1920}, {"height", 1080}};
        run([this, profile = std::move(profile)]() mutable {
            agent = std::make_unique<waa::Agent>(loop, std::move(profile), nullptr, nullptr, loop.closed());
            loop.cleanup = [this] { agent->close(); };
            loop.after_task = [this] { agent->after_task(); };
            try {
                agent->top = &agent->install_realm("top", 0);
            } catch (const waa::js::Exception &exception) {
                agent->release(exception);
                throw std::runtime_error(exception.what());
            }
        });
    }
    ~HostPage() {
        loop.join();
        agent.reset();
    }
    std::string eval_text(std::string_view script) {
        return run([this, script]() -> std::string {
            try {
                const auto value = agent->evaluate(agent->top->global, script, "https://aistudio.google.com/test.js");
                const auto text = value.kind == waa::js::Kind::string ? value.string : std::string{};
                agent->context().release(value);
                return text;
            } catch (const waa::js::Exception &exception) {
                agent->release(exception);
                throw std::runtime_error(exception.what());
            }
        });
    }
};
} // namespace

TEST_CASE("Bridge preload channel mode adds zero own properties or Symbols to Firefox 158 window while preserving legacy fallback",
          "[camoufox][waa]") {
    HostPage page;

    const auto bridge_source = std::string(browser_detail::embedded_bridge_source());
    const auto harness = "(function() {\n"
                         "  if (typeof globalThis.fetch !== 'function') globalThis.fetch = function() { return Promise.resolve(); };\n"
                         "  const beforeNames = Object.getOwnPropertyNames(globalThis);\n"
                         "  const beforeSymbols = Object.getOwnPropertySymbols(globalThis).length;\n"
                         "  let published = null;\n"
                         "  const preload = function(publish) {\n" +
                         bridge_source +
                         "\n  };\n"
                         "  preload(function(b) { published = b; });\n"
                         "  const afterNames = Object.getOwnPropertyNames(globalThis);\n"
                         "  const afterSymbols = Object.getOwnPropertySymbols(globalThis).length;\n"
                         "  const added = afterNames.filter(k => !beforeNames.includes(k));\n"
                         "  const removed = beforeNames.filter(k => !afterNames.includes(k));\n"
                         "  const okUpload = Boolean(published && published.append('u1', 'part1') === 1 && published.append('u1', 'part2') === 2);\n"
                         "  const cap = published ? published.capability() : null;\n"
                         "  const hasExpFn = Boolean(published && typeof published.experimentContext === 'function' && published.experimentContext() === null);\n"
                         "  if (published) published.dispose();\n"
                         "  return JSON.stringify({\n"
                         "    hasGlobal: ('__ais2bridge' in globalThis),\n"
                         "    typeofGlobal: typeof globalThis.__ais2bridge,\n"
                         "    added: added,\n"
                         "    removed: removed,\n"
                         "    symbolsDelta: afterSymbols - beforeSymbols,\n"
                         "    hasStart: Boolean(published && typeof published.start === 'function'),\n"
                         "    okUpload: okUpload,\n"
                         "    hasExpFn: hasExpFn,\n"
                         "    capHasKey: Boolean(cap && 'apiKey' in cap)\n"
                         "  });\n"
                         "})()";

    const auto report = json::parse(page.eval_text(harness)).as_object();
    INFO(json::serialize(report));
    CHECK_FALSE(bool_or(report, "hasGlobal"));
    CHECK(string_or(report, "typeofGlobal") == "undefined");
    CHECK(report.at("added").as_array().empty());
    CHECK(report.at("removed").as_array().empty());
    CHECK(report.at("symbolsDelta") == 0);
    CHECK(bool_or(report, "hasStart"));
    CHECK(bool_or(report, "okUpload"));
    CHECK(bool_or(report, "hasExpFn"));
    CHECK_FALSE(bool_or(report, "capHasKey"));

    // Direct invocation without a publish callback is a no-op and attaches no global properties.
    const auto noop = "(function() {\n" + bridge_source +
                      "\n  return JSON.stringify({\n"
                      "    hasGlobal: ('__ais2bridge' in globalThis),\n"
                      "    typeofGlobal: typeof globalThis.__ais2bridge\n"
                      "  });\n})()";
    const auto noop_report = json::parse(page.eval_text(noop)).as_object();
    CHECK_FALSE(bool_or(noop_report, "hasGlobal"));
    CHECK(string_or(noop_report, "typeofGlobal") == "undefined");
}
#endif

// Hidden by default: run with AIS2API_TEST_CAMOUFOX pointing to a local
// Camoufox executable. Opens only local data: documents; never connects to
// Google or external hosts.
TEST_CASE("Headless Camoufox keeps window own properties and Symbols identical while bridge handle calls succeed",
          "[.camoufox-runtime]") {
    const auto *executable = std::getenv("AIS2API_TEST_CAMOUFOX");
    REQUIRE(executable != nullptr);
    const fs::path exe_path = fs::u8path(executable);
    REQUIRE(fs::is_regular_file(exe_path));

    TempDir dir;
    Config config;
    config.data_dir = dir.path;
    config.browser_executable = exe_path;
    config.headless = std::getenv("AIS2API_TEST_CAMOUFOX_HEADED") == nullptr;
    Store store(config.data_dir);

    // 1. CamoufoxGateway::verify_runtime() checks both bridge readiness and bridgeHidden.
    {
        CamoufoxGateway gateway(config, store);
        const auto runtime = gateway.verify_runtime();
        INFO(json::serialize(runtime));
        const auto &page = runtime.at("page").as_object();
        CHECK(bool_or(page, "bridge"));
        CHECK(bool_or(page, "bridgeHidden"));
        CHECK(page.at("webdriver") == json::value(false));
        CHECK(bool_or(page, "byobReader"));
        CHECK(bool_or(page, "trustedKey"));
    }

    // 2. Direct baseline vs channel-preload comparison on local data: pages in real Camoufox.
    const auto root = dir.path / "direct-bidi";
    const auto profile = root / "profile";
    fs::create_directories(profile);
    const json::object prefs{
        {"datareporting.policy.dataSubmissionEnabled", false},
        {"toolkit.telemetry.enabled", false},
        {"app.update.enabled", false},
        {"remote.prefs.recommended", false},
    };
    {
        std::ofstream user_js(profile / "user.js", std::ios::binary);
        user_js << browser_detail::firefox_user_js(prefs);
        REQUIRE(static_cast<bool>(user_js));
    }

    std::mutex port_mutex;
    unsigned short bidi_port = 0;
    // The browser's last output, reported when it does not come up.
    std::string launch_output;
    browser_detail::ProcessOptions options;
    options.environment = browser_detail::camoufox_environment(R"({"allowMainWorld":true})");
    options.unset_prefixes = {"CAMOU_CONFIG_", "CAMOU_PREFS_"};
    options.on_output = [&](std::string_view line) {
        std::lock_guard lock(port_mutex);
        if (const auto parsed = browser_detail::bidi_endpoint_port(line)) bidi_port = *parsed;
        launch_output.append(line.substr(0, 512)).push_back('\n');
        if (launch_output.size() > 8192) launch_output.erase(0, launch_output.size() - 8192);
    };

    std::vector<std::string> args{"--profile", path_utf8(profile), "--remote-debugging-port", "0", "--no-remote"};
    if (config.headless) args.push_back("--headless");
    browser_detail::OwnedProcess proc;
    if (runtime_platform() == "termux-arm64") {
        // Termux runs the glibc Camoufox build in setup's proot-distro
        // container, as the gateway does: the temporary directory holding the
        // profile and the browser's directory are bound at their own paths,
        // and proot-distro, which builds the environment afresh, gets it as
        // --env. HOME stays the container's own.
        const char *prefix = std::getenv("PREFIX");
        REQUIRE(prefix != nullptr);
        const auto proot = fs::path(prefix) / "bin" / "proot-distro";
        REQUIRE(fs::is_regular_file(proot));
        REQUIRE_FALSE(termux_container_rootfs().empty());
        auto environment = browser_detail::private_environment(root);
        std::erase_if(environment, [](const auto &item) { return item.first == "HOME"; });
        environment.insert(environment.end(), options.environment.begin(), options.environment.end());
        options.environment.clear();
        proc.start(proot,
                   browser_detail::proot_arguments(exe_path, args, {fs::absolute(dir.path), fs::absolute(exe_path.parent_path())},
                                                   environment),
                   root, options);
    } else {
        proc.start(exe_path, args, root, options);
    }

    // The gateway's launch deadline: proot starts slower.
    const auto deadline = Clock::now() + 45s;
    unsigned short port = 0;
    for (bool alive = true; !port; std::this_thread::sleep_for(50ms)) {
        std::lock_guard lock(port_mutex);
        port = bidi_port;
        if (port) break;
        if (!alive || Clock::now() >= deadline)
            FAIL((alive ? "Camoufox announced no WebDriver BiDi endpoint" : "Camoufox exited before WebDriver BiDi became ready")
                 << ":\n"
                 << launch_output);
        // Once more after it exits: its last lines may still be arriving.
        alive = proc.alive();
    }

    Bidi bidi;
    bidi.connect(port, "/session", {});
    (void)bidi.call("session.new", {{"capabilities", {{"alwaysMatch", {{"webSocketUrl", true}}}}}}, {});
    (void)bidi.call("session.subscribe", {{"events", json::array{"browsingContext.contextDestroyed", "script.message"}}}, {});

    const auto user_ctx = string_or(bidi.call("browser.createUserContext", {}, {}), "userContext");
    REQUIRE_FALSE(user_ctx.empty());

    constexpr std::string_view local_doc =
        "data:text/html,<!doctype html><iframe id=sub srcdoc='<p>sub</p>'></iframe><title>local</title>";
    constexpr std::string_view inspect_window = R"js((() => ({
        names: Object.getOwnPropertyNames(window).sort(),
        symbols: Object.getOwnPropertySymbols(window).length,
        hasBridge: ('__ais2bridge' in window),
        typeofBridge: typeof window.__ais2bridge,
        aisKeys: Object.getOwnPropertyNames(window).filter(k => k.toLowerCase().includes('ais2')),
        subHasBridge: Boolean(document.getElementById('sub') && ('__ais2bridge' in document.getElementById('sub').contentWindow)),
        subSymbols: document.getElementById('sub') ? Object.getOwnPropertySymbols(document.getElementById('sub').contentWindow).length : -1
    }))())js";

    // Baseline tab without bridge preload.
    const auto base_tab = string_or(bidi.call("browsingContext.create", {{"type", "tab"}, {"userContext", user_ctx}}, {}), "context");
    REQUIRE_FALSE(base_tab.empty());
    (void)bidi.call("browsingContext.navigate", {{"context", base_tab}, {"url", std::string(local_doc)}, {"wait", "complete"}}, {}, 30s, base_tab);
    const auto baseline = bidi.evaluate(std::string(inspect_window), base_tab, {}).as_object();

    // Install the exact preload script with a BiDi channel argument.
    (void)bidi.call(
        "script.addPreloadScript",
        {{"functionDeclaration", "function(publish) {\n" + std::string(browser_detail::embedded_bridge_source()) + "\n}"},
         {"arguments",
          json::array{json::object{{"type", "channel"},
                                   {"value",
                                    {{"channel", "ais2api-bridge"},
                                     {"ownership", "root"},
                                     {"serializationOptions", {{"maxObjectDepth", 0}}}}}}}},
         {"userContexts", json::array{json::value(user_ctx)}}},
        {});

    // Bridged tab with preload channel active.
    const auto bridged_tab =
        string_or(bidi.call("browsingContext.create", {{"type", "tab"}, {"userContext", user_ctx}}, {}), "context");
    REQUIRE_FALSE(bridged_tab.empty());
    bidi.clear_channel("ais2api-bridge", bridged_tab);
    (void)bidi.call("browsingContext.navigate", {{"context", bridged_tab}, {"url", std::string(local_doc)}, {"wait", "complete"}}, {},
                    30s, bridged_tab);

    const auto handle = bidi.channel_handle("ais2api-bridge", bridged_tab, {}, 10s);
    REQUIRE_FALSE(handle.empty());

    const auto bridged = bidi.evaluate(std::string(inspect_window), bridged_tab, {}).as_object();
    INFO("baseline=" << json::serialize(baseline));
    INFO("bridged=" << json::serialize(bridged));
    CHECK(bridged.at("names") == baseline.at("names"));
    CHECK(bridged.at("symbols") == 0);
    CHECK(bridged.at("symbols") == baseline.at("symbols"));
    CHECK_FALSE(bool_or(bridged, "hasBridge"));
    CHECK(string_or(bridged, "typeofBridge") == "undefined");
    CHECK(bridged.at("aisKeys").as_array().empty());
    CHECK_FALSE(bool_or(bridged, "subHasBridge"));
    CHECK(bridged.at("subSymbols") == 0);

    const auto bridge_check = bidi.call_function(
                                      "function(bridge) { const up = bridge.append('u1', 'part1') === 1 && bridge.append('u1', 'part2') === 2; "
                                      "bridge.abort(); return { hasStart: typeof bridge.start === 'function', up: up, id: bridge.identity() }; }",
                                      {json::object{{"handle", handle}}}, bridged_tab, {})
                                  .as_object();
    CHECK(bool_or(bridge_check, "hasStart"));
    CHECK(bool_or(bridge_check, "up"));
    CHECK(bridge_check.at("id").is_object());

    bidi.close();
    proc.stop();
}
