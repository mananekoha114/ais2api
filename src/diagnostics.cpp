// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/diagnostics.hpp"
#include "ais2api/backend_router.hpp"
#include <algorithm>
#include <cctype>
#include <deque>
#include <map>

namespace ais2api {
namespace {
// Codes, states, counts and flags. A longer string is no code and may be
// anything, so it is left out.
bool safe_scalar(const json::value &value) {
    if (value.is_bool() || value.is_int64() || value.is_uint64() || value.is_double()) return true;
    return value.is_string() && value.as_string().size() <= 96;
}
void copy(json::object &to, const json::object &from, std::initializer_list<std::string_view> keys) {
    for (auto key : keys)
        if (auto value = from.if_contains(key); value && safe_scalar(*value)) to[key] = *value;
}
const json::object *object_at(const json::object &from, std::string_view key) {
    auto value = from.if_contains(key);
    return value && value->is_object() ? &value->as_object() : nullptr;
}
// Gemini model names; anything else a client sent is reported as "other".
std::string model_name(std::string_view model) {
    const bool plain = !model.empty() && model.size() <= 64 && std::all_of(model.begin(), model.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '/';
    });
    return plain ? std::string(model) : "other";
}
std::string proxy_scheme(std::string_view proxy) {
    if (proxy.empty()) return "none";
    const auto end = proxy.find("://");
    if (end == std::string_view::npos) return "other";
    std::string scheme(proxy.substr(0, end));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto known : {"http", "https", "socks4", "socks4a", "socks5", "socks5h"})
        if (scheme == known) return scheme;
    return "other";
}
bool failure_record(const json::object &record) {
    const auto event = string_or(record, "event");
    if (event == "generation") return !string_or(record, "error").empty();
    for (auto name : {"account_failure", "session_recovery", "template_refresh", "browser_failure",
                      "cookie_import_failed", "maintenance_failed"})
        if (event == name) return true;
    return false;
}
json::object service_part(const json::object &status) {
    json::object out{{"running", !status.empty()}};
    copy(out, status, {"uptime_seconds", "active", "queued", "connections", "pending_logs", "logging_healthy",
                       "usage_healthy", "ready"});
    return out;
}
json::object runtime_part(const json::object &doctor) {
    json::object out;
    copy(out, doctor, {"config_version", "browser_backend", "browser_configured", "browser_exists", "manifest_available"});
    if (auto needed = doctor.if_contains("backends_needed"); needed && needed->is_array()) {
        json::array backends;
        for (const auto &backend : needed->as_array())
            if (backend == "cloakbrowser" || backend == "camoufox") backends.push_back(backend);
        out["backends_needed"] = std::move(backends);
    }
    if (auto deprecations = doctor.if_contains("deprecations"); deprecations && deprecations->is_array()) {
        json::array known;
        for (const auto &item : deprecations->as_array()) {
            if (!item.is_object() || string_or(item.as_object(), "code") != "cloakbrowser_backend") continue;
            json::object entry{{"code", "cloakbrowser_backend"}};
            copy(entry, item.as_object(), {"default_backend", "accounts"});
            known.push_back(std::move(entry));
        }
        out["deprecations"] = std::move(known);
    }
    if (auto model = object_at(doctor, "fpgen_model")) {
        json::object fpgen;
        copy(fpgen, *model, {"tag", "source", "state"});
        out["fpgen_model"] = std::move(fpgen);
    }
    if (auto source = object_at(doctor, "camoufox")) {
        json::object camoufox;
        copy(camoufox, *source, {"source", "current", "previous", "held", "pinned", "state", "setup_needed"});
        if (auto bundle = object_at(*source, "bundle"))
            if (auto state = bundle->if_contains("state"); state && safe_scalar(*state)) camoufox["bundle_state"] = *state;
        out["camoufox"] = std::move(camoufox);
    }
    return out;
}
json::object browser_part(const json::object &diagnostics) {
    if (diagnostics.empty()) return {{"status", "not_running"}};
    json::object out;
    copy(out, diagnostics, {"backend", "default_backend", "status", "phase", "running", "isolatedContext", "templateCaptured",
                            "modelDiscovery", "chunkBytes", "cookieRefresh", "bridgeSource", "bridgeSha256"});
    // The SpiderMonkey backend: its page and program, the host files' and
    // the interpreter's digests (Google's public file), and its network marker.
    copy(out, diagnostics, {"pageLoaded", "vmsStarted", "claimedFirefox", "network", "networkStack", "benefitTier"});
    if (auto vm = object_at(diagnostics, "vm")) {
        json::object part;
        copy(part, *vm, {"ageSeconds", "interpreter"});
        out["vm"] = std::move(part);
    }
    if (auto host = object_at(diagnostics, "host")) {
        json::object part;
        copy(part, *host, {"dom", "host", "shape", "spidermonkey"});
        out["host"] = std::move(part);
    }
    if (auto proxy = object_at(diagnostics, "proxy")) {
        json::object part;
        copy(part, *proxy, {"mode", "relayError"});
        out["proxy"] = std::move(part);
    }
    if (auto browser = object_at(diagnostics, "browser")) {
        json::object part;
        copy(part, *browser, {"name", "version", "source", "pinned"});
        out["browser"] = std::move(part);
    }
    // The user agent and GPU strings describe this computer; only how the
    // identity relates to it is reported.
    if (auto identity = object_at(diagnostics, "identity")) {
        json::object part;
        copy(part, *identity, {"platform", "claimsHostGpu", "canvasCovered", "hostGpu", "model", "catalog", "generatedFor"});
        if (auto dropped = identity->if_contains("droppedProperties"); dropped && dropped->is_array())
            part["droppedProperties"] = dropped->as_array().size();
        out["identity"] = std::move(part);
    }
    return out;
}
json::object config_part(const Config &config) {
    const auto source = config.to_json();
    json::object out;
    copy(out, source, {"browser_backend", "headless", "browser_proxy_relay", "browser_site_isolation",
                       "browser_idle_seconds", "continuation_retry_enabled", "ignore_unsupported_parameters",
                       "search_model_drop_functions", "request_retention_days", "cookie_import_max_accounts",
                       "cookie_import_timeout_seconds"});
    out["proxy"] = proxy_scheme(config.proxy);
    if (auto limits = object_at(source, "limits")) {
        json::object part;
        for (const auto &entry : *limits)
            if (entry.value().is_number()) part[entry.key()] = entry.value();
        out["limits"] = std::move(part);
    }
    return out;
}
json::object events_part(const json::object &events) {
    json::object out;
    copy(out, events, {"since"});
    json::object backends;
    if (auto source = object_at(events, "backends"))
        for (const auto &backend : *source) {
            if (backend.key().size() > 32 || !backend.value().is_object()) continue;
            json::object counted;
            for (const auto &event : backend.value().as_object()) {
                if (event.key().size() > 32 || !event.value().is_object()) continue;
                json::object counts;
                copy(counts, event.value().as_object(), {"total", "last_7_days", "today", "last_at"});
                counted[event.key()] = std::move(counts);
            }
            backends[backend.key()] = std::move(counted);
        }
    out["backends"] = std::move(backends);
    return out;
}
} // namespace

