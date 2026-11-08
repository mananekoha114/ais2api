// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/diagnostics.hpp"
#include "ais2api/store.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-diagnostics-" + random_hex(8));
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
// Every source carries values the report must never contain.
DiagnosticsSources planted() {
    DiagnosticsSources sources;
    sources.platform = "windows-x64";
    sources.status = {{"running", true}, {"version", "0.1.0"}, {"build_mode", "release"}, {"uptime_seconds", 42},
                      {"active", false}, {"queued", 0}, {"ready", true},
                      {"active_generation_account_id", "acc_secretid0001"}};
    sources.browser = {{"backend", "camoufox"}, {"status", "account_ready"}, {"phase", "idle"}, {"running", true},
                       {"templateCaptured", true}, {"cookieRefresh", "saved"},
                       {"bridgeSource", std::string(200, 'p') + "C:/Users/PlantedUser/bridge.js"},
                       {"proxy", {{"mode", "relay"}, {"relayError", ""}, {"url", "http://planted-proxy-host:8080"}}},
                       {"fingerprint", {{"seed", 73519}, {"timezone", "Planted/Zone"}, {"locale", "xx-PLANTED"}}},
                       {"browser", {{"name", "camoufox"}, {"version", "158.0.1-beta.41"}, {"source", "setup"},
                                    {"executable", "C:/Users/PlantedUser/camoufox.exe"}}},
                       {"identity", {{"platform", "windows"}, {"userAgent", "Mozilla/5.0 PlantedAgent"},
                                     {"gpu", {{"vendor", "PlantedVendor"}, {"renderer", "PlantedRenderer 9000"}}},
                                     {"claimsHostGpu", true}, {"canvasCovered", false}, {"hostGpu", "unchanged"},
                                     {"droppedProperties", json::array{"a", "b", "c"}}}}};
    sources.runtime = {{"build_mode", "release"}, {"browser_backend", "camoufox"}, {"browser_exists", true},
                       {"config_file", "C:/Users/PlantedUser/config.json"},
                       {"data_directory", "C:/Users/PlantedUser/data"},
                       {"fpgen_model", {{"tag", "model-2/2026"}, {"source", "setup"}, {"state", "installed"},
                                        {"directory", "C:/Users/PlantedUser/data/runtime/fpgen"}}},
                       {"camoufox", {{"source", "setup"}, {"current", "158.0.1-beta.41-windows-x64"},
                                     {"executable", "C:/Users/PlantedUser/camoufox.exe"}, {"setup_needed", false},
                                     {"bundle", {{"directory", "C:/Users/PlantedUser/bin"}, {"state", "installed"}}}}}};
    sources.config.proxy = "socks5://planted-user:planted-pass@planted-proxy-host:1080";
    sources.config.api_key = "sk-planted-api-key";
    sources.config.admin_key = "planted-admin-key";
    sources.config.data_dir = "C:/Users/PlantedUser/data";
    sources.config.browser_executable = "C:/Users/PlantedUser/chrome.exe";
    sources.config.model_aliases = {{"planted-alias", "gemini-x"}};
    sources.accounts = {
        json::object{{"id", "acc_secretid0001"}, {"name", "Planted Name"}, {"email", "planted@example.com"},
                     {"disabled", false}, {"expired", false}, {"firefox_identity", true}, {"backend", "auto"},
                     {"fingerprint", {{"seed", 73519}}},
                     {"failures", json::array{json::object{{"model", "*"}, {"until", 1}, {"count", 2}, {"kind", "auth"}},
                                              json::object{{"model", "planted prompt text"}, {"until", 1}, {"kind", "permission_temporary"}}}}},
        json::object{{"id", "acc_secretid0002"}, {"email", "second@example.com"}, {"disabled", true}, {"expired", true}}};
    sources.events = {{"since", 1000}, {"backends", {{"camoufox", {{"permission_denied", {{"total", 3}, {"last_7_days", 2}, {"today", 1}, {"last_at", 5}}}}}}}};
    sources.journal = {
        json::object{{"event", "account_failure"}, {"timestamp", 10}, {"account_id", "acc_secretid0002"},
                     {"request_id", "req-planted"}, {"kind", "auth"}, {"code", "upstream_auth"}},
        json::object{{"event", "session_recovery"}, {"timestamp", 11}, {"account_id", "acc_gone00000001"},
                     {"code", "permission_denied"}, {"action", "rebuild_session"}},
        json::object{{"event", "generation"}, {"timestamp", 12}, {"model", "gemini-3.1-flash-lite"}, {"error", ""}},
        json::object{{"event", "generation"}, {"timestamp", 13}, {"model", "gemini-3.1-flash-lite"},
                     {"error", "generation_failed"}, {"attempts", 3}}};
    sources.logs = {{"sources", json::array{json::object{{"id", "service"}, {"bytes", 12}, {"path", "C:/Users/PlantedUser/service.log"}}}}};
    return sources;
}
}

