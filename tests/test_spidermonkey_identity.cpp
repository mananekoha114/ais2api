// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifdef AIS2API_HAS_SPIDERMONKEY
#include "ais2api/camoufox.hpp"
#include "ais2api/diagnostics.hpp"
#include "ais2api/firefox_identity.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/spidermonkey.hpp"
#include "ais2api/store.hpp"
#include "fpgen_fixture.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

namespace ais2api::experiment_detail {
void stage_page_session(SpiderMonkeyGateway &gateway, json::object session);
}
namespace ais2api::spidermonkey_detail {
std::string run_vm_refresh_snapshot(std::stop_token parent, std::chrono::milliseconds timeout,
                                    std::string_view account_id,
                                    const std::function<std::string(std::stop_token)> &fetch,
                                    const std::function<void()> &reset_vm);
}

using namespace ais2api;
using namespace ais2api::firefox_identity;

namespace {
using fpgen_fixture::code;
using fpgen_fixture::codes;
using fpgen_fixture::Fixture;
using fpgen_fixture::node;
using fpgen_fixture::table;
using fpgen_fixture::ValueId;

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("ais2api-sm-identity-" + random_hex(8));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string webgl_record(std::string_view vendor, std::string_view renderer, bool depth_clamp) {
    json::object record{
        {"vendor", vendor},
        {"renderer", renderer},
        {"supportedExtensions", json::array{"ANGLE_instanced_arrays", "EXT_blend_minmax", "OVR_multiview2",
                                            "WEBGL_debug_renderer_info", "WEBGL_multi_draw"}},
        {"params", {{"3379", {{"value", 16384}}}, {"33902", {{"value", json::array{1, 1}}}}}},
        {"shaderPrecisionFormats",
         json::array{json::object{{"shaderType", 35633},
                                  {"precisionType", 36338},
                                  {"shaderPrecisionFormat", {{"rangeMin", 127}, {"rangeMax", 127}, {"precision", 23}}}}}}};
    if (depth_clamp) record["supportedExtensions"].as_array().emplace_back("EXT_depth_clamp");
    return json::serialize(record);
}

// Synthetic fpgen Bayesian network covering both Windows and Linux hosts so
// browserless SpiderMonkey identity generation runs offline in unit tests.
struct SyntheticModel : Fixture {
    static constexpr std::string_view tag = "model-test/2026";
    static constexpr std::string_view dir_name = "model-test-2026";

    const std::string nvidia = "ANGLE (NVIDIA, NVIDIA GeForce GTX 980 Direct3D11 vs_5_0 ps_5_0), or similar";
    const std::string intel = "ANGLE (Intel, Intel(R) HD Graphics Direct3D11 vs_5_0 ps_5_0), or similar";
    const std::string basic = "ANGLE (Microsoft, Microsoft Basic Render Driver Direct3D11 vs_5_0 ps_5_0), or similar";
    const std::string linux_gpu = "NVIDIA GeForce GTX 980, or similar";

