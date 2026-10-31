// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/browser.hpp"
#include "ais2api/region.hpp"
#include "../browser/cdp.hpp"
#include "../browser/proxy_relay.hpp"
#include "../browser/google_auth.hpp"
#include "../browser/rpc_error.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cctype>
#include <cwchar>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
extern char **environ;
#endif

namespace ais2api {
browser_detail::IdentityVerdict browser_detail::identity_verdict(const json::object& identity, std::string_view expected_email) {
    if (bool_or(identity, "identityAmbiguous")) return IdentityVerdict::ambiguous;
    auto normalized = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const auto email = normalized(string_or(identity, "email"));
    if (!bool_or(identity, "identityReady") || email.empty()) return IdentityVerdict::pending;
    const auto expected = normalized(std::string(expected_email));
    return expected.empty() || expected == email ? IdentityVerdict::verified : IdentityVerdict::mismatch;
}
json::array browser_detail::import_cookies(const json::array& cookies, std::size_t* skipped_partition_cookies) {
    json::array result;
    if (skipped_partition_cookies) *skipped_partition_cookies = 0;
    for (const auto& value : cookies) {
        if (!value.is_object()) continue;
        const auto& source = value.as_object();
        if (source.contains("partitionKey") || bool_or(source, "partitionKeyOpaque")) {
            if (skipped_partition_cookies) ++*skipped_partition_cookies;
            continue;
        }
        auto domain = string_or(source, "domain");
        std::transform(domain.begin(), domain.end(), domain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        if (domain != "google.com" && !domain.ends_with(".google.com")) continue;
        const auto name = source.if_contains("name"), content = source.if_contains("value");
        if (!name || !name->is_string() || name->as_string().empty() || !content || !content->is_string()) continue;
        json::object cookie{{"name", *name}, {"value", *content}, {"domain", source.at("domain")}};
        if (auto p = source.if_contains("path"); p && p->is_string()) cookie["path"] = *p;
        for (auto key : {"secure", "httpOnly"}) if (auto p = source.if_contains(key); p && p->is_bool()) cookie[key] = *p;
        if (auto p = source.if_contains("sameSite"); p && p->is_string() && (p->as_string() == "Strict" || p->as_string() == "Lax" || p->as_string() == "None")) cookie["sameSite"] = *p;
        if (auto p = source.if_contains("expires"); p && p->is_number() && json::value_to<double>(*p) > 0) cookie["expires"] = *p;
        result.push_back(std::move(cookie));
    }
    return result;
}
std::optional<Error> browser_detail::navigation_failure(std::string_view text, bool proxy_configured) {
    if (text.empty()) return std::nullopt;
    const auto has = [&](std::string_view part) { return text.find(part) != std::string_view::npos; };
    // ERR_PROXY_*, ERR_NO_SUPPORTED_PROXIES, ERR_TUNNEL_CONNECTION_FAILED,
    // ERR_SOCKS_*, and a proxy's 407 that Chromium could not answer.
    if (has("PROXY") || has("PROXIES") || has("TUNNEL") || has("SOCKS") ||
        (proxy_configured && text == "net::ERR_INVALID_AUTH_CREDENTIALS"))
        return Error(502, "proxy_failed", "The browser could not reach Google through the configured proxy (" + std::string(text) + ")");
    static constexpr std::string_view unreachable[] = {
        "net::ERR_NAME_NOT_RESOLVED", "net::ERR_NAME_RESOLUTION_FAILED", "net::ERR_INTERNET_DISCONNECTED",
        "net::ERR_ADDRESS_UNREACHABLE", "net::ERR_CONNECTION_REFUSED", "net::ERR_CONNECTION_TIMED_OUT",
        "net::ERR_CONNECTION_RESET", "net::ERR_CONNECTION_CLOSED", "net::ERR_CONNECTION_FAILED",
        "net::ERR_TIMED_OUT", "net::ERR_NETWORK_CHANGED"};
    if (std::find(std::begin(unreachable), std::end(unreachable), text) != std::end(unreachable) ||
        text.starts_with("net::ERR_CERT_") || text.starts_with("net::ERR_SSL_"))
        return Error(502, "network_failed", "The browser could not reach Google (" + std::string(text) + ")");
    return std::nullopt;
}
json::array browser_detail::injected_cookies(const json::array& stored) {
    // Short-lived security and origin-bound cookies from another browser
    // conflict with the fresh session; the page negotiates new ones.
    static constexpr std::array<std::string_view, 5> conflicting{
        "OSID", "__Secure-OSID", "SIDCC", "__Secure-1PSIDCC", "__Secure-3PSIDCC"};
    json::array all, kept;
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& original = value.as_object();
        json::object cookie;
        for (auto key : {"name", "value", "domain", "path", "secure", "httpOnly", "sameSite", "partitionKey"}) if (auto p = original.if_contains(key)) cookie[key] = *p;
        if (auto expires = original.if_contains("expires"); expires && expires->is_number() && json::value_to<double>(*expires) > 0) cookie["expires"] = *expires;
        const auto name = string_or(cookie, "name");
        const bool conflict = std::find(conflicting.begin(), conflicting.end(), name) != conflicting.end();
        if (!conflict) kept.push_back(cookie);
        all.push_back(std::move(cookie));
    }
    return kept.empty() ? all : kept;
}
json::array browser_detail::refreshed_cookies(const json::array& stored, const json::array& browser) {
    auto result = import_cookies(browser);
    // Never replace credentials with a signed-out or partial export.
    auto has = [&](std::initializer_list<std::string_view> names) {
        return std::any_of(result.begin(), result.end(), [&](const json::value& value) {
            const auto name = string_or(value.as_object(), "name");
            return std::find(names.begin(), names.end(), name) != names.end();
        });
    };
    if (!has({"SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID"}) || !has({"SID", "__Secure-1PSID", "__Secure-3PSID"}))
        return {};
    for (const auto& value : stored) {
        if (!value.is_object()) continue;
        const auto& cookie = value.as_object();
        auto domain = string_or(cookie, "domain");
        std::transform(domain.begin(), domain.end(), domain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
        const bool google = domain == "google.com" || domain.ends_with(".google.com");
        // import_cookies exports neither other domains nor partitioned cookies.
        if (!google || cookie.contains("partitionKey") || bool_or(cookie, "partitionKeyOpaque"))
            result.push_back(cookie);
    }
    return result;
}
std::vector<std::string> browser_detail::launch_switches(const Config& config, bool no_zygote) {
    // CloakBrowser in aarch64 containers needs the desktop runtime's launch
    // profile: without it the zygote and GPU children can exit while CDP is
    // connected, which the gateway reports as browser_disconnected.
    std::vector<std::string> args{
        "--in-process-gpu", "--use-gl=angle", "--use-angle=swiftshader", "--disable-setuid-sandbox",
        "--disable-breakpad", "--mute-audio", "--disable-extensions",
        "--disable-component-extensions-with-background-pages", "--disable-backgrounding-occluded-windows",
        "--disable-ipc-flooding-protection", "--disable-sync", "--disable-speech-api",
        "--disable-component-update", "--disable-default-apps"};
    if (no_zygote) {
        // Child processes enter the sandbox through the zygote, so dropping it
        // also needs the sandbox off.
        args.emplace_back("--no-zygote");
        args.emplace_back("--no-sandbox");
    }
    if (config.headless) {
        // Low-memory rendering only where nobody watches the window; a visible
        // browser is used to troubleshoot sign-in and keeps normal rendering.
        for (const auto* argument : {
                 "--headless=new", "--hide-scrollbars", "--window-size=1280,800",
                 "--disable-gpu-compositing", "--js-flags=--max-old-space-size=128 --optimize-for-size",
                 "--media-cache-size=1", "--enable-low-end-device-mode", "--aggressive-cache-discard",
                 "--disable-smooth-scrolling", "--disable-threaded-animation", "--disable-threaded-scrolling",
                 "--disable-backing-store-limit"})
            args.emplace_back(argument);
    }
    if (config.browser_remote_allow_origins) args.emplace_back("--remote-allow-origins=*");
    // Chromium honours only the last --disable-features, so keep a single one.
    std::string features = "Translate,OptimizationHints,MediaRouter,DialLocalDiscovery,PreloadMediaEngagementData,"
        "CertificateTransparencyComponentUpdater,AudioServiceOutOfProcess,CalculateNativeWinOcclusion,"
        "InterestFeedContentSuggestions,BackForwardCache";
    if (!config.browser_site_isolation) {
        args.emplace_back("--disable-site-isolation-trials");
        features += ",SitePerProcess";
    }
    args.push_back("--disable-features=" + features);
    return args;
}
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
#include "browser_bridge.inc"

[[noreturn]] void incompatible(std::string message) { throw Error(502, "browser_incompatible", std::move(message)); }
std::string quote_js(std::string_view s) {
    std::string quoted;
    browser_detail::append_quoted(quoted, s);
    return quoted;
}
std::string decode64(std::string_view encoded) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (encoded.size() > 4 * ((32768 + 2) / 3) || encoded.size() % 4) incompatible("Invalid browser relay chunk");
    std::string bytes;
    bytes.reserve(encoded.size() * 3 / 4);
    unsigned bits = 0, count = 0;
    bool padding = false;
    for (char c : encoded) {
        if (c == '=') { padding = true; continue; }
        const auto n = alphabet.find(c);
        if (padding || n == std::string_view::npos) incompatible("Invalid base64 browser chunk");
        bits = (bits << 6) | static_cast<unsigned>(n); count += 6;
        if (count >= 8) { count -= 8; bytes.push_back(static_cast<char>((bits >> count) & 255)); }
    }
    if (bytes.size() > 32768) incompatible("Browser exceeded byte credit");
    return bytes;
}
// Each launch writes its own browser-*.log; keep the most recent ones only.
void prune_browser_logs(const fs::path& directory, std::size_t keep = 8) {
    std::error_code error;
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = path_utf8(it->path().filename());
        if (!name.starts_with("browser-") || !name.ends_with(".log") || !it->is_regular_file(error)) continue;
        logs.emplace_back(it->last_write_time(error), it->path());
    }
    if (logs.size() <= keep) return;
    std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = keep; i < logs.size(); ++i) fs::remove(logs[i].second, error);
}
std::string bridge_source() {
    return std::string(browser_detail::embedded_bridge_source());
}

