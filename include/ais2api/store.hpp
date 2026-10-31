// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "config.hpp"
#include <memory>
#include <limits>
namespace ais2api {
struct Account {
    std::string id, name, email;
    std::string auth_user = "0";
    json::object storage_state;
    bool disabled = false;
};
// Permanent per-account CloakBrowser identity. The seed is assigned when the
// account is first stored and only changes on explicit regeneration. Empty
// timezone or locale follow the host's region settings.
struct BrowserFingerprint {
    unsigned seed = 0;
    std::string timezone, locale;
};
// Initial seed for an account id (10000..99999). Deterministic so an account
// verified before it is stored (Cookie import) keeps the identity it was
// verified with.
unsigned default_fingerprint_seed(std::string_view account_id);
struct ModelCatalog {
    std::string account_id;
    json::array models;
    std::int64_t saved_at = 0;
};
struct RequestQuery {
    bool cumulative = false;
    std::int64_t from = 0, to = 0, before = 0, snapshot = 0;
    unsigned limit = 50;
    std::string model, account, status, id;
};
class Store {
public:
    explicit Store(const fs::path& directory);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    std::vector<Account> accounts();
    Account account(std::string_view id);
    json::array import_accounts(const json::value& input, std::string name = {});
    // Reports raw Cookie header leaves, which have no browser metadata.
    static json::object normalize_storage_state(const json::value& input, bool* raw_header = nullptr,
        std::size_t max_cookies = std::numeric_limits<std::size_t>::max());
    json::array merge_verified_accounts(const std::vector<Account>& accounts);
    void put_account(const Account& account);
    // Replaces the cookies of an account whose stored credentials still equal
    // `expected` (the state a browser session was built from); identity, expiry,
    // cooldowns and quota counters are untouched. Returns the stored state, or
    // nothing when the credentials were replaced meanwhile or are unchanged.
    std::optional<json::object> refresh_cookies(std::string_view id, const json::object& expected,
                                                const json::array& cookies);
    void update_email(std::string_view id, std::string_view email);
    void update_account_metadata(std::string_view id, const json::object& patch);
    void remove_account(std::string_view id);
    void select_account(std::string_view id);
    void reset_account(std::string_view id);
    // The current account, then the preferred account, then the first account.
    // Saved information remains readable during expiry and cooldowns.
    ModelCatalog model_catalog();
    ModelCatalog save_models(std::string_view account_id, json::array models);
    json::object rotation_status();
    std::string rotate_account();
    void clear_cooldown(std::string_view id, std::string_view model = {});
    void reset_stats();
    BrowserFingerprint fingerprint(std::string_view id);
    BrowserFingerprint regenerate_fingerprint(std::string_view id);
    // nullopt leaves a field unchanged; an empty string restores the host default.
    BrowserFingerprint set_fingerprint_region(std::string_view id, std::optional<std::string> timezone,
                                              std::optional<std::string> locale);
    // With `auth_probe`, when no account is available, one account blocked only
    // by an `auth` cooldown is admitted for a single attempt per cooldown period.
    std::optional<Account> choose(std::string_view model, const std::vector<std::string>& excluded = {},
                                  bool auth_probe = false);
    void failure(std::string_view account, std::string_view model, std::string_view kind, unsigned retry_seconds = 0);
    void success(std::string_view account, std::string_view model);
    json::object stats(const std::optional<RequestQuery> &query = std::nullopt);
    void begin_request(const json::object &record);
    // Terminal updates and lifetime aggregates commit together, exactly once.
    void save_request(const json::object &record, bool terminal);
    json::object requests(const RequestQuery &query);
    json::object request(std::string_view id);
    void recover_requests();
    void prune_requests(std::int64_t before);
    json::object price_sources();
    void save_price_source(std::string_view source, const json::object &catalog,
                           std::int64_t fetched_at, std::string_view error = {});
    json::object price_quote(std::string_view model);
    json::array account_status();
    json::object create_key(std::string name);
    json::array keys();
    void revoke_key(std::string_view id);
    std::optional<std::string> authenticate(std::string_view secret);
    json::object create_managed_key(std::string name);
    json::array managed_keys();
    void rename_managed_key(std::string_view id, std::string name);
    void revoke_managed_key(std::string_view id);
    std::optional<std::string> authenticate_managed(std::string_view secret);
    void put_tool(std::string_view principal, std::string_view call_id, const json::object& metadata);
    std::optional<json::object> get_tool(std::string_view principal, std::string_view call_id);
    void backup(const fs::path& target);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