    ValueId firefox = add(R"("Firefox")"), chrome = add(R"("Chrome")"),
            windows = add(R"("Windows")"), linux_os = add(R"("Linux")"),
            windows_ua = add(R"("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:146.0) Gecko/20100101 Firefox/146.0")"),
            linux_ua = add(R"("Mozilla/5.0 (X11; Linux x86_64; rv:146.0) Gecko/20100101 Firefox/146.0")"),
            wide = add(R"({"availLeft":0,"availTop":0,"availWidth":1920,"availHeight":1040,"width":1920,"height":1080,"colorDepth":24,"pixelDepth":24})"),
            linux_screen = add(R"({"availWidth":2560,"availHeight":1440,"width":2560,"height":1440,"colorDepth":24,"pixelDepth":24})"),
            wide_window = add(R"({"outerWidth":1936,"outerHeight":1056,"screenX":-8,"screenY":-8})"),
            linux_window = add(R"({"outerWidth":2000,"outerHeight":1400,"screenX":300})"),
            win32 = add(R"("Win32")"), linux_platform = add(R"("Linux x86_64")"),
            windows_cpu = add(R"("Windows NT 10.0; Win64; x64")"), linux_cpu = add(R"("Linux x86_64")"),
            no_touch = add("0"),
            nvidia_gpu = add(json::serialize(json::object{{"vendor", "Google Inc. (NVIDIA)"}, {"renderer", nvidia}})),
            intel_gpu = add(json::serialize(json::object{{"vendor", "Google Inc. (Intel)"}, {"renderer", intel}})),
            basic_gpu = add(json::serialize(json::object{{"vendor", "Google Inc. (Microsoft)"}, {"renderer", basic}})),
            linux_gpu_val = add(json::serialize(json::object{{"vendor", "NVIDIA Corporation"}, {"renderer", linux_gpu}})),
            nvidia_webgl = add(webgl_record("Google Inc. (NVIDIA)", nvidia, false)),
            intel_webgl = add(webgl_record("Google Inc. (Intel)", intel, false)),
            basic_webgl = add(webgl_record("Google Inc. (Microsoft)", basic, false)),
            linux_webgl = add(webgl_record("NVIDIA Corporation", linux_gpu, true)),
            webgl2 = add(webgl_record("", "", false)), no_webgl2 = add("[]");

    fpgen::Model model = load({
        node("browser", {}, codes({firefox, chrome}), table({{firefox, .8}, {chrome, .2}})),
        node("os", {"browser"}, codes({windows, linux_os}),
             {{code(firefox), table({{windows, .6}, {linux_os, .4}})}, {code(chrome), table({{windows, 1}})}}),
        node("navigator.userAgent", {"os"}, codes({windows_ua, linux_ua}),
             {{code(windows), table({{windows_ua, 1}})}, {code(linux_os), table({{linux_ua, 1}})}}),
        node("screen", {"os"}, codes({wide, linux_screen}),
             {{code(windows), table({{wide, 1}})}, {code(linux_os), table({{linux_screen, 1}})}}),
        node("window", {"screen"}, codes({wide_window, linux_window}),
             {{code(wide), table({{wide_window, 1}})}, {code(linux_screen), table({{linux_window, 1}})}}),
        node("navigator.platform", {"navigator.userAgent"}, codes({win32, linux_platform}),
             {{code(windows_ua), table({{win32, 1}})}, {code(linux_ua), table({{linux_platform, 1}})}}),
        node("navigator.oscpu", {"navigator.userAgent"}, codes({windows_cpu, linux_cpu}),
             {{code(windows_ua), table({{windows_cpu, 1}})}, {code(linux_ua), table({{linux_cpu, 1}})}}),
        node("navigator.maxTouchPoints", {"navigator.userAgent"}, codes({no_touch}),
             {{code(windows_ua), table({{no_touch, 1}})}, {code(linux_ua), table({{no_touch, 1}})}}),
        node("gpu", {"os"}, codes({nvidia_gpu, intel_gpu, basic_gpu, linux_gpu_val}),
             {{code(windows), table({{nvidia_gpu, .5}, {intel_gpu, .4}, {basic_gpu, .1}})},
              {code(linux_os), table({{linux_gpu_val, 1}})}}),
        node("webgl", {"gpu"}, codes({nvidia_webgl, intel_webgl, basic_webgl, linux_webgl}),
             {{code(nvidia_gpu), table({{nvidia_webgl, 1}})},
              {code(intel_gpu), table({{intel_webgl, 1}})},
              {code(basic_gpu), table({{basic_webgl, 1}})},
              {code(linux_gpu_val), table({{linux_webgl, 1}})}}),
        node("webgl2", {"gpu", "webgl"}, codes({webgl2, no_webgl2}),
             {{code(nvidia_gpu), {{code(nvidia_webgl), table({{webgl2, 1}})}}},
              {code(intel_gpu), {{code(intel_webgl), table({{webgl2, 1}})}}},
              {code(basic_gpu), {{code(basic_webgl), table({{webgl2, 1}})}}},
              {code(linux_gpu_val), {{code(linux_webgl), table({{no_webgl2, 1}})}}}}),
    });