class OwnedProcess {
public:
    ~OwnedProcess() { stop(); }
    void start(const fs::path& executable, const std::vector<std::string>& args, const fs::path& private_root) {
        stop();
        // Build a child-only environment before fork/CreateProcess. Never mutate
        // this multithreaded service's process-wide environment.
        const auto home = fs::absolute(private_root / "home");
        const auto cache = fs::absolute(private_root / "cache");
        const auto settings = fs::absolute(private_root / "settings");
        const auto temporary = fs::absolute(private_root / "tmp");
        const auto data = fs::absolute(private_root / "files");
        const auto state = fs::absolute(private_root / "state");
        for (const auto& directory : {home, cache, settings, temporary, data, state}) secure_directory(directory);
        const auto prefix = std::getenv("PREFIX");
        const bool termux = std::getenv("TERMUX_VERSION") || (prefix && std::string_view(prefix).find("com.termux") != std::string_view::npos);
#ifdef _WIN32
        auto wide = [](const std::string& s) {
            if (s.empty()) return std::wstring{};
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (!n) throw Error(500, "browser_launch", "Invalid UTF-8 launch argument");
            std::wstring result(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
            return result;
        };
        auto quote = [](const std::wstring& value) {
            std::wstring result = L"\"";
            unsigned slashes = 0;
            for (wchar_t c : value) {
                if (c == L'\\') { ++slashes; continue; }
                if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
                else { result.append(slashes, L'\\'); result += c; }
                slashes = 0;
            }
            result.append(slashes * 2, L'\\'); result += L'"'; return result;
        };
        std::wstring command = quote(executable.wstring());
        for (const auto& arg : args) command += L" " + quote(wide(arg));
        auto environment_less = [](const std::wstring& a, const std::wstring& b) {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        };
        std::map<std::wstring, std::wstring, decltype(environment_less)> inherited(environment_less);
        auto raw_environment = GetEnvironmentStringsW();
        if (!raw_environment) throw Error(500, "browser_launch", "Cannot read browser environment");
        for (auto entry = raw_environment; *entry; entry += std::wcslen(entry) + 1) {
            std::wstring value(entry);
            // Hidden drive-current-directory variables begin with '=' (e.g. =C:).
            const auto separator = value.find(L'=', value.front() == L'=' ? 1 : 0);
            if (separator != std::wstring::npos) inherited[value.substr(0, separator)] = value.substr(separator + 1);
        }
        FreeEnvironmentStringsW(raw_environment);
        inherited[L"LOCALAPPDATA"] = cache.wstring();
        inherited[L"APPDATA"] = settings.wstring();
        inherited[L"TEMP"] = temporary.wstring();
        inherited[L"TMP"] = temporary.wstring();
        if (!termux) {
            inherited[L"USERPROFILE"] = home.wstring();
            inherited[L"HOME"] = home.wstring();
            inherited[L"HOMEDRIVE"] = home.root_name().wstring();
            inherited[L"HOMEPATH"] = home.relative_path().empty() ? L"\\" : L"\\" + home.relative_path().wstring();
        }
        std::vector<wchar_t> environment;
        for (const auto& [key, value] : inherited) {
            environment.insert(environment.end(), key.begin(), key.end()); environment.push_back(L'=');
            environment.insert(environment.end(), value.begin(), value.end()); environment.push_back(L'\0');
        }
        environment.push_back(L'\0');
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) throw Error(500, "browser_launch", "CreateJobObject failed");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { stop(); throw Error(500, "browser_launch", "Cannot configure browser Job Object"); }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &pi)) { stop(); throw Error(500, "browser_launch", "CreateProcessW failed"); }
        process_ = pi.hProcess;
        if (!AssignProcessToJobObject(job_, process_)) { TerminateProcess(process_, 1); CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot assign browser to Job Object"); }
        if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) { CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot resume browser process"); }
        CloseHandle(pi.hThread);
#else
        std::vector<std::string> values{executable.string()};
        values.insert(values.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& value : values) argv.push_back(value.data());
        argv.push_back(nullptr);
        std::map<std::string, std::string> inherited;
        for (auto entry = ::environ; entry && *entry; ++entry) {
            std::string value(*entry);
            auto separator = value.find('=');
            if (separator != std::string::npos) inherited[value.substr(0, separator)] = value.substr(separator + 1);
        }
        inherited["XDG_CONFIG_HOME"] = settings.string();
        inherited["XDG_CACHE_HOME"] = cache.string();
        inherited["XDG_DATA_HOME"] = data.string();
        inherited["XDG_STATE_HOME"] = state.string();
        inherited["TMPDIR"] = temporary.string();
#if defined(__linux__) && !defined(__ANDROID__)
        if (!termux) {
            // Chromium binds AF_UNIX singleton sockets below TMPDIR. Their
            // 108-byte address limit otherwise breaks portable installs in
            // deeply nested directories. Keep the actual files in this private
            // profile; a held parent directory fd provides a short kernel alias.
            temporary_directory_fd_ = open(temporary.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (temporary_directory_fd_ < 0) throw Error(500, "browser_launch", "Cannot open private browser temporary directory");
            const auto alias = "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(temporary_directory_fd_);
            if (!fs::is_directory(alias)) throw Error(500, "browser_launch", "Cannot access private browser temporary directory through /proc");
            inherited["TMPDIR"] = alias;
        }
#endif
        if (!termux) inherited["HOME"] = home.string();
        std::vector<std::string> environment;
        for (const auto& [key, value] : inherited) environment.push_back(key + "=" + value);
        std::vector<char*> envp;
        for (auto& value : environment) envp.push_back(value.data());
        envp.push_back(nullptr);
        pid_ = fork();
        if (pid_ < 0) throw Error(500, "browser_launch", "fork failed");
        if (pid_ == 0) {
            if (setsid() < 0) _exit(126);
            int nullfd = open("/dev/null", O_RDWR);
            if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); dup2(nullfd, STDOUT_FILENO); dup2(nullfd, STDERR_FILENO); if (nullfd > 2) ::close(nullfd); }
            execve(argv[0], argv.data(), envp.data()); _exit(127);
        }
