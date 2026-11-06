// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/camoufox.hpp"
#include "ais2api/browser.hpp"
#include "ais2api/region.hpp"
#include "ais2api/store.hpp"
#include "../browser/bidi.hpp"
#include "../browser/generation.hpp"
#include "../browser/google_auth.hpp"
#include "../browser/json_text.hpp"
#include "../browser/process.hpp"
#include "../browser/proxy_relay.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>

namespace ais2api {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
}

json::array browser_detail::bidi_cookies(const json::array& stored) {
    json::array result;
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& source = value.as_object();
        if (source.contains("partitionKey") || bool_or(source, "partitionKeyOpaque")) continue;
        const auto* name = source.if_contains("name");
        const auto* content = source.if_contains("value");
        const auto* domain = source.if_contains("domain");
        if (!name || !name->is_string() || name->as_string().empty() || !content || !content->is_string() ||
            !domain || !domain->is_string() || domain->as_string().empty()) continue;
        json::object cookie{{"name", *name}, {"value", {{"type", "string"}, {"value", *content}}}, {"domain", *domain}};
        if (auto* path = source.if_contains("path"); path && path->is_string() && !path->as_string().empty()) cookie["path"] = *path;
        for (auto key : {"httpOnly", "secure"}) if (auto* flag = source.if_contains(key); flag && flag->is_bool()) cookie[key] = *flag;
        if (auto* expires = source.if_contains("expires"); expires && expires->is_number()) {
            const auto seconds = json::value_to<double>(*expires);
            if (seconds > 0) cookie["expiry"] = static_cast<std::uint64_t>(std::floor(seconds));
        }
        // SameSite=None is only valid on secure cookies.
        const auto same_site = lower(string_or(source, "sameSite"));
        if (same_site == "strict" || same_site == "lax" || (same_site == "none" && bool_or(source, "secure"))) cookie["sameSite"] = same_site;
        result.push_back(std::move(cookie));
    }
    return result;
}

json::array browser_detail::stored_cookies(const json::array& bidi) {
    json::array result;
    for (const auto& value : bidi) {
        if (!value.is_object()) continue;
        const auto& source = value.as_object();
        const auto* content = source.if_contains("value");
        if (!content || !content->is_object() || string_or(content->as_object(), "type") != "string") continue;
        const auto* text = content->as_object().if_contains("value");
        const auto* name = source.if_contains("name");
        if (!text || !text->is_string() || !name || !name->is_string() || name->as_string().empty()) continue;
        json::object cookie{{"name", *name}, {"value", *text}, {"domain", string_or(source, "domain")}};
        if (auto* path = source.if_contains("path"); path && path->is_string()) cookie["path"] = *path;
        for (auto key : {"secure", "httpOnly"}) if (auto* flag = source.if_contains(key); flag && flag->is_bool()) cookie[key] = *flag;
        const auto same_site = string_or(source, "sameSite");
        if (same_site == "strict") cookie["sameSite"] = "Strict";
        else if (same_site == "lax") cookie["sameSite"] = "Lax";
        else if (same_site == "none") cookie["sameSite"] = "None";
        if (auto* expiry = source.if_contains("expiry"); expiry && expiry->is_number() && json::value_to<double>(*expiry) > 0) cookie["expires"] = *expiry;
        result.push_back(std::move(cookie));
    }
    return result;
}

json::array browser_detail::cookies_for_host(const json::array& stored, std::string_view host) {
    const auto target = lower(std::string(host));
    json::array result;
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& cookie = value.as_object();
        auto domain = lower(string_or(cookie, "domain"));
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        if (domain.empty() || (target != domain && !target.ends_with("." + domain))) continue;
        // Only cookies for the site root, the path the signing request uses.
        if (const auto path = string_or(cookie, "path"); !path.empty() && path != "/") continue;
        result.push_back(cookie);
    }
    return result;
}

std::optional<Error> browser_detail::firefox_navigation_failure(std::string_view message) {
    if (message.empty()) return std::nullopt;
    const auto has = [&](std::string_view part) { return message.find(part) != std::string_view::npos; };
    // NS_ERROR_PROXY_* (including a proxy's 407 Firefox could not answer) and
    // NS_ERROR_UNKNOWN_PROXY_HOST.
    if (has("NS_ERROR_PROXY_") || has("NS_ERROR_UNKNOWN_PROXY_HOST"))
        return Error(502, "proxy_failed", "The browser could not reach Google through the configured proxy (" + std::string(message) + ")");
    static constexpr std::string_view unreachable[] = {
        "NS_ERROR_UNKNOWN_HOST", "NS_ERROR_CONNECTION_REFUSED", "NS_ERROR_NET_TIMEOUT", "NS_ERROR_NET_RESET",
        "NS_ERROR_NET_INTERRUPT", "NS_ERROR_OFFLINE", "NS_ERROR_NET_EMPTY_RESPONSE"};
    if (std::any_of(std::begin(unreachable), std::end(unreachable), has) || has("SSL_ERROR_") || has("SEC_ERROR_"))
        return Error(502, "network_failed", "The browser could not reach Google (" + std::string(message) + ")");
    return std::nullopt;
}