    void install_into(const fs::path &data_dir) const {
        const auto destination = data_dir / "runtime" / "fpgen" / std::string(dir_name);
        fs::create_directories(destination);
        for (const auto &[name, digest] : sha256s) {
            (void)digest;
            fs::copy_file(directory.path / name, destination / name, fs::copy_options::overwrite_existing);
        }
    }
};

// Scoped runtime-manifest.json beside the test executable so fpgen_model_pin()
// resolves the synthetic model without network or external files.
struct ScopedManifest {
    fs::path path = executable_path().parent_path() / "runtime-manifest.json";
    std::optional<std::string> previous;

    explicit ScopedManifest(const SyntheticModel &synthetic) {
        if (fs::is_regular_file(path)) previous = read_file(path, 65536);
        auto doc = json::parse(read_file(fs::path(AIS2API_SOURCE_DIR) / "runtime-manifest.json", 65536)).as_object();
        json::object files;
        for (const auto &[name, digest] : synthetic.sha256s) files[name] = digest;
        doc["fpgen_model"] = json::object{
            {"tag", std::string(SyntheticModel::tag)},
            {"url", "https://example.invalid/model-release.zip"},
            {"size", 1024},
            {"sha256", std::string(64, 'a')},
            {"file_sha256", std::move(files)},
        };
        atomic_write(path, json::serialize(doc));
    }

    ~ScopedManifest() {
        std::error_code error;
        if (previous) atomic_write(path, *previous);
        else fs::remove(path, error);
    }
};

// Binds and immediately closes a loopback socket so HTTP requests routed
// through this proxy port fail locally in milliseconds without leaving
// 127.0.0.1.
unsigned short closed_loopback_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), 0});
    return acceptor.local_endpoint().port();
}

Account sample_account(std::string id = "acc_sm_test") {
    Account account;
    account.id = std::move(id);
    account.email = "tester@example.com";
    account.auth_user = "0";
    account.storage_state = {
        {"cookies",
         json::array{json::object{{"name", "SID"}, {"value", "sid-v"}, {"domain", ".google.com"}, {"path", "/"}},
                     json::object{{"name", "SAPISID"}, {"value", "sapisid-v"}, {"domain", ".google.com"}, {"path", "/"}}}}};
    return account;
}

template <typename Call>
std::pair<std::string, std::string> catch_error(Call &&call) {
    try {
        call();
    } catch (const Error &error) {
        return {error.code, error.what()};
    }
    return {"none", ""};
}
} // namespace

TEST_CASE("SpiderMonkey refuses missing fpgen models directly without asking for Camoufox", "[spidermonkey][firefox-identity]") {
    TempDir dir;
    Config config;
    config.data_dir = dir.path;
    config.proxy = "http://127.0.0.1:" + std::to_string(closed_loopback_port());
    Store store(config.data_dir);
    SpiderMonkeyGateway gateway(config, store);
    const auto account = sample_account("acc_missing_model");

#ifndef AIS2API_DEV_BUNDLE
    const auto [code_name, message] = catch_error([&] { gateway.probe(account); });
    CHECK(code_name == "browser_not_configured");
    CHECK(message.starts_with("fpgen model is not installed"));
    CHECK(message.find("camoufox backend first") == std::string::npos);
#endif
    const auto diag = gateway.diagnostics();
    CHECK(string_or(diag, "backend") == "spidermonkey");
    CHECK(string_or(diag, "claimedFirefox") == "158");
    CHECK_FALSE(bool_or(diag, "running"));
}