#endif
    }
    bool alive() {
#ifdef _WIN32
        return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
        if (pid_ <= 0) return false;
        siginfo_t info{};
        // Leave an exited leader unreaped until stop(), preventing PID/group
        // reuse while descendants are still owned by this gateway.
        return waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
#endif
    }
    void stop() noexcept {
#ifdef _WIN32
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        if (process_) { WaitForSingleObject(process_, 5000); CloseHandle(process_); process_ = nullptr; }
#else
        if (pid_ > 0) {
            kill(-pid_, SIGTERM);
            kill(pid_, SIGTERM);
            for (unsigned i = 0; i < 30; ++i) { siginfo_t info{}; if (waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid) break; std::this_thread::sleep_for(20ms); }
            kill(-pid_, SIGKILL);
            int status = 0; while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
        if (temporary_directory_fd_ >= 0) { ::close(temporary_directory_fd_); temporary_directory_fd_ = -1; }
#endif
    }
private:
#ifdef _WIN32
    HANDLE process_ = nullptr, job_ = nullptr;
#else
    pid_t pid_ = -1;
    int temporary_directory_fd_ = -1;
#endif
};

using Cdp = browser_detail::Cdp;

// The DevToolsActivePort file belongs to our fresh private profile. No global
// process or existing listening browser is adopted or killed.
std::pair<unsigned short, std::string> read_endpoint(const fs::path& profile) {
    std::ifstream input(profile / "DevToolsActivePort");
    std::string port, target;
    if (!std::getline(input, port) || !std::getline(input, target)) return {};
    if (port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); })) return {};
    try {
        const auto number = std::stoul(port);
        if (!number || number > 65535 || !target.starts_with("/devtools/browser/")) return {};
        return {static_cast<unsigned short>(number), target};
    } catch (...) { return {}; }
}
void pause(std::stop_token token, std::chrono::milliseconds duration) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) { check_cancel(token); std::this_thread::sleep_for(20ms); }
}
std::string content_hash(const json::array& wire) {
    // Hashed in place: the parts can hold megabytes of inline data.
    Sha256 joined;
    bool first = true;
    if (wire.size() > 1 && wire[1].is_array()) for (const auto& content : wire[1].as_array()) {
        if (!content.is_array() || content.as_array().empty() || !content.as_array()[0].is_array()) continue;
        for (const auto& part : content.as_array()[0].as_array()) {
            if (!first) joined.update(" ");
            first = false;
            if (!part.is_array()) continue;
            const auto& p = part.as_array();
            if (p.size() > 1 && p[1].is_string()) joined.update(p[1].as_string());
            else if (p.size() > 2 && p[2].is_array()) {
                const auto& data = p[2].as_array();
                if (data.size() > 1 && data[1].is_string()) joined.update(data[1].as_string());
            }
            // Function calls/results and executable code/results contribute an
            // empty field, preserving the page's single-space join semantics.
        }
    }
    return joined.finish();
}
} // namespace