std::optional<unsigned short> browser_detail::bidi_endpoint_port(std::string_view line) {
    for (std::string_view host : {"ws://127.0.0.1:", "ws://localhost:"}) {
        const auto start = line.find(host);
        if (start == std::string_view::npos) continue;
        auto digits = line.substr(start + host.size());
        std::size_t length = 0;
        while (length < digits.size() && length < 6 && std::isdigit(static_cast<unsigned char>(digits[length]))) ++length;
        if (!length || length > 5 || (length < digits.size() && digits[length] != '/' && !std::isspace(static_cast<unsigned char>(digits[length]))))
            return std::nullopt;
        const auto port = std::stoul(std::string(digits.substr(0, length)));
        if (!port || port > 65535) return std::nullopt;
        return static_cast<unsigned short>(port);
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> browser_detail::camoufox_environment(std::string_view config_json) {
    constexpr std::size_t piece = 2047;
    std::vector<std::pair<std::string, std::string>> result;
    while (!config_json.empty()) {
        const auto size = utf8_prefix(config_json, piece);
        result.emplace_back("CAMOU_CONFIG_" + std::to_string(result.size() + 1), std::string(config_json.substr(0, size)));
        config_json.remove_prefix(size);
    }
    return result;
}

std::string browser_detail::firefox_user_js(const json::object& prefs) {
    std::vector<std::string_view> names;
    for (const auto& item : prefs) names.emplace_back(item.key());
    std::sort(names.begin(), names.end());
    const auto quoted = [](std::string_view text, std::string_view name) {
        // The preference parser is not JSON: keep to text it reads the same way.
        if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20; }))
            throw Error(500, "browser_launch", "Firefox preference " + std::string(name) + " contains a control character");
        std::string result;
        append_quoted(result, text);
        return result;
    };
    std::string result;
    for (auto name : names) {
        const auto& value = prefs.at(name);
        std::string literal;
        if (value.is_string()) literal = quoted(value.as_string(), name);
        else if (value.is_bool()) literal = value.as_bool() ? "true" : "false";
        else if (value.is_int64() || value.is_uint64()) {
            const auto number = value.is_int64() ? value.as_int64() : static_cast<std::int64_t>(std::min<std::uint64_t>(value.as_uint64(), INT64_MAX));
            if (number < std::numeric_limits<std::int32_t>::min() || number > std::numeric_limits<std::int32_t>::max())
                throw Error(500, "browser_launch", "Firefox preference " + std::string(name) + " is out of range");
            literal = std::to_string(number);
        } else throw Error(500, "browser_launch", "Firefox preference " + std::string(name) + " has an unsupported type");
        result += "user_pref(" + quoted(name, name) + ", " + literal + ");\n";
    }
    return result;
}

browser_detail::CamoufoxIdentityPool browser_detail::load_identity_pool(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw Error(503, "browser_not_configured", "Camoufox identity pool missing: run scripts/camoufox-dev.py identities");
    std::stringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    boost::system::error_code error;
    auto value = json::parse(text, error);
    const auto invalid = [] { return Error(503, "browser_not_configured", "Camoufox identity pool is invalid: regenerate it with scripts/camoufox-dev.py"); };
    if (error || !value.is_object()) throw invalid();
    const auto& object = value.as_object();
    const auto* schema = object.if_contains("schema");
    const auto* identities = object.if_contains("identities");
    if (!schema || !schema->is_int64() || schema->as_int64() != 1 || !identities || !identities->is_array() || identities->as_array().empty()) throw invalid();
    CamoufoxIdentityPool pool;
    if (auto* camoufox = object.if_contains("camoufox"); camoufox && camoufox->is_object()) pool.camoufox = camoufox->as_object();
    for (const auto& item : identities->as_array()) {
        if (!item.is_object()) throw invalid();
        const auto* config = item.as_object().if_contains("config");
        const auto* prefs = item.as_object().if_contains("prefs");
        if (!config || !config->is_object() || config->as_object().empty() || (prefs && !prefs->is_object())) throw invalid();
        pool.identities.push_back(item.as_object());
    }
    pool.sha256 = sha256(text);
    return pool;
}