TEST_CASE("SpiderMonkey generates, persists, reuses and regenerates Firefox 158 identities without a browser",
          "[spidermonkey][firefox-identity]") {
    SyntheticModel synthetic;
    ScopedManifest manifest_override(synthetic);
    TempDir dir;
    synthetic.install_into(dir.path);

    Config config;
    config.data_dir = dir.path;
    config.proxy = "http://127.0.0.1:" + std::to_string(closed_loopback_port());
    Store store(config.data_dir);

    auto account = sample_account("acc_sm_stored");
    store.put_account(account);
    store.set_fingerprint_region(account.id, "Asia/Tokyo", "ja-JP");
    REQUIRE_FALSE(store.firefox_identity(account.id).has_value());

    SpiderMonkeyGateway gateway(config, store);
    const auto [first_code, first_message] = catch_error([&] { gateway.probe(account); });
    INFO(first_code << ": " << first_message);
    CHECK(first_code != "browser_not_configured");
    CHECK(first_code != "missing_cookies");

    const auto saved = store.firefox_identity(account.id);
    REQUIRE(saved.has_value());
    CHECK(saved->at("seed") == store.fingerprint(account.id).seed);
    const auto parsed = firefox_identity::from_json(*saved);
    CHECK(parsed.browser == "158.0");
    CHECK(parsed.model == SyntheticModel::tag);
    CHECK_FALSE(parsed.host_gpu.has_value());
    const auto ua = string_or(parsed.config, "navigator.userAgent");
    CHECK(ua.find("rv:158.0") != std::string::npos);
    CHECK(ua.find("Firefox/158.0") != std::string::npos);

    const auto diag = gateway.diagnostics();
    REQUIRE(diag.at("identity").is_object());
    const auto &id_diag = diag.at("identity").as_object();
    CHECK(string_or(id_diag, "generatedFor") == "158.0");
    CHECK(string_or(id_diag, "model") == SyntheticModel::tag);
    CHECK(string_or(id_diag, "hostGpu") == "not_checked");
    CHECK(string_or(id_diag, "catalog").size() == 12);
    CHECK(id_diag.contains("claimsHostGpu"));
    CHECK(id_diag.contains("canvasCovered"));
    CHECK(string_or(id_diag, "userAgent") == ua);

    // Safe diagnostics keep allowlisted identity metadata while stripping the raw userAgent.
    DiagnosticsSources sources;
    sources.browser = diag;
    const auto report = safe_report(sources);
    const auto serialized_report = json::serialize(report);
    CHECK(serialized_report.find("Firefox/158.0") == std::string::npos);
    const auto &safe_id = report.at("browser").at("identity").as_object();
    CHECK(string_or(safe_id, "generatedFor") == "158.0");
    CHECK(string_or(safe_id, "model") == SyntheticModel::tag);
    CHECK(string_or(safe_id, "hostGpu") == "not_checked");
    CHECK(string_or(safe_id, "catalog") == string_or(id_diag, "catalog"));
    CHECK_FALSE(safe_id.contains("userAgent"));

    // Subsequent calls reuse the stored identity without regenerating it.
    (void)catch_error([&] { gateway.probe(account); });
    CHECK(store.firefox_identity(account.id) == saved);

    // Regenerating the account's fingerprint draws and persists a fresh Firefox identity.
    const auto next_fp = store.regenerate_fingerprint(account.id);
    CHECK(next_fp.seed != saved->at("seed").to_number<std::uint64_t>());
    (void)catch_error([&] { gateway.probe(account); });
    const auto regenerated = store.firefox_identity(account.id);
    REQUIRE(regenerated.has_value());
    CHECK(regenerated->at("seed") == next_fp.seed);
    CHECK(firefox_identity::from_json(*regenerated).salt != parsed.salt);
}

