// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/management.hpp"
#include "ais2api/diagnostics.hpp"
#include "ais2api/log_sources.hpp"
#include "ais2api/region.hpp"
#include "ais2api/runtime.hpp"
#include <algorithm>
#include <cctype>

namespace ais2api {
namespace {
std::string identifier(const json::object& command) {
    auto id = string_or(command, "id");
    if (id.empty()) throw Error(400, "invalid_command", "An id is required");
    return id;
}

json::object account_summary(const json::object& input) {
    json::object output;
    for (auto key : {"id", "name", "email", "auth_user", "disabled", "expired", "failures", "fingerprint", "firefox_identity"}) {
        if (auto value = input.if_contains(key)) output[key] = *value;
    }
    return output;
}

std::string redact_proxy(std::string proxy) {
    auto begin = proxy.find("://");
    begin = begin == std::string::npos ? 0 : begin + 3;
    auto end = proxy.find_first_of("/?#", begin);
    auto at = proxy.rfind('@', end == std::string::npos ? proxy.size() : end);
    if (at != std::string::npos && at >= begin)
        proxy.replace(begin, at - begin, "***");
    if (auto query = proxy.find_first_of("?#"); query != std::string::npos)
        proxy.erase(query);
    return proxy;
}

bool sensitive_key(std::string key) {
    if (key == "cookie_import_max_accounts" || key == "cookie_import_timeout_seconds") return false;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "admin_key") return true;
    if (key == "token" || key.ends_with("_token") || key == "accesstoken" || key == "refreshtoken") return true;
    for (auto fragment : {"password", "secret", "cookie", "authorization", "api_key", "apikey", "thoughtsignature"})
        if (key.find(fragment) != std::string::npos) return true;
    return false;
}

void redact(json::value& value) {
    if (value.is_array()) {
        for (auto& item : value.as_array()) redact(item);
    } else if (value.is_object()) {
        for (auto& entry : value.as_object()) {
            if (sensitive_key(std::string(entry.key()))) entry.value() = "***";
            else if (entry.key() == "proxy" && entry.value().is_string())
                entry.value() = redact_proxy(std::string(entry.value().as_string()));
            else redact(entry.value());
        }
    }
}

// Stored values plus what the browser will actually present; empty stored
// timezone/locale follow this computer's region settings.
json::object fingerprint_view(const BrowserFingerprint& fingerprint) {
    const auto timezone = host_timezone(), locale = host_locale();
    return {{"seed", fingerprint.seed}, {"platform", fingerprint_platform()},
            {"timezone", fingerprint.timezone}, {"locale", fingerprint.locale},
            {"effective_timezone", fingerprint.timezone.empty() ? timezone : fingerprint.timezone},
            {"effective_locale", fingerprint.locale.empty() ? locale : fingerprint.locale},
            {"host_timezone", timezone}, {"host_locale", locale}};
}
}

json::object manage(Store& store, Config& config, const json::object& command) {
    auto name = string_or(command, "command");
    if (name == "accounts.fingerprint") return fingerprint_view(store.fingerprint(identifier(command)));
    if (name == "accounts.fingerprint.regenerate") return fingerprint_view(store.regenerate_fingerprint(identifier(command)));
    if (name == "accounts.fingerprint.update") {
        std::optional<std::string> timezone, locale;
        bool any = false;
        for (const auto& field : command) {
            const auto key = field.key();
            if (key == "command" || key == "id") continue;
            if (key != "timezone" && key != "locale")
                throw Error(400, "invalid_command", "Unknown fingerprint field");
            // null or "" restores the host default.
            if (!field.value().is_null() && !field.value().is_string())
                throw Error(400, "invalid_command", "Fingerprint timezone and locale must be strings or null");
            std::string value = field.value().is_string() ? std::string(field.value().as_string()) : std::string{};
            (key == "timezone" ? timezone : locale) = std::move(value);
            any = true;
        }
        if (!any) throw Error(400, "invalid_command", "Fingerprint update requires timezone or locale");
        return fingerprint_view(store.set_fingerprint_region(identifier(command), timezone, locale));
    }
    if (name == "accounts.list") {
        json::array accounts;
        for (auto& item : store.account_status()) accounts.push_back(account_summary(item.as_object()));
        return {{"accounts", accounts}};
    }
    if (name == "accounts.import") {
        auto input = command.if_contains("input");
        if (!input) throw Error(400, "invalid_command", "Account import input is required");
        auto imported = store.import_accounts(*input, string_or(command, "name"));
        json::array accounts;
        for (auto& item : imported) accounts.push_back(account_summary(item.as_object()));
        return {{"imported", accounts.size()}, {"accounts", accounts}};
    }
    if (name == "accounts.update") {
        for (const auto& field : command)
            if (field.key() != "command" && field.key() != "id" && field.key() != "name" && field.key() != "disabled")
                throw Error(400, "invalid_command", "Unknown account update field");
        auto id_value = command.if_contains("id");
        if (!id_value || !id_value->is_string()) throw Error(400, "invalid_command", "Account id must be a string");
        auto id = identifier(command);
        auto name_value = command.if_contains("name");
        auto disabled_value = command.if_contains("disabled");
        if ((!name_value && !disabled_value) || (name_value && !name_value->is_string()) || (disabled_value && !disabled_value->is_bool()))
            throw Error(400, "invalid_command", "Account update requires a string name or boolean disabled");
        json::object patch;
        if (name_value) patch["name"] = *name_value;
        if (disabled_value) patch["disabled"] = *disabled_value;
        store.update_account_metadata(id, patch);
        return {{"ok", true}, {"id", id}};
    }
    if (name == "accounts.select" || name == "accounts.remove" || name == "accounts.reset") {
        auto id = identifier(command);
        // Resolve first so removal/reset of an unknown id cannot claim success.
        (void)store.account(id);
        if (name == "accounts.select") store.select_account(id);
        else if (name == "accounts.remove") store.remove_account(id);
        else store.reset_account(id);
        return {{"ok", true}, {"id", id}};
    }
    if (name == "keys.list") {
        json::array keys;
        if (!config.api_key.empty())
            keys.push_back(json::object{{"id", "config"}, {"name", "Configured API key"},
                                        {"source", "config"}});
        return {{"source", "config"}, {"configured", !config.api_key.empty()}, {"keys", keys}};
    }
    if (name == "keys.create" || name == "keys.revoke")
        throw Error(409, "keys_managed_in_config",
                    "Edit api_key in config.json, then run config reload or restart; clear it to revoke access");
    if (name == "stats") return store.stats();
    if (name == "diagnostics.events") return store.backend_events();
    if (name == "diagnostics.report") {
        // The service is not running: no service status or browser state.
        DiagnosticsSources sources;
        sources.runtime = doctor(config);
        sources.config = config;
        sources.accounts = store.account_status();
        sources.events = store.backend_events();
        sources.journal = recent_failure_records(logs::Journal(config.data_dir));
        sources.logs = log_sources(config.data_dir);
        sources.platform = runtime_platform();
        return safe_report(sources);
    }
    if (name == "config.show") {
        json::value value(config.to_json());
        redact(value);
        return {{"config", value}, {"config_file", config.config_file.string()}};
    }
    if (name == "config.validate") {
        config.validate();
        return {{"valid", true}, {"config_file", config.config_file.string()}};
    }
    throw Error(400, "invalid_command", "Unknown management command");
}
}
