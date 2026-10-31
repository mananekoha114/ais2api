// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "common.hpp"
namespace ais2api {
// Management password written to newly created configurations. While it is in
// use, management sessions may only replace it, and only from loopback or
// private-network peers (see local_network_address).
inline constexpr std::string_view default_admin_key = "admin";
struct Limits {
    std::size_t request_bytes = 32 * 1024 * 1024;
    std::size_t object_bytes = 64 * 1024 * 1024;
    std::size_t relay_bytes = 1024 * 1024;
    std::size_t queue_size = 8;
    unsigned queue_seconds = 30;
    unsigned generation_seconds = 240;
    unsigned stream_idle_seconds = 240;
    unsigned max_attempts = 3;
};
struct Config {
    int version = 1;
    std::string bind = "127.0.0.1";
    unsigned short port = 18451;
    std::string api_key;
    std::string admin_key;
    json::array admin_origins;
    std::string cookie_import_key;
    unsigned cookie_import_max_accounts = 8;
    unsigned cookie_import_timeout_seconds = 180;
    fs::path data_dir;
    fs::path config_file;
    fs::path browser_executable;
    std::string proxy;
    // Route the browser through a local relay that authenticates to `proxy`
    // (http:// or socks5://, with or without credentials). Without it the
    // browser uses `proxy` directly and cannot use one with credentials.
    bool browser_proxy_relay = false;
    // Let pages of any origin open the browser's DevTools WebSocket. The
    // gateway's own CDP client sends no Origin header and does not need it.
    bool browser_remote_allow_origins = false;
    // Chromium site isolation. Turning it off saves renderer memory on small
    // hosts at the cost of cross-site process separation.
    bool browser_site_isolation = true;
    bool headless = true;
    bool ignore_unsupported_parameters = false;
    // A `*-search` model request carrying function declarations either drops
    // them to keep Google Search (true) or keeps the caller's tools unchanged
    // without injecting search (false); upstream rejects mixing the two.
    bool search_model_drop_functions = true;
    // Optional memory saving: stop the browser after this many seconds without
    // work (0 = keep it running). The next request starts it again.
    unsigned browser_idle_seconds = 0;
    unsigned request_retention_days = 90;
    Limits limits;
    json::object model_aliases;
    json::array configured_models;
    json::object model_defaults;
    static fs::path default_directory();
    static Config load(const fs::path& file, const json::object& overrides = {});
    json::object to_json(bool portable_paths = false) const;
    void save() const;
    std::optional<std::string> authenticate(std::string_view secret) const;
    std::optional<std::string> authenticate_cookie_import(std::string_view secret) const;
    Config patched(const json::object& patch) const;
    std::optional<std::string> authenticate_admin(std::string_view secret) const;
    void validate() const;
    bool admin_key_is_default() const { return admin_key == default_admin_key; }
};
bool restart_required(const Config& previous, const Config& next);
// Throws 400 admin_password_invalid unless `next` may replace `current` as the
// management password: 8..4096 visible ASCII, not the default, not unchanged.
void validate_admin_password(std::string_view next, std::string_view current);
// Loopback, private (RFC 1918, IPv6 ULA) and link-local addresses, including
// IPv4-mapped IPv6. Unparseable text is not local.
bool local_network_address(std::string_view address);
}