TEST_CASE("SpiderMonkey derives deterministic identities for unstored import verification accounts",
          "[spidermonkey][firefox-identity]") {
    SyntheticModel synthetic;
    ScopedManifest manifest_override(synthetic);
    TempDir dir;
    synthetic.install_into(dir.path);

    Config config;
    config.data_dir = dir.path;
    config.proxy = "http://127.0.0.1:" + std::to_string(closed_loopback_port());
    Store store(config.data_dir);

    const auto unstored = sample_account("acc_unstored_import");
    SpiderMonkeyGateway gateway(config, store);
    const auto [code_name, message] = catch_error([&] { gateway.probe(unstored); });
    INFO(code_name << ": " << message);
    CHECK(code_name == "proxy_failed");
    CHECK(catch_error([&] { (void)store.firefox_identity(unstored.id); }).first == "account_not_found");

    const auto unstored_diag = gateway.diagnostics();
    REQUIRE(unstored_diag.at("identity").is_object());
    const auto unstored_ua = string_or(unstored_diag.at("identity").as_object(), "userAgent");
    CHECK(unstored_ua.find("Firefox/158.0") != std::string::npos);

    // Once stored, the account receives default_fingerprint_seed(account.id) and draws the identical identity.
    store.put_account(unstored);
    SpiderMonkeyGateway stored_gateway(config, store);
    (void)catch_error([&] { stored_gateway.probe(unstored); });
    const auto persisted = store.firefox_identity(unstored.id);
    REQUIRE(persisted.has_value());
    const auto expected_salt =
        browser_detail::identity_salt(store.identity_secret(), unstored.id, default_fingerprint_seed(unstored.id));
    CHECK(firefox_identity::from_json(*persisted).salt == expected_salt);
    const auto stored_ua = string_or(stored_gateway.diagnostics().at("identity").as_object(), "userAgent");
    CHECK(stored_ua == unstored_ua);
}

TEST_CASE("SpiderMonkey adopts a staged Camoufox page session for offline hybrid Cell 2 alignment", "[spidermonkey][waa]") {
    TempDir dir;
    Config config;
    config.data_dir = dir.path;
    config.proxy = "http://127.0.0.1:" + std::to_string(closed_loopback_port());
    Store store(config.data_dir);
    SpiderMonkeyGateway gateway(config, store);

    const auto account = sample_account("acc_hybrid_cell2");
    const std::string expected_ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0";
    json::object captured{
        {"pageId", "tab-hybrid-1"},
        {"userAgent", expected_ua},
        {"acceptLanguage", "ja,en-US;q=0.5"},
        {"platform", "windows"},
        {"cookies", account.storage_state.at("cookies").as_array()},
        {"context",
         json::object{
             {"url", "https://aistudio.google.com/prompts/new_chat"},
             {"apiKey", "test-page-api-key"},
             {"visitId", "v1_hybrid_visit"},
             {"extension", "[150,\"ext\"]"},
             {"tier", "free"},
             {"timezone", "Asia/Tokyo"},
             {"body", json::parse(R"(["models/gemini-3.1-flash-lite-preview",[],null,{}])").as_array()},
         }},
    };

    experiment_detail::stage_page_session(gateway, std::move(captured));

    Request request;
    request.model = "gemini-3.1-flash-lite-preview";
    request.gemini = {{"contents", json::array{json::object{{"role", "user"}, {"parts", json::array{json::object{{"text", "ping"}}}}}}}};

    // Fails at the local closed proxy during GenerateContent without fetching the HTML page or needing fpgen.
    const auto [code_name, message] = catch_error([&] {
        gateway.experiment_send(request, account, "!proof-token", [](GenerationEvent) {}, {});
    });
    INFO(code_name << ": " << message);
    CHECK(code_name != "browser_not_configured");
    CHECK(code_name != "waa_unavailable");

    const auto diag = gateway.diagnostics();
    CHECK(bool_or(diag, "pageLoaded"));
    REQUIRE(diag.at("identity").is_object());
    CHECK(string_or(diag.at("identity").as_object(), "userAgent") == expected_ua);
    CHECK(string_or(diag.at("identity").as_object(), "platform") == "windows");

    gateway.reset_session();
    CHECK_FALSE(bool_or(gateway.diagnostics(), "pageLoaded"));
}