std::string browser_detail::wire_content_hash(const json::array& wire) { return content_hash(wire); }
std::string_view browser_detail::embedded_bridge_source() {
    return {reinterpret_cast<const char*>(embedded_bridge_bytes), sizeof(embedded_bridge_bytes)};
}
std::string_view browser_detail::embedded_bridge_sha256() { return embedded_bridge_digest; }

struct BrowserGateway::Impl {
    Config config;
    Store& store;
    std::mutex mutex;
    OwnedProcess process;
    std::unique_ptr<Cdp> cdp;
    // Lives exactly as long as the browser process it serves.
    std::unique_ptr<browser_detail::ProxyRelay> relay;
    std::string relay_error;
    fs::path profile;
    std::string context, session, account_id, account_fingerprint;
    json::array base_template;
    std::string timezone = "UTC";
    bool models_discovered = false;
    browser_detail::GoogleAuthorizationCache authorization_cache;
    std::string last_status = "not_started";
    std::string phase = "idle";
    // CloakBrowser applies fingerprints per process, so the running browser is
    // tied to the identity it was launched with.
    struct LaunchIdentity {
        unsigned seed = 0;
        std::string timezone, locale;
        bool operator==(const LaunchIdentity&) const = default;
    };
    std::optional<LaunchIdentity> launched;
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
        return {stored.seed, stored.timezone.empty() ? host_timezone() : stored.timezone,
                stored.locale.empty() ? host_locale() : stored.locale};
    }
    // Credentials the current context was built from, for write-back CAS.
    json::object session_state;
    Clock::time_point cookies_saved_at{};
    std::string cookie_refresh = "not_attempted";
    explicit Impl(const Config& c, Store& s) : config(c), store(s) {}
    ~Impl() { close(); }
    void close() noexcept {
        authorization_cache.clear();
        if (cdp) cdp->close();
        launched.reset();
        cdp.reset(); process.stop();
        if (relay) { relay_error = relay->last_error(); relay.reset(); }
        context.clear(); session.clear(); account_id.clear(); account_fingerprint.clear(); base_template.clear(); models_discovered = false; session_state = {};
        if (!profile.empty()) { std::error_code ec; fs::remove_all(profile, ec); profile.clear(); }
        last_status = "closed";
    }
    json::value eval(std::string expression, std::stop_token token, std::chrono::seconds timeout = 30s) { return cdp->evaluate(std::move(expression), session, token, timeout); }
    // Sends a request body to the page in pieces passed as call arguments.
    // The bridge keeps each piece as a Blob, outside the JavaScript heap, so
    // neither a script source nor a page string ever holds the whole body.
    std::string upload(std::string body, std::stop_token token) {
        constexpr std::size_t piece_bytes = 512 * 1024;
        auto bridge = cdp->call("Runtime.evaluate", {{"expression", "globalThis.__ais2bridge"}, {"objectGroup", "ais2api-upload"}}, session, token);
        auto remote = bridge.if_contains("result");
        const auto handle = remote && remote->is_object() ? string_or(remote->as_object(), "objectId") : std::string{};
        if (bridge.if_contains("exceptionDetails") || handle.empty()) incompatible("Page bridge is unavailable for the request upload");
        const auto release = [&] { cdp->notify("Runtime.releaseObjectGroup", {{"objectGroup", "ais2api-upload"}}, session); };
        const auto id = random_hex(8);
        try {
            const std::string_view rest(body);
            std::size_t offset = 0;
            do {
                const auto size = browser_detail::utf8_prefix(rest.substr(offset), piece_bytes);
                cdp->call_function(handle, "function(id, chunk) { return this.append(id, chunk); }", {id, rest.substr(offset, size)}, session, token);
                offset += size;
            } while (offset < rest.size());
        } catch (...) { release(); throw; }
        release();
        return id;
    }
    // A large body's Blob is released only when the page collects its small
    // wrapper, which heap pressure alone may not trigger for a long time.
    void collect_garbage() noexcept {
        try { if (cdp && !session.empty()) cdp->notify("HeapProfiler.collectGarbage", {}, session); } catch (...) {}
    }
    std::string authorization(std::stop_token token) {
        check_cancel(token);
        auto result = authorization_cache.authorization([&] {
            // Query the selected page's cookie store, including HttpOnly values.
            // Never use the import file or cookies from another browser context.
            auto state = cdp->call("Network.getCookies", {{"urls", json::array{"https://aistudio.google.com/"}}}, session, token, 5s);
            auto cookies = state.if_contains("cookies");
            if (!cookies || !cookies->is_array()) incompatible("Browser did not return signing cookies");
            return cookies->as_array();
        });
        check_cancel(token);
        return result;
    }
    void ensure_browser(std::stop_token token, const LaunchIdentity& identity) {
        check_cancel(token);
        if (cdp && cdp->open() && process.alive() &&
            launched == identity) return;
        // A different account fingerprint needs a new browser process.
        close();
        if (config.browser_executable.empty() || !fs::is_regular_file(config.browser_executable)) throw Error(503, "browser_not_configured", "Configure an executable browser or Termux launcher path");
        profile = config.data_dir / "runtime" / ("browser-" + random_hex(12));
        secure_directory(profile);
        std::vector<std::string> args{"--remote-debugging-address=127.0.0.1", "--remote-debugging-port=0", "--user-data-dir=" + path_utf8(fs::absolute(profile)), "--no-first-run", "--no-default-browser-check", "--disable-background-networking", "--disable-dev-shm-usage", "--disable-gpu", "--renderer-process-limit=1"};
        for (const auto& directory : {profile / "crash-dumps", profile / "disk-cache", profile / "logs"}) secure_directory(directory);
        args.push_back("--crash-dumps-dir=" + path_utf8(fs::absolute(profile / "crash-dumps")));
        args.push_back("--disk-cache-dir=" + path_utf8(fs::absolute(profile / "disk-cache")));
        // A long-running browser would otherwise let the cache grow to
        // Chromium's disk-size heuristic.
        args.push_back("--disk-cache-size=" + std::to_string(64 * 1024 * 1024));
        // Persist browser diagnostics beyond temporary profile cleanup.
        secure_directory(config.data_dir / "logs");
        prune_browser_logs(config.data_dir / "logs");
        args.push_back("--enable-logging");
        args.push_back("--log-file=" + path_utf8(fs::absolute(config.data_dir / "logs" /
            (path_utf8(profile.filename()) + ".log"))));
        bool no_zygote = false;
#if !defined(_WIN32) && !defined(__APPLE__)
        // Only Linux has a zygote. CloakBrowser's zygote children can exit
        // while CDP is connected; proot's root user and Android cannot provide
        // the sandbox that needs it anyway.
        no_zygote = true;
#endif
        for (auto& argument : browser_detail::launch_switches(config, no_zygote)) args.push_back(std::move(argument));
        if (!config.proxy.empty()) {
            const auto endpoint = parse_proxy(config.proxy);
            if (config.browser_proxy_relay) {
                if (!endpoint || !relay_supports(*endpoint)) throw Error(400, "invalid_config", "browser_proxy_relay needs an http:// or socks5:// proxy URL");
                relay = std::make_unique<browser_detail::ProxyRelay>();
                args.push_back("--proxy-server=http://127.0.0.1:" + std::to_string(relay->start(*endpoint)));
            } else {
                // Chromium rejects proxy credentials, and a command line is
                // visible to other local users: never pass them on.
                if (endpoint && endpoint->credentials)
                    throw Error(503, "proxy_auth_unsupported", "The browser cannot use a proxy with credentials directly; enable browser_proxy_relay");
                args.push_back("--proxy-server=" + config.proxy);
            }
        }
        // Stable per-account CloakBrowser identity: without a seed every launch
        // is a new random device, and without timezone/locale it presents
        // UTC + en-US. Other Chromium builds ignore these switches.
        if (identity.seed) args.push_back("--fingerprint=" + std::to_string(identity.seed));
        args.push_back("--fingerprint-platform=" + std::string(fingerprint_platform()));
        args.push_back("--fingerprint-timezone=" + identity.timezone);
        args.push_back("--fingerprint-locale=" + identity.locale);
        // WebRTC may only use UDP through the configured proxy, so ICE
        // candidates cannot reveal the real address.
        args.push_back("--force-webrtc-ip-handling-policy=disable_non_proxied_udp");
        args.push_back("about:blank");
        try {
            process.start(config.browser_executable, args, profile);
            const auto deadline = Clock::now() + 30s;
            while (Clock::now() < deadline) {
                check_cancel(token);
                if (!process.alive()) throw Error(502, "browser_launch", "Browser exited before DevTools became ready");
                auto endpoint = read_endpoint(profile);
                if (endpoint.first) { cdp = std::make_unique<Cdp>(); cdp->connect(endpoint.first, endpoint.second, token); launched = identity; last_status = "browser_ready"; return; }
                pause(token, 100ms);
            }
            throw Error(504, "browser_timeout", "Browser did not publish a DevTools endpoint");
        } catch (...) { close(); throw; }
    }
    void dispose_context(std::stop_token token) {
        authorization_cache.clear();
        if (!context.empty()) cdp->call("Target.disposeBrowserContext", {{"browserContextId", context}}, {}, token);
        context.clear(); session.clear(); account_id.clear(); account_fingerprint.clear(); base_template.clear(); models_discovered = false; session_state = {};
    }
    // Google rotates session cookies while the page is in use. Persist the
    // verified context's current cookies so later sessions do not restart from
    // the stale imported set. Best effort; only cancellation propagates.
    void persist_cookies(const Account& account, std::stop_token token) {
        if (context.empty() || session_state.empty()) return;
        try {
            cookies_saved_at = Clock::now();
            auto reply = cdp->call("Storage.getCookies", {{"browserContextId", context}}, {}, token, 5s);
            auto cookies = reply.if_contains("cookies");
            auto stored = session_state.if_contains("cookies");
            auto merged = cookies && cookies->is_array()
                ? browser_detail::refreshed_cookies(stored && stored->is_array() ? stored->as_array() : json::array{}, cookies->as_array())
                : json::array{};
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
                // Redirects can replace an execution context while the account
                // badge is loading. Missing identity is never a proven mismatch.
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
    void ensure_account(const Account& account, std::stop_token token, bool bootstrap, bool identity_only = false) {
        phase = "browser_start";
        ensure_browser(token, identity_for(account));
        auto fingerprint = sha256(json::serialize(account.storage_state) + "\n" + account.auth_user + "\n" + account.email);
        bool verified_session = false;
        if (account.id != account_id || fingerprint != account_fingerprint || session.empty()) {
            // Dispose before create: never keep two account contexts resident.
            dispose_context(token);
            try {
                context = string_or(cdp->call("Target.createBrowserContext", {{"disposeOnDetach", true}}, {}, token), "browserContextId");
                if (context.empty()) incompatible("Browser did not create an isolated context");
                auto cookies = account.storage_state.if_contains("cookies");
                if (!cookies || !cookies->is_array() || cookies->as_array().empty()) throw Error(401, "missing_cookies", "Account has no storage-state cookies");
                cdp->call("Storage.setCookies", {{"cookies", browser_detail::injected_cookies(cookies->as_array())}, {"browserContextId", context}}, {}, token);
                auto target = string_or(cdp->call("Target.createTarget", {{"url", "about:blank"}, {"browserContextId", context}}, {}, token), "targetId");
                if (target.empty()) incompatible("Browser did not create an isolated page");
                session = string_or(cdp->call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}}, {}, token), "sessionId");
                if (session.empty()) incompatible("Browser did not attach the page session");
                cdp->call("Page.enable", {}, session, token);
                cdp->call("Runtime.enable", {}, session, token);
                // Renderer crash notifications (best effort; generation does
                // not depend on them).
                try { cdp->call("Inspector.enable", {}, session, token, 5s); }
                catch (const Error& error) { if (error.code == "cancelled") throw; }
                cdp->call("Page.addScriptToEvaluateOnNewDocument", {{"source", bridge_source()}}, session, token);
                if (auto origins = account.storage_state.if_contains("origins"); origins && origins->is_array()) {
                    const auto restore = "(() => {const entries=" + json::serialize(*origins) + ";for(const e of entries){if(e.origin!==location.origin||!Array.isArray(e.localStorage))continue;for(const p of e.localStorage){if(typeof p.name==='string'&&typeof p.value==='string')localStorage.setItem(p.name,p.value);}}})()";
                    cdp->call("Page.addScriptToEvaluateOnNewDocument", {{"source", restore}}, session, token);
                }
                if (account.auth_user.empty() || !std::all_of(account.auth_user.begin(), account.auth_user.end(), [](unsigned char c) { return std::isdigit(c); })) throw Error(400, "invalid_auth_user", "auth_user must be a nonnegative integer");
                const auto navigation = cdp->call("Page.navigate", {{"url", "https://aistudio.google.com/prompts/new_chat?authuser=" + account.auth_user}}, session, token);
                // Without this an unreachable page waits out the identity
                // check and is blamed on the account.
                if (auto failure = browser_detail::navigation_failure(string_or(navigation, "errorText"), !config.proxy.empty())) {
                    if (failure->code == "proxy_failed" && relay)
                        if (const auto detail = relay->last_error(); !detail.empty()) failure = Error(failure->status, failure->code, std::string(failure->what()) + ": " + detail);
                    throw *failure;
                }
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
            bool trusted_shortcut = !shortcut;
            const auto deadline = triggered_at + 45s;
            phase = "bootstrap_capture";
            while (Clock::now() < deadline) {
                if (!trusted_shortcut && !template_seen && Clock::now() - triggered_at >= 1s) {
                    trusted_shortcut = true;
                    const int modifiers = fingerprint_platform() == "macos" ? 4 : 2; // Meta : Ctrl
                    for (auto type : {"rawKeyDown", "keyUp"})
                        cdp->call("Input.dispatchKeyEvent", {{"type", type}, {"modifiers", modifiers},
                            {"windowsVirtualKeyCode", 13}, {"code", "Enter"}, {"key", "Enter"}}, session, token);
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
};

BrowserGateway::BrowserGateway(const Config& config, Store& store) : impl_(std::make_unique<Impl>(config, store)) {}
BrowserGateway::~BrowserGateway() = default;
json::object BrowserGateway::verify_runtime(std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    try {
        impl_->ensure_browser(cancel, {0, host_timezone(), host_locale()});
        auto version=impl_->cdp->call("Browser.getVersion",{}, {},cancel);
        auto context=string_or(impl_->cdp->call("Target.createBrowserContext",{{"disposeOnDetach",true}}, {},cancel),"browserContextId");
        if(context.empty())incompatible("Browser did not create a verification context");
        impl_->cdp->call("Target.disposeBrowserContext",{{"browserContextId",context}}, {},cancel);
        impl_->close();
        return {{"product",string_or(version,"product")},{"protocol_version",string_or(version,"protocolVersion")},{"isolated_context",true},{"google_verified",false}};
    } catch (...) { impl_->close();throw; }
}
void BrowserGateway::close() { std::lock_guard lock(impl_->mutex); impl_->close(); }
void BrowserGateway::reset_session() {
    std::lock_guard lock(impl_->mutex);
    // A new context reloads cookies, the page, BotGuard and the request template.
    try {
        if (impl_->cdp && impl_->process.alive()) impl_->dispose_context({});
        else impl_->close();
    } catch (...) { impl_->close(); }
}
void BrowserGateway::clear_template() {
    std::lock_guard lock(impl_->mutex);
    // The next generate() bootstraps the resident page again; the bridge drops
    // its own captured template when that bootstrap starts.
    impl_->base_template.clear();
}
bool BrowserGateway::release_idle() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->cdp && !impl_->process.alive()) return false;
    // Keep the cookies Google rotated during this session before its context
    // goes away. Best effort, like the periodic write-back.
    if (!impl_->account_id.empty())
        try { impl_->persist_cookies(impl_->store.account(impl_->account_id), {}); } catch (...) {}
    impl_->close();
    impl_->last_status = "idle_closed";
    impl_->phase = "idle";
    return true;
}
void BrowserGateway::reconfigure(const Config& config) {
    std::lock_guard lock(impl_->mutex);
    // Transport/lifecycle settings require a service restart. These fields are
    // the model policy that config.reload may atomically replace.
    impl_->config.model_aliases = config.model_aliases;
    impl_->config.model_defaults = config.model_defaults;
}
json::object BrowserGateway::diagnostics() {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return {{"status", "busy"}, {"phase", "active"}, {"bridgeSource", "embedded"},
                {"bridgeSha256", browser_detail::embedded_bridge_sha256()}};
    return {{"status", impl_->last_status}, {"phase", impl_->phase},
        {"bridgeSource", "embedded"}, {"bridgeSha256", browser_detail::embedded_bridge_sha256()},
        {"running", impl_->process.alive()}, {"isolatedContext", !impl_->context.empty()}, {"templateCaptured", !impl_->base_template.empty()}, {"modelDiscovery", impl_->models_discovered ? "live_rpc" : "not_loaded"}, {"chunkBytes", 32768},
        {"cookieRefresh", impl_->cookie_refresh},
        {"proxy", json::object{{"mode", impl_->config.proxy.empty() ? "none" : impl_->config.browser_proxy_relay ? "relay" : "direct"},
                               {"relayError", impl_->relay ? impl_->relay->last_error() : impl_->relay_error}}},
        {"fingerprint", impl_->launched ? json::value(json::object{{"seed", impl_->launched->seed},
            {"platform", fingerprint_platform()}, {"timezone", impl_->launched->timezone},
            {"locale", impl_->launched->locale}}) : json::value(nullptr)}};
}
json::object BrowserGateway::probe(const Account& account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, false, true);
    auto result = impl_->identity(cancel); result["accountId"] = account.id;
    result["ready"] = bool_or(result, "identityReady");
    result["status"] = string_or(result, "email").empty() ? "identity_unverified" : "ready";
    result["generationVerified"] = false;
    return result;
}
json::object BrowserGateway::probe_import(Account& account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, false, true);
    const auto info = impl_->identity(cancel);
    const auto email = string_or(info, "email");
    if (!bool_or(info, "identityReady") || bool_or(info, "identityAmbiguous") || email.empty())
        throw Error(409, "identity_unverified", "Import requires a ready AI Studio page with one verified Google identity");
    auto state = impl_->cdp->call("Storage.getCookies", {{"browserContextId", impl_->context}}, {}, cancel);
    auto cookies = state.if_contains("cookies");
    if (!cookies || !cookies->is_array()) incompatible("Browser did not return context cookies");
    std::size_t skipped_partition_cookies = 0;
    auto refreshed = browser_detail::import_cookies(cookies->as_array(), &skipped_partition_cookies);
    if (refreshed.empty()) throw Error(401, "missing_cookies", "Verified browser context has no Google cookies");
    check_cancel(cancel);
    account.storage_state["cookies"] = std::move(refreshed);
    impl_->account_fingerprint = sha256(json::serialize(account.storage_state) + "\n" + account.auth_user + "\n" + account.email);
    return {{"accountId", account.id}, {"email", email}, {"ready", true}, {"login", false}, {"identityAmbiguous", false}, {"status", "ready"}, {"generationVerified", false}, {"skippedPartitionCookies", skipped_partition_cookies}};
}
json::array BrowserGateway::models(const Account &account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    check_cancel(cancel);
    // Model discovery verifies this account without generation initialization.
    impl_->ensure_account(account, cancel, false, true);
    const auto authorization = impl_->authorization(cancel);
    auto result = impl_->eval("globalThis.__ais2bridge.discoverModels(" + quote_js(account.auth_user) + "," + quote_js(authorization) + ")", cancel, 25s);
    if (!result.is_array()) incompatible("ListModels returned an unsupported response");
    impl_->models_discovered = true;
    return result.as_array();
}
void BrowserGateway::generate(const Request& request, const Account& account, const EventSink& sink, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_account(account, cancel, true);
    impl_->phase = "snapshot_ticket";
    auto wire = encode_request(request, impl_->base_template, "", impl_->timezone);
    // Covers the bridge's bounded snapshot retries (3 x 9 s).
    auto snapshot = impl_->eval("globalThis.__ais2bridge.snapshot(" + quote_js(content_hash(wire)) + ")", cancel, 30s);
    if (!snapshot.is_string() || snapshot.as_string().empty()) incompatible("Snapshot service produced no request ticket");
    wire[4] = snapshot;
    auto body = json::serialize(wire);
    json::array().swap(wire);
    const auto chunk_bytes = std::min<std::size_t>(32768, impl_->config.limits.relay_bytes);
    if (!chunk_bytes) throw Error(500, "invalid_config", "Browser relay byte budget must be positive");
    impl_->phase = "request_authorization";
    const auto authorization = impl_->authorization(cancel);
    const bool large_body = body.size() >= 1024 * 1024;
    WireStreamParser parser(impl_->config.limits.object_bytes);
    std::uint64_t sequence = 0;
    std::string error_prefix;
    constexpr std::size_t error_prefix_limit = 16384;
    std::optional<Error> parse_failure;
    try {
        impl_->phase = "generation_upload";
        const auto upload = impl_->upload(std::move(body), cancel);
        impl_->phase = "generation_start";
        impl_->eval("globalThis.__ais2bridge.start({upload:" + quote_js(upload) + ",authUser:" + quote_js(account.auth_user) +
                    ",authorization:" + quote_js(authorization) + ",chunkBytes:" + std::to_string(chunk_bytes) + "})", cancel);
        const auto began = Clock::now();
        auto activity = began;
        impl_->phase = "generation_read";
        // Sequence of the consumed chunk the next call acknowledges (0: none).
        std::uint64_t unacknowledged = 0;
        while (true) {
            check_cancel(cancel);
            if (!request.stream && Clock::now() - began > std::chrono::seconds(impl_->config.limits.generation_seconds)) throw Error(504, "generation_timeout", "Generation deadline exceeded");
            if (Clock::now() - activity > std::chrono::seconds(impl_->config.limits.stream_idle_seconds)) throw Error(504, "stream_idle_timeout", "No upstream stream bytes before idle deadline");
            // The page answers as soon as a chunk or the end arrives.
            auto value = impl_->eval("globalThis.__ais2bridge.next(" + std::to_string(unacknowledged) + ",250)", cancel, 5s);
            unacknowledged = 0;
            check_cancel(cancel);
            if (!value.is_object()) incompatible("Browser relay state was lost");
            const auto& state = value.as_object();
            auto classified = [&](Error error) {
                if (error.status == 403 && string_or(state, "benefitTier", "unknown") == "unknown")
                    error.code = "benefit_tier_unconfirmed";
                return error;
            };
            const auto retry_value = state.if_contains("retryAfter");
            const auto retry_after = retry_value && retry_value->is_int64() && retry_value->as_int64() > 0 ? static_cast<unsigned>(std::min<std::int64_t>(86400, retry_value->as_int64())) : 0u;
            auto pending = state.if_contains("pending");
            if (pending && pending->is_object()) {
                auto number = pending->as_object().if_contains("sequence");
                if (!number || !number->is_int64() || number->as_int64() != static_cast<std::int64_t>(sequence + 1)) incompatible("Browser relay sequence mismatch");
                const auto bytes = decode64(string_or(pending->as_object(), "bytes"));
                if (bytes.empty()) incompatible("Empty browser relay credit");
                if (error_prefix.size() < error_prefix_limit) error_prefix.append(bytes.substr(0, error_prefix_limit - error_prefix.size()));
                if (auto error = browser_detail::rpc_text_error(error_prefix, retry_after)) throw classified(*error);
                try { if (!parse_failure) parser.feed(bytes, [&](json::value chunk) {
                    check_cancel(cancel);
                    if (auto error = browser_detail::rpc_error(chunk, retry_after)) throw classified(*error);
                    auto event = decode_chunk(chunk);
                    if (!event.empty()) sink(std::move(event));
                    check_cancel(cancel);
                }); } catch (const Error& error) {
                    if (error.code != "invalid_upstream" && error.code != "truncated_upstream") throw;
                    parse_failure = error;
                }
                if (parse_failure && error_prefix.size() >= error_prefix_limit) throw *parse_failure;
                if (!request.stream && Clock::now() - began > std::chrono::seconds(impl_->config.limits.generation_seconds)) throw Error(504, "generation_timeout", "Generation deadline exceeded while consuming downstream events");
                // EventSink returns only once downstream consumed its event.
                // ACK grants the page permission for exactly one next read.
                unacknowledged = ++sequence;
                activity = Clock::now();
            } else if (bool_or(state, "done")) {
                const auto error = string_or(state, "error");
                const auto status_value = state.if_contains("status");
                const auto status = status_value && status_value->is_int64() ? status_value->as_int64() : 0;
                if (status == 204) throw Error(502, "upstream_no_content", "Google returned an empty response");
                if (!error.empty()) {
                    const auto error_body = string_or(state, "errorBody");
                    if (auto rpc = browser_detail::rpc_text_error(error_body, retry_after)) throw classified(*rpc);
                    if (auto http = browser_detail::rpc_status_error(status, retry_after, error_body)) throw classified(*http);
                    throw Error(502, error.starts_with("browser_incompatible") ? "browser_incompatible" : "upstream_error", "Browser generation transport failed");
                }
                if (auto rpc = browser_detail::rpc_text_error(error_prefix, retry_after)) throw classified(*rpc);
                if (parse_failure) throw *parse_failure;
                parser.finish(); impl_->phase = "generation_complete";
                if (Clock::now() - impl_->cookies_saved_at >= 300s)
                    try { impl_->persist_cookies(account, {}); } catch (...) {}
                if (large_body) impl_->collect_garbage();
                return;
            }
        }
    } catch (...) {
        try { impl_->eval("globalThis.__ais2bridge.abort()", {}, 3s); } catch (...) { impl_->close(); }
        if (large_body) impl_->collect_garbage();
        throw;
    }
}
} // namespace ais2api