TEST_CASE("the diagnostics report holds allowlisted fields only", "[diagnostics]") {
    const auto report = safe_report(planted());
    const auto text = json::serialize(report);
    for (auto secret : {"Planted", "planted", "acc_secretid", "example.com", "73519", "req-planted",
                        "sk-", "admin-key", "Users", "PlantedAgent", "Renderer", "Zone", "alias"}) {
        INFO(secret);
        CHECK(text.find(secret) == std::string::npos);
    }
    CHECK(report.at("platform") == "windows-x64");
    CHECK(report.at("service").at("running") == true);
    CHECK(report.at("config").at("proxy") == "socks5");
    CHECK(report.at("runtime").at("camoufox").at("bundle_state") == "installed");
    CHECK(report.at("runtime").at("fpgen_model").at("state") == "installed");
    CHECK(report.at("browser").at("identity").at("droppedProperties") == 3);
    CHECK(report.at("browser").at("browser").at("version") == "158.0.1-beta.41");
    CHECK(report.at("events").at("backends").at("camoufox").at("permission_denied").at("total") == 3);
    const auto &accounts = report.at("accounts").as_array();
    REQUIRE(accounts.size() == 2);
    CHECK(accounts[0].at("label") == "account-1");
    CHECK(accounts[0].at("backend") == "auto");
    CHECK(accounts[0].at("cooldowns").as_array()[0].at("scope") == "account");
    CHECK(accounts[0].at("cooldowns").as_array()[1].at("model") == "other");
    CHECK(accounts[1].at("expired") == true);
    const auto &recent = report.at("recent").as_array();
    REQUIRE(recent.size() == 3);
    CHECK(recent[0].at("account") == "account-2");
    CHECK(recent[1].at("account") == "removed");
    CHECK(recent[2].at("error") == "generation_failed");
    CHECK(report.at("logs").as_array()[0].as_object().size() == 2);
}

TEST_CASE("a report without a running service says so", "[diagnostics]") {
    DiagnosticsSources sources;
    const auto report = safe_report(sources);
    CHECK(report.at("service").at("running") == false);
    CHECK(report.at("browser").at("status") == "not_running");
    CHECK(report.at("config").at("proxy") == "none");
    CHECK(report.at("platform") == "unsupported");
}

TEST_CASE("recent failure records keep the newest failures in order", "[diagnostics]") {
    Directory dir;
    logs::Journal journal(dir.path);
    for (int i = 0; i < 700; ++i) {
        journal.append({{"event", "generation_attempt"}, {"sequence", i}});
        journal.append({{"event", i % 2 ? "account_failure" : "generation"}, {"sequence", i},
                        {"error", i % 4 == 0 ? "" : "upstream_auth"}});
    }
    const auto records = recent_failure_records(journal, 50);
    REQUIRE(records.size() == 50);
    CHECK(records.back().at("sequence") == 699);
    for (const auto &record : records) CHECK(string_or(record.as_object(), "event") != "generation_attempt");
    CHECK(recent_failure_records(journal, 50).size() == 50);
}

TEST_CASE("backend events count per day and reset with the statistics", "[diagnostics]") {
    Directory dir;
    {
        Store store(dir.path);
        store.record_backend_event("camoufox", "permission_denied");
        store.record_backend_event("camoufox", "permission_denied");
        store.record_backend_event("cloakbrowser", "login_redirect");
    }
    Store store(dir.path);
    auto events = store.backend_events();
    const auto &camoufox = events.at("backends").at("camoufox").at("permission_denied").as_object();
    CHECK(camoufox.at("total") == 2);
    CHECK(camoufox.at("today") == 2);
    CHECK(camoufox.at("last_7_days") == 2);
    CHECK(events.at("backends").at("cloakbrowser").at("login_redirect").at("total") == 1);
    store.reset_stats();
    CHECK(store.backend_events().at("backends").as_object().empty());
}