TEST_CASE("SpiderMonkey refresh_snapshot timeout degrades to E:UCE while external cancellation propagates",
          "[spidermonkey][waa]") {
    using namespace std::chrono_literals;

    SECTION("internal deadline timeout resets the old VM and returns E:UCE") {
        bool vm_reset = false;
        const auto snapshot = spidermonkey_detail::run_vm_refresh_snapshot(
            {}, 50ms, "acc_refresh_timeout",
            [](std::stop_token call_token) -> std::string {
                std::mutex wait_mutex;
                std::condition_variable_any wake;
                std::unique_lock lock(wait_mutex);
                wake.wait(lock, call_token, [] { return false; });
                throw Error(499, "cancelled", "WAA refresh interrupted");
            },
            [&] { vm_reset = true; });
        CHECK(vm_reset);
        CHECK(snapshot == "E:UCE");
    }

    SECTION("external caller cancellation resets the VM and propagates 499 cancelled") {
        bool vm_reset = false;
        std::stop_source stop;
        std::jthread canceller([&] {
            std::this_thread::sleep_for(40ms);
            stop.request_stop();
        });
        int status = 0;
        std::string code;
        try {
            (void)spidermonkey_detail::run_vm_refresh_snapshot(
                stop.get_token(), 5s, "acc_refresh_cancel",
                [](std::stop_token call_token) -> std::string {
                    std::mutex wait_mutex;
                    std::condition_variable_any wake;
                    std::unique_lock lock(wait_mutex);
                    wake.wait(lock, call_token, [] { return false; });
                    throw Error(499, "cancelled", "WAA refresh interrupted");
                },
                [&] { vm_reset = true; });
        } catch (const Error &error) {
            status = error.status;
            code = error.code;
        }
        CHECK(vm_reset);
        CHECK(status == 499);
        CHECK(code == "cancelled");
    }

    SECTION("non-cancellation errors fall back to E:UCE and normal snapshots succeed") {
        bool vm_reset = false;
        const auto fallback = spidermonkey_detail::run_vm_refresh_snapshot(
            {}, 5s, "acc_refresh_err",
            [](std::stop_token) -> std::string { throw Error(502, "waa_error", "snapshot failed"); },
            [&] { vm_reset = true; });
        CHECK_FALSE(vm_reset);
        CHECK(fallback == "E:UCE");

        const auto ok = spidermonkey_detail::run_vm_refresh_snapshot(
            {}, 5s, "acc_refresh_ok",
            [](std::stop_token) -> std::string { return "!fresh-snapshot"; },
            [&] { vm_reset = true; });
        CHECK_FALSE(vm_reset);
        CHECK(ok == "!fresh-snapshot");
    }

    SECTION("reconfigure accepts hot-reloaded response_header_seconds") {
        TempDir dir;
        Config config;
        config.data_dir = dir.path;
        Store store(config.data_dir);
        SpiderMonkeyGateway gateway(config, store);
        Config updated = config;
        updated.limits.response_header_seconds = 180;
        CHECK_NOTHROW(gateway.reconfigure(updated));
    }
}

// Hidden by default: run with AIS2API_TEST_FPGEN_MODEL pointing at the pinned
// fpgen model directory. Never makes external network requests.
TEST_CASE("SpiderMonkey generates a real pinned fpgen Firefox 158 identity offline", "[.fpgen-model]") {
    const auto *model_dir = std::getenv("AIS2API_TEST_FPGEN_MODEL");
    REQUIRE(model_dir != nullptr);
    TempDir dir;
    Config config;
    config.data_dir = dir.path;
    config.proxy = "http://127.0.0.1:" + std::to_string(closed_loopback_port());
    const auto pin = fpgen_model_pin();
    const auto installed = fpgen_model_directory(config.data_dir, pin);
    fs::create_directories(installed);
    for (const auto &[name, digest] : pin.file_sha256) {
        (void)digest;
        fs::copy_file(fs::u8path(model_dir) / name, installed / name);
    }

    Store store(config.data_dir);
    auto account = sample_account("acc_real_fpgen");
    store.put_account(account);
    store.set_fingerprint_region(account.id, "Asia/Tokyo", "ja-JP");

    SpiderMonkeyGateway gateway(config, store);
    (void)catch_error([&] { gateway.probe(account); });

    const auto saved = store.firefox_identity(account.id);
    REQUIRE(saved.has_value());
    const auto parsed = firefox_identity::from_json(*saved);
    CHECK(parsed.browser == "158.0");
    CHECK(parsed.model == pin.tag);
    CHECK_FALSE(parsed.host_gpu.has_value());
    const auto ua = string_or(parsed.config, "navigator.userAgent");
    CHECK(ua.find("Firefox/158.0") != std::string::npos);
}
#endif