json::object browser_detail::camoufox_launch_config(const json::object& identity_config, std::string_view locale, std::string_view timezone) {
    auto config = identity_config;
    if (!locale.empty()) {
        // BCP 47 subtags: language first, then an optional 4-letter script and
        // a 2-letter or 3-digit region.
        std::vector<std::string> parts;
        for (std::size_t start = 0; start <= locale.size();) {
            const auto end = std::min(locale.find('-', start), locale.size());
            parts.emplace_back(locale.substr(start, end - start));
            start = end + 1;
        }
        const auto language = lower(parts.front());
        std::string region;
        for (std::size_t i = 1; i < parts.size(); ++i)
            if (parts[i].size() == 2 || (parts[i].size() == 3 && std::all_of(parts[i].begin(), parts[i].end(), [](unsigned char c) { return std::isdigit(c); }))) {
                region = parts[i];
                std::transform(region.begin(), region.end(), region.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            }
        const std::string tag(locale);
        json::array languages{json::value(tag)};
        if (language != tag) languages.emplace_back(language);
        config["navigator.language"] = tag;
        config["navigator.languages"] = languages;
        config["headers.Accept-Language"] = language != tag ? tag + "," + language + ";q=0.9" : tag;
        config["locale:language"] = language;
        if (!region.empty()) config["locale:region"] = region;
        else config.erase("locale:region");
        config["locale:all"] = language != tag ? tag + ", " + language : tag;
    }
    if (!timezone.empty()) config["timezone"] = std::string(timezone);
    // The bridge runs in the page's own world, as AI Studio's code does.
    config["allowMainWorld"] = true;
    return config;
}

namespace {
using namespace std::chrono_literals;
using browser_detail::Bidi;
using browser_detail::OwnedProcess;
using browser_detail::pause;

[[noreturn]] void incompatible(std::string message) { throw Error(502, "browser_incompatible", std::move(message)); }
std::string quote_js(std::string_view text) {
    std::string quoted;
    browser_detail::append_quoted(quoted, text);
    return quoted;
}

// Preferences every launch sets, over the identity's own.
json::object base_prefs() {
    return {
        // WebDriver BiDi is the only remote protocol.
        {"remote.active-protocols", 1},
        {"browser.shell.checkDefaultBrowser", false},
        // Fewer processes: extensions, networking and media decoding stay in
        // the parent process.
        {"extensions.webextensions.remote", false},
        {"network.process.enabled", false},
        {"media.rdd-process.enabled", false},
        {"media.utility-process.enabled", false},
        // A smaller JavaScript nursery and smaller shared surfaces.
        {"javascript.options.mem.nursery.max_kb", 4096},
        {"gfx.webrender.max-shared-surface-size", 512},
        {"webgl.force-enabled", true},
        // WebRTC may only use the proxy's route, like Chromium's
        // disable_non_proxied_udp, so ICE candidates cannot reveal the address.
        {"media.peerconnection.ice.default_address_only", true},
        {"media.peerconnection.ice.no_host", true},
        {"media.peerconnection.ice.proxy_only_if_behind_proxy", true}};
}

// Preferences sending all traffic through `proxy`, never around it.
json::object proxy_prefs(const ProxyEndpoint& proxy) {
    json::object prefs{{"network.proxy.type", 1}, {"network.proxy.failover_direct", false}, {"network.proxy.no_proxies_on", ""}};
    if (proxy.scheme == "http") {
        for (std::string kind : {"http", "ssl"}) {
            prefs["network.proxy." + kind] = proxy.host;
            prefs["network.proxy." + kind + "_port"] = proxy.port;
        }
    } else if (proxy.scheme == "socks4" || proxy.scheme == "socks4a" || proxy.scheme == "socks5" || proxy.scheme == "socks5h") {
        prefs["network.proxy.socks"] = proxy.host;
        prefs["network.proxy.socks_port"] = proxy.port;
        prefs["network.proxy.socks_version"] = proxy.scheme.starts_with("socks4") ? 4 : 5;
        // Chromium resolves names through a SOCKS5 proxy as well.
        const bool remote_dns = proxy.scheme != "socks4";
        prefs["network.proxy.socks_remote_dns"] = remote_dns;
        prefs["network.proxy.socks5_remote_dns"] = remote_dns;
    } else {
        throw Error(400, "invalid_config", "The camoufox backend needs an http:// or socks proxy URL");
    }
    return prefs;
}

// The major version in a Firefox user agent ("... Firefox/158.0").
std::string firefox_major(std::string_view user_agent) {
    const auto at = user_agent.find("Firefox/");
    if (at == std::string_view::npos) return {};
    auto rest = user_agent.substr(at + 8);
    return std::string(rest.substr(0, std::min(rest.find_first_not_of("0123456789"), rest.size())));
}
} // namespace

struct CamoufoxGateway::Impl {
    Config config;
    Store& store;
    std::mutex mutex;
    OwnedProcess process;
    std::unique_ptr<Bidi> bidi;
    // Lives exactly as long as the browser process it serves.
    std::unique_ptr<browser_detail::ProxyRelay> relay;
    std::string relay_error;
    fs::path root;
    std::string user_context, page, account_id, account_fingerprint;
    json::array base_template;
    std::string timezone = "UTC";
    bool models_discovered = false;
    browser_detail::GoogleAuthorizationCache authorization_cache;
    std::string last_status = "not_started";
    std::string phase = "idle";
    std::string browser_name, browser_version, user_agent;
    // Camoufox applies an identity per process, so the running browser is tied
    // to the identity it was launched with. No pool: no identity (verify_runtime).
    struct LaunchIdentity {
        std::string pool;
        std::size_t index = 0;
        unsigned seed = 0;
        std::string timezone, locale;
        bool operator==(const LaunchIdentity&) const = default;
    };
    std::optional<LaunchIdentity> launched;
    // Reloaded when the file changes.
    std::optional<browser_detail::CamoufoxIdentityPool> pool;
    fs::file_time_type pool_time{};
    // Browser output, written on the process reader thread.
    std::mutex output_mutex;
    std::ofstream output_log;
    std::deque<std::string> output_tail;
    unsigned short endpoint_port = 0;
    // Credentials the current context was built from, for write-back CAS.
    json::object session_state;
    Clock::time_point cookies_saved_at{};
    std::string cookie_refresh = "not_attempted";
    explicit Impl(const Config& c, Store& s) : config(c), store(s) {}
    ~Impl() { close(); }
    void close() noexcept {
        authorization_cache.clear();
        if (bidi) bidi->close();
        launched.reset();
        bidi.reset(); process.stop();
        if (relay) { relay_error = relay->last_error(); relay.reset(); }
        { std::lock_guard lock(output_mutex); output_log.close(); }
        user_context.clear(); page.clear(); account_id.clear(); account_fingerprint.clear(); base_template.clear(); models_discovered = false; session_state = {};
        if (!root.empty()) { std::error_code ec; fs::remove_all(root, ec); root.clear(); }
        last_status = "closed";
    }
    const browser_detail::CamoufoxIdentityPool& identity_pool() {
        const auto file = config.data_dir / "camoufox" / "identity-pool.json";
        std::error_code error;
        const auto time = fs::last_write_time(file, error);
        if (error) {
            pool.reset();
            throw Error(503, "browser_not_configured", "Camoufox identity pool missing: run scripts/camoufox-dev.py identities");
        }
        if (!pool || time != pool_time) {
            pool.reset();
            pool = browser_detail::load_identity_pool(file);
            pool_time = time;
        }
        return *pool;
    }
    LaunchIdentity identity_for(const Account& account) {
        BrowserFingerprint stored;
        try {
            stored = store.fingerprint(account.id);
        } catch (const Error& error) {
            if (error.code != "account_not_found") throw;
            // Cookie import verifies an account before storing it; use the
            // seed the store will assign to that id.
            stored.seed = default_fingerprint_seed(account.id);
        }
        const auto& identities = identity_pool();
        return {identities.sha256, stored.seed % identities.identities.size(), stored.seed,
                stored.timezone.empty() ? host_timezone() : stored.timezone, stored.locale.empty() ? host_locale() : stored.locale};
    }
    json::value eval(std::string expression, std::stop_token token, std::chrono::seconds timeout = 30s) { return bidi->evaluate(std::move(expression), page, token, timeout); }
    json::object partition() const { return {{"type", "storageKey"}, {"userContext", user_context}}; }
    // The context's cookies in the stored shape.
    json::array context_cookies(std::stop_token token, std::chrono::seconds timeout = 30s) {
        auto reply = bidi->call("storage.getCookies", {{"partition", partition()}}, token, timeout);
        const auto* cookies = reply.if_contains("cookies");
        if (!cookies || !cookies->is_array()) incompatible("Browser did not return context cookies");
        return browser_detail::stored_cookies(cookies->as_array());
    }
    void set_cookies(const json::array& cookies, std::stop_token token) {
        std::size_t rejected = 0;
        for (const auto& cookie : cookies) {
            auto reply = bidi->exchange("storage.setCookie", {{"cookie", cookie}, {"partition", partition()}}, token);
            if (reply.error.empty()) continue;
            // Firefox refuses a cookie it would not accept from a response;
            // the others can still make a usable session.
            if (reply.error != "unable to set cookie" && reply.error != "invalid argument") throw Bidi::command_error("storage.setCookie", reply);
            ++rejected;
        }
        if (!cookies.empty() && rejected == cookies.size()) incompatible("Browser refused every account cookie");
    }
    void preload(std::string declaration, std::stop_token token) {
        bidi->call("script.addPreloadScript", {{"functionDeclaration", std::move(declaration)}, {"contexts", json::array{json::value(page)}}}, token);
    }
    void install_bridge(std::stop_token token) {
        preload("() => {\n" + std::string(browser_detail::embedded_bridge_source()) + "\n}", token);
    }
    // Sends a request body to the page in pieces passed as call arguments, as
    // BrowserGateway does over CDP.
    std::string upload(std::string_view body, std::stop_token token) {
        constexpr std::size_t piece_bytes = 512 * 1024;
        const auto id = random_hex(8);
        std::size_t offset = 0;
        do {
            const auto size = browser_detail::utf8_prefix(body.substr(offset), piece_bytes);
            bidi->call_function("function(id, chunk) { return globalThis.__ais2bridge.append(id, chunk); }", {id, body.substr(offset, size)}, page, token);
            offset += size;
        } while (offset < body.size());
        return id;
    }
    // One trusted Ctrl+Enter (U+E009, U+E007): every identity is a Windows device.
    void trusted_shortcut(std::stop_token token) {
        const std::string control = "\xEE\x80\x89", enter = "\xEE\x80\x87";
        json::array keys{json::object{{"type", "keyDown"}, {"value", control}}, json::object{{"type", "keyDown"}, {"value", enter}},
                         json::object{{"type", "keyUp"}, {"value", enter}}, json::object{{"type", "keyUp"}, {"value", control}}};
        bidi->call("input.performActions", {{"context", page}, {"actions", json::array{json::object{{"type", "key"}, {"id", "keyboard"}, {"actions", std::move(keys)}}}}}, token, 30s, page);
        bidi->call("input.releaseActions", {{"context", page}}, token, 30s, page);
    }
    std::string authorization(std::stop_token token) {
        check_cancel(token);
        // The selected context's own cookie store, including HttpOnly values;
        // never the import file or another context.
        auto result = authorization_cache.authorization([&] { return browser_detail::cookies_for_host(context_cookies(token, 5s), "aistudio.google.com"); });
        check_cancel(token);
        return result;
    }
    std::string output_hint() {
        std::lock_guard lock(output_mutex);
        if (output_tail.empty()) return {};
        const std::string_view line = output_tail.back();
        return " (" + std::string(line.substr(0, browser_detail::utf8_prefix(line, 300))) + ")";
    }
    void ensure_browser(std::stop_token token, const LaunchIdentity& identity) {
        check_cancel(token);
        if (bidi && bidi->open() && process.alive() && launched == identity) return;
        // A different identity needs a new browser process.
        close();
        if (config.browser_executable.empty() || !fs::is_regular_file(config.browser_executable))
            throw Error(503, "browser_not_configured", "Configure browser_executable as the Camoufox executable");
        if (lower(path_utf8(config.browser_executable.stem())) != "camoufox")
            throw Error(503, "browser_not_configured", "browser_backend camoufox needs browser_executable to name the Camoufox executable (camoufox.exe)");
        try {
            json::object identity_config, prefs;
            user_agent.clear();
            if (!identity.pool.empty()) {
                const auto& entry = identity_pool().identities.at(identity.index);
                identity_config = entry.at("config").as_object();
                if (const auto* own = entry.if_contains("prefs")) prefs = own->as_object();
                user_agent = string_or(identity_config, "navigator.userAgent");
            }
            for (const auto& item : base_prefs()) prefs[item.key()] = item.value();
            if (!config.proxy.empty()) {
                const auto endpoint = parse_proxy(config.proxy);
                if (config.browser_proxy_relay) {
                    if (!endpoint || !relay_supports(*endpoint)) throw Error(400, "invalid_config", "browser_proxy_relay needs an http:// or socks5:// proxy URL");
                    relay = std::make_unique<browser_detail::ProxyRelay>();
                    for (const auto& item : proxy_prefs({"http", "127.0.0.1", relay->start(*endpoint)})) prefs[item.key()] = item.value();
                } else {
                    if (!endpoint) throw Error(400, "invalid_config", "The camoufox backend needs a single proxy URL");
                    // The profile is readable by other local processes of this
                    // user: never write credentials into it.
                    if (endpoint->credentials)
                        throw Error(503, "proxy_auth_unsupported", "The browser cannot use a proxy with credentials directly; enable browser_proxy_relay");
                    for (const auto& item : proxy_prefs(*endpoint)) prefs[item.key()] = item.value();
                }
            }
            root = config.data_dir / "runtime" / ("camoufox-" + random_hex(12));
            secure_directory(root);
            const auto profile = root / "profile";
            secure_directory(profile);
            {
                std::ofstream user_js(profile / "user.js", std::ios::binary);
                user_js << browser_detail::firefox_user_js(prefs);
                if (!user_js) throw Error(500, "browser_launch", "Cannot write the Camoufox profile");
            }
            browser_detail::ProcessOptions options;
            options.environment = browser_detail::camoufox_environment(
                json::serialize(browser_detail::camoufox_launch_config(identity_config, identity.locale, identity.timezone)));
            // Only this launch's identity reaches the browser.
            options.unset_prefixes = {"CAMOU_CONFIG_", "CAMOU_PREFS_"};
            // Persist browser output beyond the profile cleanup.
            secure_directory(config.data_dir / "logs");
            browser_detail::prune_browser_logs(config.data_dir / "logs", "camoufox-");
            {
                std::lock_guard lock(output_mutex);
                output_log.open(config.data_dir / "logs" / (path_utf8(root.filename()) + ".log"), std::ios::binary | std::ios::app);
                output_tail.clear();
                endpoint_port = 0;
            }
            options.on_output = [this](std::string_view line) {
                std::lock_guard lock(output_mutex);
                if (output_log.is_open()) { output_log << line << '\n'; output_log.flush(); }
                if (!endpoint_port && line.find("WebDriver BiDi listening on") != std::string_view::npos)
                    if (const auto port = browser_detail::bidi_endpoint_port(line)) endpoint_port = *port;
                output_tail.emplace_back(line.substr(0, 1024));
                if (output_tail.size() > 20) output_tail.pop_front();
            };
            // -wait-for-browser keeps the Windows launcher process until the
            // browser exits, so the owned process is the browser's lifetime.
            std::vector<std::string> args{"--remote-debugging-port", "0", "-no-remote", "-wait-for-browser", "-profile", path_utf8(fs::absolute(profile))};
            if (config.headless) args.emplace_back("-headless");
            args.emplace_back("about:blank");
            process.start(config.browser_executable, args, root, options);
            const auto deadline = Clock::now() + 45s;
            unsigned short port = 0;
            while (!port) {
                check_cancel(token);
                { std::lock_guard lock(output_mutex); port = endpoint_port; }
                if (port) break;
                if (!process.alive()) throw Error(502, "browser_launch", "Camoufox exited before WebDriver BiDi became ready" + output_hint());
                if (Clock::now() >= deadline) throw Error(504, "browser_timeout", "Camoufox did not announce a WebDriver BiDi endpoint" + output_hint());
                pause(token, 100ms);
            }
            bidi = std::make_unique<Bidi>();
            bidi->connect(port, "/session", token);
            const auto session = bidi->call("session.new", {{"capabilities", json::object{}}}, token);
            if (const auto* capabilities = session.if_contains("capabilities"); capabilities && capabilities->is_object()) {
                browser_name = string_or(capabilities->as_object(), "browserName");
                browser_version = string_or(capabilities->as_object(), "browserVersion");
            }
            bidi->call("session.subscribe", {{"events", json::array{"browsingContext.contextDestroyed"}}}, token);
            launched = identity;
            last_status = "browser_ready";
        } catch (...) { close(); throw; }
    }
    void dispose_context(std::stop_token token) {
        authorization_cache.clear();
        // Removing the user context closes its page and drops its storage.
        if (!user_context.empty()) bidi->call("browser.removeUserContext", {{"userContext", user_context}}, token);
        user_context.clear(); page.clear(); account_id.clear(); account_fingerprint.clear(); base_template.clear(); models_discovered = false; session_state = {};
    }
    // Google rotates session cookies while the page is in use. Persist the
    // verified context's current cookies so later sessions do not restart from
    // the stale imported set. Best effort; only cancellation propagates.
    void persist_cookies(const Account& account, std::stop_token token) {
        if (user_context.empty() || session_state.empty()) return;
        try {
            cookies_saved_at = Clock::now();
            const auto cookies = context_cookies(token, 5s);
            auto stored = session_state.if_contains("cookies");
            auto merged = browser_detail::refreshed_cookies(stored && stored->is_array() ? stored->as_array() : json::array{}, cookies);
            if (merged.empty()) { cookie_refresh = "failed"; return; }
            if (!store.refresh_cookies(account.id, session_state, merged)) { cookie_refresh = "unchanged"; return; }
            // Adopt the stored form so the next request's account matches this
            // session's fingerprint instead of triggering a context rebuild.
            const auto saved = store.account(account.id);
            session_state = saved.storage_state;
            account_fingerprint = sha256(json::serialize(saved.storage_state) + "\n" + saved.auth_user + "\n" + saved.email);
            cookie_refresh = "saved";
        } catch (const Error& error) {
            if (error.code == "cancelled") throw;
            cookie_refresh = "failed";
        }
    }
    json::object identity(std::stop_token token, bool allow_login = false, std::chrono::seconds timeout = 30s) {
        auto value = eval("globalThis.__ais2bridge.identity()", token, timeout);
        if (!value.is_object()) incompatible("Page identity probe unavailable");
        auto object = value.as_object();
        if (bool_or(object, "login") && !allow_login) throw Error(401, "session_expired", "Google redirected the account to sign-in");
        if (bool_or(object, "regionBlocked")) throw Error(403, "region_blocked", "AI Studio is unavailable in the browser region");
        return object;
    }
    void await_account_identity(const Account& account, std::stop_token token, bool require_editor) {
        const auto deadline = Clock::now() + 60s;
        std::optional<Clock::time_point> login_since, mismatch_since;
        auto last = browser_detail::IdentityVerdict::pending;
        while (Clock::now() < deadline) {
            check_cancel(token);
            try {
                auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                if (remaining < 1s) break;
                auto exists = eval("!!globalThis.__ais2bridge", token, std::min(5s, remaining));
                if (exists.is_bool() && exists.as_bool()) {
                    remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now());
                    if (remaining < 1s) break;
                    const auto info = identity(token, true, std::min(5s, remaining));
                    if (bool_or(info, "login")) {
                        mismatch_since.reset();
                        if (!login_since) login_since = Clock::now();
                        if (Clock::now() - *login_since >= 10s)
                            throw Error(401, "session_expired", "Google remained on sign-in after the navigation grace period");
                    } else {
                        login_since.reset();
                        last = browser_detail::identity_verdict(info, account.email);
                        if (last == browser_detail::IdentityVerdict::verified && (!require_editor || bool_or(info, "ready"))) return;
                        if (last == browser_detail::IdentityVerdict::mismatch) {
                            if (!mismatch_since) mismatch_since = Clock::now();
                            if (Clock::now() - *mismatch_since >= 2s)
                                throw Error(401, "identity_mismatch", "The verified Google account differs from the selected account");
                        } else mismatch_since.reset();
                    }
                } else { login_since.reset(); mismatch_since.reset(); last = browser_detail::IdentityVerdict::pending; }
            } catch (const Error& error) {
                // Redirects can replace the page realm while the account badge
                // is loading. Missing identity is never a proven mismatch.
                if (error.code != "browser_incompatible") throw;
                login_since.reset(); mismatch_since.reset(); last = browser_detail::IdentityVerdict::pending;
            }
            pause(token, 250ms);
        }
        if (last == browser_detail::IdentityVerdict::verified)
            incompatible("AI Studio page did not expose a supported ready prompt editor");
        throw Error(409, "identity_unverified", last == browser_detail::IdentityVerdict::ambiguous
            ? "AI Studio exposed multiple account identities" : "AI Studio account identity did not finish loading");
    }
    void navigate(const std::string& url, std::stop_token token) {
        const auto reply = bidi->exchange("browsingContext.navigate", {{"context", page}, {"url", url}, {"wait", "interactive"}}, token, 45s, page);
        if (reply.error.empty()) return;
        // Without this an unreachable page waits out the identity check and is
        // blamed on the account.
        if (auto failure = browser_detail::firefox_navigation_failure(reply.message)) {
            if (failure->code == "proxy_failed" && relay)
                if (const auto detail = relay->last_error(); !detail.empty()) failure = Error(failure->status, failure->code, std::string(failure->what()) + ": " + detail);
            throw *failure;
        }
        // A redirect that superseded the navigation (NS_BINDING_ABORTED) is no
        // failure: the identity check judges the page it lands on.
        if (auto error = Bidi::command_error("browsingContext.navigate", reply); error.code == "browser_disconnected") throw error;
    }
    void ensure_account(const Account& account, std::stop_token token, bool bootstrap, bool identity_only = false) {
        phase = "browser_start";
        ensure_browser(token, identity_for(account));
        auto fingerprint = sha256(json::serialize(account.storage_state) + "\n" + account.auth_user + "\n" + account.email);
        bool verified_session = false;
        if (account.id != account_id || fingerprint != account_fingerprint || page.empty()) {
            // Dispose before create: never keep two account contexts resident.
            dispose_context(token);
            try {
                user_context = string_or(bidi->call("browser.createUserContext", {}, token), "userContext");
                if (user_context.empty()) incompatible("Browser did not create an isolated context");
                auto cookies = account.storage_state.if_contains("cookies");
                if (!cookies || !cookies->is_array() || cookies->as_array().empty()) throw Error(401, "missing_cookies", "Account has no storage-state cookies");
                set_cookies(browser_detail::bidi_cookies(browser_detail::injected_cookies(cookies->as_array())), token);
                page = string_or(bidi->call("browsingContext.create", {{"type", "tab"}, {"userContext", user_context}}, token), "context");
                if (page.empty()) incompatible("Browser did not create an isolated page");
                install_bridge(token);
                if (auto origins = account.storage_state.if_contains("origins"); origins && origins->is_array())
                    preload("() => {const entries=" + json::serialize(*origins) + ";for(const e of entries){if(e.origin!==location.origin||!Array.isArray(e.localStorage))continue;for(const p of e.localStorage){if(typeof p.name==='string'&&typeof p.value==='string')localStorage.setItem(p.name,p.value);}}}", token);
                if (account.auth_user.empty() || !std::all_of(account.auth_user.begin(), account.auth_user.end(), [](unsigned char c) { return std::isdigit(c); })) throw Error(400, "invalid_auth_user", "auth_user must be a nonnegative integer");
                navigate("https://aistudio.google.com/prompts/new_chat?authuser=" + account.auth_user, token);
                phase = "account_identity";
                await_account_identity(account, token, !identity_only);
                account_id = account.id; account_fingerprint = std::move(fingerprint); last_status = "account_ready";
                session_state = account.storage_state;
                verified_session = true;
                persist_cookies(account, token);
            } catch (...) { try { dispose_context({}); } catch (...) { close(); } throw; }
        }
        if (!verified_session) {
            phase = "account_identity";
            await_account_identity(account, token, !identity_only);
        }
        if (!bootstrap || !base_template.empty()) return;
        phase = "bootstrap_prepare";
        auto prepared = eval("globalThis.__ais2bridge.bootstrap()", token);
        if (!prepared.is_bool() || !prepared.as_bool()) incompatible("Cannot prepare the real page bootstrap prompt");
        bool template_seen = false;
        try {
            pause(token, 350ms);
            phase = "bootstrap_run";
            auto triggered = eval("globalThis.__ais2bridge.run()", token);
            // Without a Run button the bridge sent the editor shortcut as a
            // page event; follow with one trusted key press if nothing arrives.
            const bool shortcut = triggered.is_string() && triggered.as_string() == "shortcut";
            if (!shortcut && (!triggered.is_bool() || !triggered.as_bool())) incompatible("Cannot identify a supported page Run button");
            const auto triggered_at = Clock::now();
            bool trusted_shortcut_sent = !shortcut;
            const auto deadline = triggered_at + 45s;
            phase = "bootstrap_capture";
            while (Clock::now() < deadline) {
                if (!trusted_shortcut_sent && !template_seen && Clock::now() - triggered_at >= 1s) {
                    trusted_shortcut_sent = true;
                    trusted_shortcut(token);
                }
                identity(token);
                auto capability = eval("globalThis.__ais2bridge.capability()", token);
                if (capability.is_object()) {
                    auto t = capability.as_object().if_contains("template");
                    if (t && t->is_object()) {
                        auto body = t->as_object().if_contains("body");
                        if (body && body->is_array()) {
                            template_seen = true;
                            if (bool_or(capability.as_object(), "snapshot")) {
                                base_template = body->as_array();
                                timezone = string_or(capability.as_object(), "timezone", "UTC");
                                phase = "bootstrap_finish";
                                eval("globalThis.__ais2bridge.finishBootstrap()", token);
                                last_status = "gateway_ready"; phase = "ready"; return;
                            }
                        }
                    }
                }
                pause(token, 150ms);
            }
        } catch (...) {
            try { eval("globalThis.__ais2bridge.finishBootstrap()", {}, 2s); } catch (...) {}
            throw;
        }
        try { eval("globalThis.__ais2bridge.finishBootstrap()", {}, 2s); } catch (...) {}
        last_status = template_seen ? "bootstrap_snapshot_missing" : "bootstrap_template_missing";
        incompatible(template_seen
            ? "Live GenerateContent was captured but its snapshot adapter could not be verified"
            : "The page did not expose a supported fetch/XHR GenerateContent request during bootstrap");
    }
    // What generation depends on, checked on the current (local) page.
    json::object page_checks(std::stop_token token) {
        json::object result;
        result["bridge"] = eval("typeof globalThis.__ais2bridge === 'object' && globalThis.__ais2bridge !== null", token);
        result["webdriver"] = eval("navigator.webdriver", token);
        result["byobReader"] = eval("(() => { try { new Response('x').body.getReader({mode: 'byob'}).releaseLock(); return true; } catch (error) { return false; } })()", token);
        result["userAgent"] = eval("navigator.userAgent", token);
        eval("(() => { globalThis.__ais2keys = []; addEventListener('keydown', (event) => globalThis.__ais2keys.push({key: event.key, control: event.ctrlKey, trusted: event.isTrusted}), true); return true; })()", token);
        trusted_shortcut(token);
        bool trusted = false;
        for (int attempt = 0; attempt < 10 && !trusted; ++attempt) {
            pause(token, 100ms);
            const auto keys = eval("globalThis.__ais2keys", token);
            if (keys.is_array()) for (const auto& key : keys.as_array())
                trusted = trusted || (key.is_object() && string_or(key.as_object(), "key") == "Enter" && bool_or(key.as_object(), "control") && bool_or(key.as_object(), "trusted"));
        }
        result["trustedKey"] = trusted;
        return result;
    }
};

