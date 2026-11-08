// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/camoufox.hpp"
#include "ais2api/browser.hpp"
#include "ais2api/console.hpp"
#include "ais2api/firefox_identity.hpp"
#include "ais2api/region.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/store.hpp"
#include "../browser/bidi.hpp"
#include "../browser/generation.hpp"
#include "../browser/google_auth.hpp"
#include "../browser/json_text.hpp"
#include "../browser/process.hpp"
#include "../browser/proxy_relay.hpp"
#include <boost/beast/core/detail/base64.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

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

bool browser_detail::camoufox_log_noise(std::string_view line) {
    const auto has = [&](std::string_view text) { return line.find(text) != std::string_view::npos; };
    return (has("juggler/SimpleChannel.js") && has("redeclaration of let SimpleChannel")) ||
           (has("webdriver-bidi/modules/root/browsingContext.sys.mjs") && has("WeakMap key null must be an object"));
}
namespace {
// What Camoufox's Linux fonts.conf holds in place of absolute paths.
constexpr std::string_view fontconfig_fonts = R"(<dir prefix="cwd">fonts</dir>)";
constexpr std::string_view fontconfig_cache = R"(<cachedir prefix="xdg">fontconfig</cachedir>)";
std::string xml_text(std::string_view text) {
    std::string result;
    for (const char c : text) {
        if (c == '&') result += "&amp;";
        else if (c == '<') result += "&lt;";
        else if (c == '>') result += "&gt;";
        else result += c;
    }
    return result;
}
} // namespace
std::string browser_detail::camoufox_fontconfig(std::string_view conf, const std::vector<fs::path>& font_directories,
                                                const fs::path& cache_directory) {
    const auto fonts = conf.find(fontconfig_fonts), cache = conf.find(fontconfig_cache);
    if (font_directories.empty() || fonts == std::string_view::npos || cache == std::string_view::npos ||
        conf.find(fontconfig_fonts, fonts + 1) != std::string_view::npos ||
        conf.find(fontconfig_cache, cache + 1) != std::string_view::npos)
        throw Error(502, "camoufox_layout_changed", "Camoufox's Linux fonts.conf no longer names its fonts and cache as ais2api expects");
    std::string directories;
    for (const auto& directory : font_directories)
        directories += (directories.empty() ? "" : "\n\t") + ("<dir>" + xml_text(path_utf8(directory)) + "</dir>");
    std::string result(conf);
    // The later one first, so the earlier offset still holds.
    const auto replace = [&](std::size_t at, std::string_view old, const std::string& text) { result.replace(at, old.size(), text); };
    const auto cachedir = "<cachedir>" + xml_text(path_utf8(cache_directory)) + "</cachedir>";
    if (cache > fonts) { replace(cache, fontconfig_cache, cachedir); replace(fonts, fontconfig_fonts, directories); }
    else { replace(fonts, fontconfig_fonts, directories); replace(cache, fontconfig_cache, cachedir); }
    return result;
}
std::vector<std::string> browser_detail::xvfb_arguments() {
    return {"-displayfd", "1", "-screen", "0", "1x1x24", "-ac", "-nolisten", "tcp",
            "-extension", "RENDER", "+extension", "GLX", "-extension", "COMPOSITE",
            "-extension", "XVideo", "-extension", "XVideo-MotionCompensation", "-extension", "XINERAMA",
            "-fp", "built-ins", "-nocursor", "-br"};
}
std::optional<unsigned> browser_detail::xvfb_display(std::string_view line) {
    if (line.empty() || line.size() > 5 ||
        !std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    return static_cast<unsigned>(std::stoul(std::string(line)));
}
std::uint64_t browser_detail::identity_salt(std::string_view secret, std::string_view account_id, unsigned seed) {
    std::string input(secret);
    input += '\0';
    input += account_id;
    input += '\0';
    input += std::to_string(seed);
    return std::stoull(sha256(input).substr(0, 16), nullptr, 16);
}

namespace {
using namespace std::chrono_literals;
using browser_detail::Bidi;
using browser_detail::OwnedProcess;
using browser_detail::pause;

[[noreturn]] void incompatible(std::string message) { throw Error(502, "browser_incompatible", std::move(message)); }
#ifndef _WIN32
// The executable `name` in a PATH directory; empty when there is none.
fs::path program_on_path(std::string_view name) {
    const char* value = std::getenv("PATH");
    std::string_view rest = value ? value : "";
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto directory = rest.substr(0, colon);
        rest.remove_prefix(colon == std::string_view::npos ? rest.size() : colon + 1);
        if (directory.empty() || directory.front() != '/') continue;
        const auto candidate = fs::path(std::string(directory)) / std::string(name);
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}
// Xvfb removes its lock and socket when it stops. One killed leaves them,
// and each stale socket pushes the displays after it higher.
void remove_stale_display(unsigned display) {
    const auto number = std::to_string(display);
    const fs::path lock = "/tmp/.X" + number + "-lock";
    long pid = 0;
    try {
        pid = std::stol(read_file(lock, 64));
    } catch (const std::exception&) {
        return;
    }
    if (pid <= 0 || ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) return;
    std::error_code error;
    fs::remove(lock, error);
    fs::remove(fs::path("/tmp/.X11-unix") / ("X" + number), error);
}
#endif
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

// The WebGL vendor and renderer, as Firefox sanitizes them, or null.
constexpr std::string_view read_gpu = R"((() => {
  const gl = document.createElement('canvas').getContext('webgl');
  const info = gl && gl.getExtension('WEBGL_debug_renderer_info');
  return info ? [gl.getParameter(info.UNMASKED_VENDOR_WEBGL), gl.getParameter(info.UNMASKED_RENDERER_WEBGL)] : null;
})())";
// What an identity sets, as a page reads it.
// A family is found as fingerprinting scripts find it: text set in the family,
// with a generic fallback, measures differently from the fallback alone.
constexpr std::string_view read_identity = R"((async (families) => {
  const context = document.createElement('canvas').getContext('2d');
  const measure = (font, text) => { context.font = font; return context.measureText(text).width; };
  const generics = ['monospace', 'serif', 'sans-serif'];
  const probe = 'mmmmmmmmmmlli1WwQ@#\u4e2d\u6587\u3042\ud83d\ude00';
  const base = generics.map((generic) => measure('32px ' + generic, probe));
  const measureAll = () => families.map((family) => generics.map((generic) => measure('32px ' + JSON.stringify(family) + ', ' + generic, probe)));
  // Firefox loads localized family names after a lookup misses one, so what
  // the first pass finds depends on timing; the second finds them loaded.
  measureAll();
  await new Promise((resolve) => setTimeout(resolve, 1000));
  const measured = measureAll();
  const detected = families.filter((family, f) => measured[f].some((width, i) => width !== base[i]));
  const samples = { latin: 'The quick brown fox 0123456789', cjk: '\u4e2d\u6587\u65e5\u672c\u8a9e\ud55c\uad6d\uc5b4',
    emoji: '\ud83d\ude00\ud83d\udc4d\ud83c\udffd\ud83c\uddef\ud83c\uddf5', symbols: '\u2603\u2665\u221e\u2211' };
  const widths = {};
  for (const generic of generics)
    for (const [name, text] of Object.entries(samples)) widths[generic + ':' + name] = measure('32px ' + generic, text);
  const voices = await new Promise((resolve) => {
    if (speechSynthesis.getVoices().length) return resolve(speechSynthesis.getVoices());
    const timer = setTimeout(() => resolve(speechSynthesis.getVoices()), 2000);
    speechSynthesis.addEventListener('voiceschanged', () => { clearTimeout(timer); resolve(speechSynthesis.getVoices()); }, { once: true });
  });
  const gl = document.createElement('canvas').getContext('webgl');
  const info = gl && gl.getExtension('WEBGL_debug_renderer_info');
  return { userAgent: navigator.userAgent, platform: navigator.platform, oscpu: navigator.oscpu,
    hardwareConcurrency: navigator.hardwareConcurrency, maxTouchPoints: navigator.maxTouchPoints,
    language: navigator.language, timezone: Intl.DateTimeFormat().resolvedOptions().timeZone,
    screen: [screen.width, screen.height],
    webgl: info ? [gl.getParameter(info.UNMASKED_VENDOR_WEBGL), gl.getParameter(info.UNMASKED_RENDERER_WEBGL)] : null,
    voices: voices.map((voice) => voice.name),
    fonts: { detected, widths, families: Object.fromEntries(families.map((family, f) => [family, measured[f]])) } };
}))";
// The same readings from a launch configuration.
json::object expected_identity(const json::object& config) {
    const auto value = [&](std::string_view key) { const auto* found = config.if_contains(key); return found ? *found : json::value(nullptr); };
    json::array voices;
    if (const auto* list = config.if_contains("voices"); list && list->is_array())
        for (const auto& voice : list->as_array())
            if (voice.is_object()) voices.emplace_back(string_or(voice.as_object(), "name"));
    return {{"userAgent", value("navigator.userAgent")}, {"platform", value("navigator.platform")}, {"oscpu", value("navigator.oscpu")},
            {"hardwareConcurrency", value("navigator.hardwareConcurrency")}, {"maxTouchPoints", value("navigator.maxTouchPoints")},
            {"language", value("navigator.language")}, {"timezone", value("timezone")},
            {"screen", json::array{value("screen.width"), value("screen.height")}},
            {"webgl", json::array{value("webGl:vendor"), value("webGl:renderer")}}, {"voices", std::move(voices)}};
}
json::value gpu_json(const std::optional<firefox_identity::Gpu>& gpu) {
    if (!gpu) return nullptr;
    return json::object{{"vendor", gpu->vendor}, {"renderer", gpu->renderer}};
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
    // The executable the last launch resolved, and the version the manifest pins.
    CamoufoxExecutable executable;
    std::string pinned_version;
    // Camoufox applies an identity per process, so the running browser is tied
    // to the identity it was launched with. No identity: verify_runtime and
    // the host GPU probe.
    struct LaunchIdentity {
        std::shared_ptr<const firefox_identity::Identity> identity;
        std::string digest;  // of the identity as stored
        unsigned seed = 0;
        std::string timezone, locale;
        bool operator==(const LaunchIdentity& other) const {
            return digest == other.digest && seed == other.seed && timezone == other.timezone && locale == other.locale;
        }
    };
    std::optional<LaunchIdentity> launched;
    // The launch's configuration, and the properties the build did not read.
    json::object launched_config;
    std::vector<std::string> dropped;
    // The last stored identity read, so requests do not reparse it.
    struct StoredIdentity {
        std::string account;
        unsigned seed = 0;
        std::shared_ptr<const firefox_identity::Identity> identity;
        std::string digest;
    };
    std::optional<StoredIdentity> stored_identity;
    // The GPU this machine's Camoufox renders WebGL on without an identity,
    // read once per process for each browser version and display mode.
    struct HostGpu {
        std::string browser;
        bool headless = true;
        std::optional<firefox_identity::Gpu> gpu;
    };
    std::optional<HostGpu> host;
    // Browser output, written on the process reader thread.
    std::mutex output_mutex;
    std::ofstream output_log;
    std::deque<std::string> output_tail;
    unsigned short endpoint_port = 0;
#ifndef _WIN32
    // Xvfb, the display a headless Linux launch runs on. Its display number
    // arrives on the reader thread, under output_mutex.
    OwnedProcess display_process;
    std::optional<unsigned> display_number;
#endif
    // Credentials the current context was built from, for write-back CAS.
    json::object session_state;
    Clock::time_point cookies_saved_at{};
    std::string cookie_refresh = "not_attempted";
    // AIS2API_CAMOUFOX_DEBUG: keep what the page showed when its identity did
    // not load. Local logs only; the screenshot shows the account.
    const bool debug = [] {
#ifdef _WIN32
        wchar_t* value = nullptr;
        std::size_t size = 0;
        const bool set = _wdupenv_s(&value, &size, L"AIS2API_CAMOUFOX_DEBUG") == 0 && value;
        std::free(value);
        return set;
#else
        return std::getenv("AIS2API_CAMOUFOX_DEBUG") != nullptr;
#endif
    }();
    explicit Impl(const Config& c, Store& s) : config(c), store(s) {}
    ~Impl() { close(); }
    void close() noexcept {
        authorization_cache.clear();
        if (bidi) bidi->close();
        launched.reset(); launched_config = {};
        bidi.reset(); process.stop();
        stop_display();
        if (relay) { relay_error = relay->last_error(); relay.reset(); }
        { std::lock_guard lock(output_mutex); output_log.close(); }
        user_context.clear(); page.clear(); account_id.clear(); account_fingerprint.clear(); base_template.clear(); models_discovered = false; session_state = {};
        if (!root.empty()) { std::error_code ec; fs::remove_all(root, ec); root.clear(); }
        last_status = "closed";
    }
    // Resolved again for each launch: setup may have installed or rolled back
    // Camoufox meanwhile.
    void check_executable() {
        auto found = camoufox_executable(config);
        if (found.path.empty())
            throw Error(503, "browser_not_configured", "Camoufox is not installed: run ais2api setup");
        std::error_code error;
        if (!fs::is_regular_file(found.path, error))
            throw Error(503, "browser_not_configured", "browser_executable names a Camoufox executable that does not exist");
        executable = std::move(found);
        try {
            const auto pin = camoufox_pin();
            pinned_version = pin ? pin->version : "";
        } catch (const std::exception&) {
            pinned_version.clear();
        }
    }
    fs::path browser_directory() const { return executable.path.parent_path(); }
#ifndef _WIN32
    // Starts Xvfb below the launch's directory and returns its DISPLAY.
    std::string start_display(std::stop_token token) {
        const auto xvfb = program_on_path("Xvfb");
        if (xvfb.empty())
            throw Error(503, "display_unavailable", "Xvfb is not installed: run ais2api setup --yes, or turn headless off on a desktop");
        browser_detail::ProcessOptions options;
        // Mesa's software OpenGL, as Camoufox's launcher runs Xvfb.
        options.environment = {{"__GLX_VENDOR_LIBRARY_NAME", "mesa"}, {"LIBGL_ALWAYS_SOFTWARE", "1"}};
        options.on_output = [this](std::string_view line) {
            std::lock_guard lock(output_mutex);
            if (!display_number)
                if (const auto display = browser_detail::xvfb_display(line)) { display_number = display; return; }
            const auto text = "Xvfb: " + std::string(line.substr(0, 1024));
            if (output_log.is_open()) { output_log << text << '\n'; output_log.flush(); }
            output_tail.push_back(text);
            if (output_tail.size() > 20) output_tail.pop_front();
        };
        display_process.start(xvfb, browser_detail::xvfb_arguments(), root / "display", options);
        // A cold first Xvfb on a fresh host took seconds to report.
        const auto deadline = Clock::now() + 30s;
        for (;;) {
            check_cancel(token);
            {
                std::lock_guard lock(output_mutex);
                if (display_number) return ":" + std::to_string(*display_number);
            }
            if (!display_process.alive()) throw Error(503, "display_unavailable", "Xvfb exited before it reported a display" + output_hint());
            if (Clock::now() >= deadline) throw Error(504, "browser_timeout", "Xvfb did not report a display" + output_hint());
            pause(token, 50ms);
        }
    }
    // FONTCONFIG_FILE for the install: its Linux fonts.conf, scanning the
    // font groups a Linux identity reads, written once for each content below
    // runtime/camoufox-fontconfig. Empty for a build without one.
    fs::path fontconfig_file() const {
        const auto directory = fs::absolute(browser_directory());
        const auto source = directory / "fontconfig" / "linux" / "fonts.conf";
        std::error_code error;
        if (!fs::is_regular_file(source, error)) return {};
        std::vector<fs::path> fonts;
        try {
            const auto groups = json::parse(read_file(directory / "fonts" / "groups.json", 1 << 20)).as_object();
            for (const auto& value : groups.at("readBy").as_object().at("lin").as_array()) {
                const std::string group(value.as_string());
                if (group.empty() || !std::all_of(group.begin(), group.end(), [](unsigned char c) { return std::isalnum(c); })) continue;
                if (fs::is_directory(directory / "fonts" / group, error)) fonts.push_back(directory / "fonts" / group);
            }
        } catch (const std::exception&) {
            fonts.clear();
        }
        // Older builds keep every font in fonts/ itself.
        if (fonts.empty()) fonts.push_back(directory / "fonts");
        const auto written = fs::absolute(config.data_dir / "runtime" / "camoufox-fontconfig");
        secure_directory(written);
        secure_directory(written / "cache");
        const auto text = browser_detail::camoufox_fontconfig(read_file(source, 16 << 20), fonts, written / "cache");
        const auto file = written / ("fonts-" + sha256(text).substr(0, 12) + ".conf");
        if (!fs::is_regular_file(file, error)) atomic_write(file, text);
        return file;
    }
#endif
    void stop_display() noexcept {
#ifndef _WIN32
        display_process.stop();
        std::optional<unsigned> display;
        {
            std::lock_guard lock(output_mutex);
            display = display_number;
            display_number.reset();
        }
        if (display) remove_stale_display(*display);
#endif
    }
    // Opens a page in a new user context on a local document.
    void open_local_page(std::string_view html, bool bridge, std::stop_token token) {
        user_context = string_or(bidi->call("browser.createUserContext", {}, token), "userContext");
        if (user_context.empty()) incompatible("Browser did not create a verification context");
        page = string_or(bidi->call("browsingContext.create", {{"type", "tab"}, {"userContext", user_context}}, token), "context");
        if (page.empty()) incompatible("Browser did not create a verification page");
        if (bridge) install_bridge(token);
        navigate_local(html, token);
    }
    void navigate_local(std::string_view html, std::stop_token token) {
        bidi->call("browsingContext.navigate", {{"context", page}, {"url", "data:text/html," + std::string(html)}, {"wait", "complete"}}, token, 30s, page);
    }
    // Launches the browser without an identity, under the display mode and
    // preferences an identity launches with, as the Python launcher's
    // host_gpu does.
    std::optional<firefox_identity::Gpu> host_gpu(std::stop_token token) {
        check_executable();
        const auto version = firefox_identity::browser_version(browser_directory());
        if (host && host->browser == version && host->headless == config.headless) return host->gpu;
        ensure_browser(token, {});
        try {
            open_local_page("<title>ais2api</title>", false, token);
            const auto value = eval(std::string(read_gpu), token);
            dispose_context(token);
            close();
            std::optional<firefox_identity::Gpu> gpu;
            if (value.is_array() && value.as_array().size() == 2 && value.as_array()[0].is_string() && value.as_array()[1].is_string())
                gpu = firefox_identity::Gpu{std::string(value.as_array()[0].as_string()), std::string(value.as_array()[1].as_string())};
            host = HostGpu{version, config.headless, gpu};
            return gpu;
        } catch (...) { close(); throw; }
    }
    json::object identity_summary(const firefox_identity::Identity& identity) const {
        json::array properties;
        for (const auto& name : dropped) properties.emplace_back(name);
        // An identity generated on another GPU than this machine renders on
        // now (a new graphics card, or a data directory moved here) stays
        // until the account's fingerprint is regenerated.
        const auto host_gpu = !host ? "not_checked" : host->gpu == identity.host_gpu ? "unchanged" : "changed";
        return {{"platform", firefox_identity::os_name(identity.os)}, {"userAgent", user_agent},
                {"gpu", json::object{{"vendor", string_or(identity.config, "webGl:vendor")}, {"renderer", string_or(identity.config, "webGl:renderer")}}},
                {"claimsHostGpu", identity.claims_host_gpu}, {"canvasCovered", identity.canvas_covered}, {"hostGpu", host_gpu},
                {"model", identity.model}, {"generatedFor", identity.browser}, {"catalog", identity.catalog.substr(0, 12)},
                {"droppedProperties", std::move(properties)}};
    }
    // The account's identity: the stored one, or one generated for it now and
    // stored with its seed. An account being imported is not stored yet; its
    // identity is generated from the seed the store will assign it, so the
    // account's first launch draws the same one.
    LaunchIdentity identity_for(const Account& account, std::stop_token token) {
        BrowserFingerprint fingerprint;
        bool stored = true;
        try {
            fingerprint = store.fingerprint(account.id);
        } catch (const Error& error) {
            if (error.code != "account_not_found") throw;
            fingerprint.seed = default_fingerprint_seed(account.id);
            stored = false;
        }
        LaunchIdentity result{nullptr, {}, fingerprint.seed, fingerprint.timezone.empty() ? host_timezone() : fingerprint.timezone,
                              fingerprint.locale.empty() ? host_locale() : fingerprint.locale};
        if (stored && stored_identity && stored_identity->account == account.id && stored_identity->seed == fingerprint.seed) {
            result.identity = stored_identity->identity;
            result.digest = stored_identity->digest;
            return result;
        }
        auto saved = stored ? store.firefox_identity(account.id) : std::nullopt;
        if (!saved) {
            check_executable();
            FpgenModelPin pin;
            try {
                pin = fpgen_model_pin();
            } catch (const Error& error) {
                throw Error(503, "browser_not_configured", std::string("The fpgen model is not pinned: ") + error.what());
            }
            // Before any launch, so a missing model fails fast.
            const auto model = load_fpgen_model(config.data_dir, pin);
            auto machine = firefox_identity::local_host();
            machine.gpu = host_gpu(token);
            const auto generated = firefox_identity::generate(model, pin.tag, machine,
                browser_detail::identity_salt(store.identity_secret(), account.id, fingerprint.seed),
                firefox_identity::browser_version(browser_directory()));
            saved = firefox_identity::to_json(generated);
            (*saved)["seed"] = fingerprint.seed;
            if (stored) store.save_firefox_identity(account.id, *saved);
            console::emit_record({{"event", "camoufox_identity"}, {"account_id", account.id}, {"stored", stored},
                                  {"claims_host_gpu", generated.claims_host_gpu}, {"canvas_covered", generated.canvas_covered}});
        }
        auto identity = std::make_shared<const firefox_identity::Identity>(firefox_identity::from_json(*saved));
        result.identity = identity;
        result.digest = sha256(json::serialize(*saved));
        if (stored) stored_identity = StoredIdentity{account.id, fingerprint.seed, identity, result.digest};
        return result;
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
        check_executable();
        try {
            json::object launch_config, prefs;
            user_agent.clear();
            dropped.clear();
            if (identity.identity) {
                auto launch = firefox_identity::launch(*identity.identity, identity.locale, identity.timezone,
                                                       firefox_identity::browser_version(browser_directory()),
                                                       firefox_identity::load_properties(browser_directory()));
                launch_config = std::move(launch.config);
                prefs = std::move(launch.prefs);
                dropped = std::move(launch.dropped);
                if (!dropped.empty()) {
                    json::array names;
                    for (const auto& name : dropped) names.emplace_back(name);
                    console::emit_record({{"event", "camoufox_properties_dropped"}, {"properties", std::move(names)}});
                }
                user_agent = string_or(launch_config, "navigator.userAgent");
            } else {
                // The bridge runs in the page's own world, as AI Studio's code does.
                launch_config["allowMainWorld"] = true;
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
            options.environment = browser_detail::camoufox_environment(json::serialize(launch_config));
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
                if (browser_detail::camoufox_log_noise(line)) return;
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
#ifdef _WIN32
            if (config.headless) args.emplace_back("-headless");
#else
            // Linux runs Camoufox with a display, as a desktop does: Xvfb's
            // when headless, else the session's.
            if (config.headless) {
                options.environment.emplace_back("DISPLAY", start_display(token));
                // GTK would otherwise prefer a Wayland session's display.
                options.unset_prefixes.emplace_back("WAYLAND_DISPLAY");
                options.environment.emplace_back("MOZ_ENABLE_WAYLAND", "0");
            } else if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
                throw Error(503, "display_unavailable", "headless is off and there is no display: set DISPLAY, or turn headless on to use Xvfb");
            if (const auto fontconfig = fontconfig_file(); !fontconfig.empty())
                options.environment.emplace_back("FONTCONFIG_FILE", path_utf8(fontconfig));
#endif
            args.emplace_back("about:blank");
            process.start(executable.path, args, root, options);
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
            launched_config = std::move(launch_config);
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
        browser_detail::check_identity(object, allow_login);
        return object;
    }
    void await_account_identity(const Account& account, std::stop_token token, bool require_editor) {
        browser_detail::await_account_identity({
            [&](std::chrono::seconds timeout) {
                const auto exists = eval("!!globalThis.__ais2bridge", token, timeout);
                return exists.is_bool() && exists.as_bool();
            },
            [&](std::chrono::seconds timeout) { return identity(token, true, timeout); },
            [&] { pause(token, 250ms); },
            [&] { record_page("identity"); },
        }, account.email, require_editor, token);
    }
    // Debug only: the page state and a screenshot as logs/<profile>-<what>.*.
    void record_page(std::string_view what) noexcept {
        if (!debug || page.empty() || !bidi) return;
        const auto stem = config.data_dir / "logs" / (path_utf8(root.filename()) + "-" + std::string(what));
        try {
            const auto state = eval(R"((() => {
  const links = [...document.querySelectorAll('a[href*="accounts.google.com"], a[href*="myaccount.google.com"]')];
  const labelled = [...document.querySelectorAll('[data-email], [aria-label*="@"], img[alt*="@"]')];
  const bridge = globalThis.__ais2bridge;
  let identity = null;
  try { if (bridge) { const i = bridge.identity(); identity = { login: i.login, ready: i.ready, identityReady: i.identityReady, ambiguous: i.identityAmbiguous, email: !!i.email }; } } catch (error) { identity = String(error); }
  return { page: location.origin + location.pathname, title: document.title, readyState: document.readyState,
    visibility: document.visibilityState, bridge: !!bridge, identity, accountLinks: links.length,
    accountLinksWithEmail: links.filter(a => /@/.test((a.getAttribute('aria-label') || '') + (a.getAttribute('data-email') || ''))).length,
    labelled: labelled.length, labelledVisible: labelled.filter(el => el.getClientRects().length > 0).length,
    textarea: !!document.querySelector('textarea'), dialogs: document.querySelectorAll('[role="dialog"], mat-dialog-container').length,
    environment: { userAgent: navigator.userAgent, platform: navigator.platform, webdriver: navigator.webdriver,
      language: navigator.language, languages: navigator.languages, timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
      intlLocale: Intl.DateTimeFormat().resolvedOptions().locale, offset: new Date().getTimezoneOffset(), hasFocus: document.hasFocus(),
      cores: navigator.hardwareConcurrency, screen: [screen.width, screen.height, devicePixelRatio], window: [innerWidth, innerHeight],
      voices: speechSynthesis.getVoices().length,
      byob: (() => { try { new Response('x').body.getReader({ mode: 'byob' }).releaseLock(); return true; } catch (error) { return false; } })() },
    text: (document.body ? document.body.innerText : '').slice(0, 600) };
})())", {}, 5s);
            std::ofstream(stem.string() + ".json", std::ios::binary) << json::serialize(state);
        } catch (...) {}
        try {
            const auto shot = bidi->call("browsingContext.captureScreenshot", {{"context", page}}, {}, 10s, page);
            const auto data = string_or(shot, "data");
            std::string png(boost::beast::detail::base64::decoded_size(data.size()), '\0');
            png.resize(boost::beast::detail::base64::decode(png.data(), data.data(), data.size()).first);
            std::ofstream(stem.string() + ".png", std::ios::binary) << png;
        } catch (...) {}
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
        ensure_browser(token, identity_for(account, token));
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
                record_page("account");
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
        impl_->ensure_browser(cancel, {});
        impl_->open_local_page("<title>ais2api</title><textarea autofocus></textarea>", true, cancel);
        auto page = impl_->page_checks(cancel);
        impl_->dispose_context(cancel);
        json::object result{{"product", impl_->browser_name + "/" + impl_->browser_version}, {"protocol_version", "webdriver-bidi"},
                            {"isolated_context", true}, {"google_verified", false}, {"page", std::move(page)}};
        impl_->close();
        return result;
    } catch (...) { impl_->close(); throw; }
}
json::object CamoufoxGateway::verify_identity(std::string_view account_id, std::stop_token cancel,
                                              const std::vector<std::string>& probe_fonts) {
    std::lock_guard lock(impl_->mutex);
    try {
        Account account;
        account.id = std::string(account_id);
        auto identity = impl_->identity_for(account, cancel);
        // The host's GPU now, which tells whether the identity was generated
        // on another.
        const auto host = impl_->host_gpu(cancel);
        impl_->ensure_browser(cancel, identity);
        impl_->open_local_page("<title>ais2api</title>", false, cancel);
        // Camoufox registers the identity's voices for the documents after the
        // first one that asks for them.
        impl_->eval("speechSynthesis.getVoices().length", cancel);
        impl_->navigate_local("<title>ais2api identity</title>", cancel);
        json::array claimed;
        if (const auto* fonts = impl_->launched_config.if_contains("fonts"); fonts && fonts->is_array()) claimed = fonts->as_array();
        auto families = claimed;
        for (const auto& font : probe_fonts) families.emplace_back(font);
        auto page = impl_->eval(std::string(read_identity) + "(" + json::serialize(families) + ")", cancel);
        impl_->dispose_context(cancel);
        json::object result{{"product", impl_->browser_name + "/" + impl_->browser_version}, {"google_verified", false},
                            {"identity", impl_->identity_summary(*identity.identity)}, {"hostGpu", gpu_json(host)},
                            {"expected", expected_identity(impl_->launched_config)}, {"claimedFonts", std::move(claimed)},
                            {"page", std::move(page)}};
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
    if (launched) {
        fingerprint = json::object{{"seed", launched->seed}, {"timezone", launched->timezone}, {"locale", launched->locale}};
        browser = json::object{{"name", impl_->browser_name}, {"version", impl_->browser_version},
                               {"source", impl_->executable.source}, {"pinned", impl_->pinned_version}};
        if (launched->identity) {
            fingerprint.as_object()["platform"] = firefox_identity::os_name(launched->identity->os);
            identity = impl_->identity_summary(*launched->identity);
        }
    }
    return {{"status", impl_->last_status}, {"phase", impl_->phase}, {"backend", "camoufox"},
        {"bridgeSource", "embedded"}, {"bridgeSha256", browser_detail::embedded_bridge_sha256()},
        {"running", impl_->process.alive()}, {"isolatedContext", !impl_->user_context.empty()}, {"templateCaptured", !impl_->base_template.empty()}, {"modelDiscovery", impl_->models_discovered ? "live_rpc" : "not_loaded"}, {"chunkBytes", 32768},
        {"cookieRefresh", impl_->cookie_refresh},
        {"proxy", json::object{{"mode", impl_->config.proxy.empty() ? "none" : impl_->config.browser_proxy_relay ? "relay" : "direct"},
                               {"relayError", impl_->relay ? impl_->relay->last_error() : impl_->relay_error}}},
        {"fingerprint", std::move(fingerprint)}, {"browser", std::move(browser)}, {"identity", std::move(identity)}};
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