json::object safe_report(const DiagnosticsSources &sources) {
    std::map<std::string, std::string, std::less<>> labels;
    json::array accounts;
    for (const auto &value : sources.accounts) {
        if (!value.is_object()) continue;
        const auto &account = value.as_object();
        auto label = "account-" + std::to_string(accounts.size() + 1);
        labels.emplace(string_or(account, "id"), label);
        json::object out{{"label", label}};
        copy(out, account, {"disabled", "expired", "firefox_identity"});
        if (auto backend = account.if_contains("backend"); backend && backend->is_string() && valid_account_backend(backend->as_string()))
            out["backend"] = *backend;
        json::array cooldowns;
        if (auto failures = account.if_contains("failures"); failures && failures->is_array())
            for (const auto &item : failures->as_array()) {
                if (!item.is_object()) continue;
                const auto &failure = item.as_object();
                const auto model = string_or(failure, "model");
                json::object cooldown{{"scope", model == "*" ? "account" : "model"}};
                if (model != "*") cooldown["model"] = model_name(model);
                copy(cooldown, failure, {"kind", "until", "count"});
                cooldowns.push_back(std::move(cooldown));
            }
        out["cooldowns"] = std::move(cooldowns);
        accounts.push_back(std::move(out));
    }
    json::array recent;
    for (const auto &value : sources.journal) {
        if (!value.is_object() || !failure_record(value.as_object())) continue;
        const auto &record = value.as_object();
        json::object out;
        copy(out, record, {"timestamp", "event", "code", "kind", "action", "attempt", "attempts", "error",
                           "reason", "status", "phase"});
        if (auto model = record.if_contains("model"); model && model->is_string())
            out["model"] = model_name(model->as_string());
        if (auto id = record.if_contains("account_id"); id && id->is_string()) {
            const auto found = labels.find(std::string_view(id->as_string()));
            out["account"] = found == labels.end() ? "removed" : found->second;
        }
        recent.push_back(std::move(out));
    }
    json::array logs;
    if (auto list = sources.logs.if_contains("sources"); list && list->is_array())
        for (const auto &item : list->as_array())
            if (item.is_object()) {
                json::object out;
                copy(out, item.as_object(), {"id", "bytes"});
                logs.push_back(std::move(out));
            }
    json::object build;
    copy(build, sources.status, {"version", "build_mode"});
    copy(build, sources.runtime, {"build_mode"});
    return {{"report", "ais2api-diagnostics"}, {"format", 1}, {"generated_at", unix_seconds()},
            {"version", build.if_contains("version") ? build.at("version") : json::value("0.1.0")},
            {"build_mode", build.if_contains("build_mode") ? build.at("build_mode") : json::value(build_mode())},
            {"platform", sources.platform.empty() ? "unsupported" : sources.platform},
            {"service", service_part(sources.status)},
            {"runtime", runtime_part(sources.runtime)},
            {"browser", browser_part(sources.browser)},
            {"config", config_part(sources.config)},
            {"events", events_part(sources.events)},
            {"accounts", std::move(accounts)},
            {"recent", std::move(recent)},
            {"logs", std::move(logs)}};
}

json::array recent_failure_records(const logs::Journal &journal, std::size_t limit) {
    std::deque<json::object> kept;
    std::string cursor = "0";
    for (;;) {
        const auto page = journal.read(cursor, 500);
        if (auto entries = page.if_contains("entries"); entries && entries->is_array())
            for (const auto &entry : entries->as_array()) {
                auto record = entry.as_object().if_contains("record");
                if (!record || !record->is_object() || !failure_record(record->as_object())) continue;
                kept.push_back(record->as_object());
                if (kept.size() > limit) kept.pop_front();
            }
        if (!bool_or(page, "has_more")) break;
        const auto next = string_or(page, "next_cursor");
        if (next.empty() || next == cursor) break;
        cursor = next;
    }
    return json::array(kept.begin(), kept.end());
}
} // namespace ais2api