CamoufoxGateway::CamoufoxGateway(const Config& config, Store& store) : impl_(std::make_unique<Impl>(config, store)) {}
CamoufoxGateway::~CamoufoxGateway() = default;
json::object CamoufoxGateway::verify_runtime(std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    try {
        impl_->ensure_browser(cancel, {{}, 0, 0, host_timezone(), host_locale()});
        auto& bidi = *impl_->bidi;
        impl_->user_context = string_or(bidi.call("browser.createUserContext", {}, cancel), "userContext");
        if (impl_->user_context.empty()) incompatible("Browser did not create a verification context");
        impl_->page = string_or(bidi.call("browsingContext.create", {{"type", "tab"}, {"userContext", impl_->user_context}}, cancel), "context");
        if (impl_->page.empty()) incompatible("Browser did not create a verification page");
        impl_->install_bridge(cancel);
        bidi.call("browsingContext.navigate", {{"context", impl_->page}, {"url", "data:text/html,<title>ais2api</title><textarea autofocus></textarea>"}, {"wait", "complete"}}, cancel, 30s, impl_->page);
        auto page = impl_->page_checks(cancel);
        impl_->dispose_context(cancel);
        json::object result{{"product", impl_->browser_name + "/" + impl_->browser_version}, {"protocol_version", "webdriver-bidi"},
                            {"isolated_context", true}, {"google_verified", false}, {"page", std::move(page)}};
        impl_->close();
        return result;
    } catch (...) { impl_->close(); throw; }
}
void CamoufoxGateway::close() { std::lock_guard lock(impl_->mutex); impl_->close(); }
void CamoufoxGateway::reset_session() {
    std::lock_guard lock(impl_->mutex);
    // A new user context reloads cookies, the page, BotGuard and the template.
    try {
        if (impl_->bidi && impl_->process.alive()) impl_->dispose_context({});
        else impl_->close();
    } catch (...) { impl_->close(); }
}
void CamoufoxGateway::clear_template() {
    std::lock_guard lock(impl_->mutex);
    impl_->base_template.clear();
}
bool CamoufoxGateway::release_idle() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->bidi && !impl_->process.alive()) return false;
    if (!impl_->account_id.empty())
        try { impl_->persist_cookies(impl_->store.account(impl_->account_id), {}); } catch (...) {}
    impl_->close();
    impl_->last_status = "idle_closed";
    impl_->phase = "idle";
    return true;
}
void CamoufoxGateway::reconfigure(const Config& config) {
    std::lock_guard lock(impl_->mutex);
    impl_->config.model_aliases = config.model_aliases;
    impl_->config.model_defaults = config.model_defaults;
    impl_->config.continuation_retry_enabled = config.continuation_retry_enabled;
    impl_->config.limits.max_continuation_retries = config.limits.max_continuation_retries;
}
json::object CamoufoxGateway::diagnostics() {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return {{"status", "busy"}, {"phase", "active"}, {"backend", "camoufox"}, {"bridgeSource", "embedded"},
                {"bridgeSha256", browser_detail::embedded_bridge_sha256()}};
    const auto& launched = impl_->launched;
    json::value fingerprint = nullptr, identity = nullptr, browser = nullptr;
    bool version_mismatch = false;
    if (launched) {
        fingerprint = json::object{{"seed", launched->seed}, {"platform", "windows"}, {"timezone", launched->timezone}, {"locale", launched->locale}};
        browser = json::object{{"name", impl_->browser_name}, {"version", impl_->browser_version}};
        if (!launched->pool.empty()) {
            identity = json::object{{"pool", launched->pool.substr(0, 12)}, {"index", launched->index}, {"userAgent", impl_->user_agent}};
            const auto claimed = firefox_major(impl_->user_agent);
            const auto running = impl_->browser_version.substr(0, impl_->browser_version.find('.'));
            version_mismatch = !claimed.empty() && !running.empty() && claimed != running;
        }
    }
    return {{"status", impl_->last_status}, {"phase", impl_->phase}, {"backend", "camoufox"},
        {"bridgeSource", "embedded"}, {"bridgeSha256", browser_detail::embedded_bridge_sha256()},
        {"running", impl_->process.alive()}, {"isolatedContext", !impl_->user_context.empty()}, {"templateCaptured", !impl_->base_template.empty()}, {"modelDiscovery", impl_->models_discovered ? "live_rpc" : "not_loaded"}, {"chunkBytes", 32768},
        {"cookieRefresh", impl_->cookie_refresh},
        {"proxy", json::object{{"mode", impl_->config.proxy.empty() ? "none" : impl_->config.browser_proxy_relay ? "relay" : "direct"},
                               {"relayError", impl_->relay ? impl_->relay->last_error() : impl_->relay_error}}},
        {"fingerprint", std::move(fingerprint)}, {"browser", std::move(browser)}, {"identity", std::move(identity)},
        {"identityVersionMismatch", version_mismatch}};
}
json::object CamoufoxGateway::probe(const Account& account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, false, true);
    auto result = impl_->identity(cancel); result["accountId"] = account.id;
    result["ready"] = bool_or(result, "identityReady");
    result["status"] = string_or(result, "email").empty() ? "identity_unverified" : "ready";
    result["generationVerified"] = false;
    return result;
}
json::object CamoufoxGateway::probe_import(Account& account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, false, true);
    const auto info = impl_->identity(cancel);
    const auto email = string_or(info, "email");
    if (!bool_or(info, "identityReady") || bool_or(info, "identityAmbiguous") || email.empty())
        throw Error(409, "identity_unverified", "Import requires a ready AI Studio page with one verified Google identity");
    std::size_t skipped_partition_cookies = 0;
    auto refreshed = browser_detail::import_cookies(impl_->context_cookies(cancel), &skipped_partition_cookies);
    if (refreshed.empty()) throw Error(401, "missing_cookies", "Verified browser context has no Google cookies");
    check_cancel(cancel);
    account.storage_state["cookies"] = std::move(refreshed);
    impl_->account_fingerprint = sha256(json::serialize(account.storage_state) + "\n" + account.auth_user + "\n" + account.email);
    return {{"accountId", account.id}, {"email", email}, {"ready", true}, {"login", false}, {"identityAmbiguous", false}, {"status", "ready"}, {"generationVerified", false}, {"skippedPartitionCookies", skipped_partition_cookies}};
}
json::array CamoufoxGateway::models(const Account& account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    check_cancel(cancel);
    impl_->ensure_account(account, cancel, false, true);
    const auto authorization = impl_->authorization(cancel);
    auto result = impl_->eval("globalThis.__ais2bridge.discoverModels(" + quote_js(account.auth_user) + "," + quote_js(authorization) + ")", cancel, 25s);
    if (!result.is_array()) incompatible("ListModels returned an unsupported response");
    impl_->models_discovered = true;
    return result.as_array();
}
void CamoufoxGateway::generate(const Request& request, const Account& account, const EventSink& sink, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, true);
    impl_->phase = "snapshot_ticket";
    auto wire = encode_request(request, impl_->base_template, "", impl_->timezone);
    // Covers the bridge's bounded snapshot retries (3 x 9 s).
    auto snapshot = impl_->eval("globalThis.__ais2bridge.snapshot(" + quote_js(browser_detail::wire_content_hash(wire)) + ")", cancel, 30s);
    if (!snapshot.is_string() || snapshot.as_string().empty()) incompatible("Snapshot service produced no request ticket");
    json::array().swap(wire);
    const auto chunk_bytes = std::min<std::size_t>(32768, impl_->config.limits.relay_bytes);
    if (!chunk_bytes) throw Error(500, "invalid_config", "Browser relay byte budget must be positive");
    try {
        browser_detail::GenerationIO io;
        io.encode = [&](std::string_view checkpoint) {
            return json::serialize(encode_request(request, impl_->base_template, std::string_view(snapshot.as_string()), impl_->timezone, checkpoint));
        };
        io.start = [&](std::string_view body, const std::function<void()>& check) {
            check();
            impl_->phase = "request_authorization";
            const auto authorization = impl_->authorization(cancel);
            impl_->phase = "generation_upload";
            const auto upload = impl_->upload(body, cancel);
            check();
            impl_->phase = "generation_start";
            impl_->eval("globalThis.__ais2bridge.start({upload:" + quote_js(upload) + ",authUser:" + quote_js(account.auth_user) +
                            ",authorization:" + quote_js(authorization) + ",chunkBytes:" + std::to_string(chunk_bytes) + "})",
                        cancel);
            impl_->phase = "generation_read";
        };
        io.next = [&](std::uint64_t acknowledgment) {
            return impl_->eval("globalThis.__ais2bridge.next(" + std::to_string(acknowledgment) + ",250)", cancel, 5s);
        };
        io.completed = [&] {
            impl_->phase = "generation_complete";
            if (Clock::now() - impl_->cookies_saved_at >= 300s)
                try { impl_->persist_cookies(account, {}); } catch (...) {}
        };
        browser_detail::run_generation(request, impl_->config.limits, impl_->config.continuation_retry_enabled, io, sink, cancel);
    } catch (...) {
        try { impl_->eval("globalThis.__ais2bridge.abort()", {}, 3s); } catch (...) { impl_->close(); }
        throw;
    }
}
} // namespace ais2api
