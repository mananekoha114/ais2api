// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The experimental SpiderMonkey backend: the AI Studio page without a
// browser. The page's requests go out over libcurl and Google's WAA program
// runs in the embedded engine. After AIStudio2API's
// internal/aistudio/runtime_go.go (MIT).
#include "ais2api/spidermonkey.hpp"
#include "../browser/firefox_http.hpp"
#include "../browser/generation.hpp"
#include "../browser/google_auth.hpp"
#include "ais2api/browser.hpp"
#include "ais2api/console.hpp"
#include "ais2api/firefox_identity.hpp"
#include "ais2api/region.hpp"
#include "ais2api/store.hpp"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <tuple>
#include <openssl/evp.h>

namespace ais2api {
namespace {
using namespace std::chrono_literals;
namespace sm = spidermonkey_detail;
using browser_detail::HttpRequest;
using browser_detail::HttpSession;

constexpr std::string_view origin = "https://aistudio.google.com";
constexpr std::string_view maker_suite =
    "https://alkalimakersuite-pa.clients6.google.com/$rpc/google.internal.alkali.applications.makersuite.v1.MakerSuiteService/";
constexpr std::string_view waa_create_url = "https://waa-pa.clients6.google.com/$rpc/google.internal.waa.v1.Waa/Create";
// What the page's Waa/Create carries: its request key, and the WAA service's
// own API key in place of the page's.
constexpr std::string_view waa_request_key = "lmnUSbltwc5ULv48iKLX";
constexpr std::string_view waa_api_key = "AIzaSyBGb5fGAyC-pRcRU6MUHb__b_vKha71HRE";
// A page renews its WAA program after this long.
constexpr auto vm_lifetime = 12h;
// The page counts its prompt box's tokens this long after Run.
constexpr auto count_tokens_delay = 2200ms;
// The Firefox the prototype claims: the version of its SpiderMonkey and of
// the shape table the host follows.
constexpr std::string_view claimed_firefox = "158";
// Firefox follows at most this many redirects in a row.
constexpr int redirect_limit = 20;

[[noreturn]] void incompatible(std::string message) { throw Error(502, "browser_incompatible", std::move(message)); }

std::string base64(std::string_view bytes) {
    std::string encoded((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                     reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}
std::string query_escape(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
std::string host_of(std::string_view url) {
    const auto scheme = url.find("://");
    auto host = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    return std::string(host.substr(0, host.find_first_of("/?#:")));
}
std::vector<std::string> header_values(const browser_detail::HttpHeaders &headers, std::string_view name) {
    std::vector<std::string> values;
    for (const auto &[key, value] : headers)
        if (key.size() == name.size() && std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
            values.push_back(value);
    return values;
}
unsigned retry_after(const browser_detail::HttpHeaders &headers) {
    const auto value = browser_detail::header_value(headers, "retry-after");
    if (value.empty() || value.size() > 6 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
        return 0;
    return static_cast<unsigned>(std::min<unsigned long>(86400, std::stoul(value)));
}
std::chrono::milliseconds whole_milliseconds(std::chrono::microseconds duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
}
// Waits until `moment`; 499 when the token is requested first.
void wait_until(std::chrono::system_clock::time_point moment, std::stop_token token) {
    const auto left = moment - std::chrono::system_clock::now();
    if (left > std::chrono::system_clock::duration::zero()) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        wake.wait_for(lock, token, left, [] { return false; });
    }
    check_cancel(token);
}
// The interpreter's name in the cache directory: its digest, which Waa/Create
// sends as unpadded URL-safe Base64.
bool cacheable_hash(std::string_view hash) {
    return !hash.empty() && hash.size() <= 128 &&
           std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}
} // namespace

struct SpiderMonkeyGateway::Impl {
    Config config;
    Store &store;
    std::mutex mutex;
    std::string phase = "idle", last_status = "idle", cookie_refresh = "not_attempted";

    // The resident account: its cookies and connections, and the identity the
    // page's requests and host show.
    std::string account_id, account_fingerprint, auth_user = "0";
    json::object session_state;
    std::unique_ptr<HttpSession> http;
    sm::HostIdentity host;
    sm::PageHeaders headers;
    std::string platform;
    browser_detail::GoogleAuthorizationCache authorization_cache;
    Clock::time_point cookies_saved_at{};

    // What loading the page gave.
    struct Page {
        std::string url, api_key, visit_id, extension, email;
        json::object globals;
        bool trusted_types = false;
        std::chrono::system_clock::time_point navigation_start, loaded;
        // The page's PerformanceNavigationTiming and the legacy domLoading.
        json::object navigation;
        int dom_loading = 0;
        // Where the trusted pointer and the focus are, for the page's
        // programs in turn.
        std::shared_ptr<waa::Pointer> pointer = std::make_shared<waa::Pointer>();
    };
    std::optional<Page> page;
    std::string tier = "unknown";
    Clock::time_point tier_expires{}, tier_retry{};
    bool models_discovered = false;

    // The page's WAA program.
    std::unique_ptr<waa::Runtime> vm;
    Clock::time_point vm_started{};
    std::string interpreter_hash;
    unsigned vms_started = 0;

    // The program whose click on Run or Rerun the next GenerateContent
    // answers, and that response while its body is open: the page replaces
    // the element under the pointer when the response starts and ends.
    waa::Runtime *running = nullptr, *answering = nullptr;
    // The conversations each account's page ran, to run one again as Rerun.
    sm::PageRuns page_runs;
    std::mt19937_64 random{std::random_device{}()};

    // The CountTokens the page sends after Run.
    std::jthread follow_up;

    Impl(const Config &c, Store &s) : config(c), store(s) {}
    ~Impl() { close(); }

    void drop_page() noexcept {
        running = answering = nullptr;
        // The program's loads use the session: they end before it does.
        vm.reset();
        page.reset();
        authorization_cache.clear();
        tier = "unknown";
        tier_expires = tier_retry = {};
        models_discovered = false;
    }
    void close() noexcept {
        if (follow_up.joinable()) {
            follow_up.request_stop();
            follow_up.join();
        }
        drop_page();
        http.reset();
        account_id.clear();
        account_fingerprint.clear();
        session_state = {};
        last_status = "closed";
        phase = "idle";
    }

    static std::string fingerprint_of(const Account &account) {
        return sha256(json::serialize(account.storage_state) + "\n" + account.auth_user + "\n" + account.email);
    }

    // The account's cookies, identity and connections, kept while the account
    // and its stored state stay the same.
    void ensure_session(const Account &account) {
        if (http && account.id == account_id && fingerprint_of(account) == account_fingerprint) return;
        close();
        phase = "session";
        if (account.auth_user.empty() || account.auth_user.size() > 3 ||
            !std::all_of(account.auth_user.begin(), account.auth_user.end(), [](unsigned char c) { return std::isdigit(c); }))
            throw Error(400, "invalid_auth_user", "auth_user must be a nonnegative integer");
        const auto *cookies = account.storage_state.if_contains("cookies");
        if (!cookies || !cookies->is_array() || cookies->as_array().empty())
            throw Error(401, "missing_cookies", "Account has no storage-state cookies");
        const auto saved = store.firefox_identity(account.id);
        if (!saved)
            throw Error(503, "browser_not_configured",
                        "The account has no Firefox identity yet: serve one request with the camoufox backend first");
        BrowserFingerprint fingerprint;
        try {
            fingerprint = store.fingerprint(account.id);
        } catch (const Error &error) {
            if (error.code != "account_not_found") throw;
        }
        const auto identity = firefox_identity::from_json(*saved);
        auto config_values = identity.config;
        if (auto *user_agent = config_values.if_contains("navigator.userAgent"); user_agent && user_agent->is_string())
            *user_agent = firefox_identity::detail::rewrite_version(user_agent->as_string(), claimed_firefox);
        firefox_identity::detail::apply_locale(config_values, fingerprint.locale.empty() ? host_locale() : fingerprint.locale,
                                               fingerprint.timezone.empty() ? host_timezone() : fingerprint.timezone);
        host = sm::host_identity(config_values);
        if (host.user_agent.empty()) incompatible("The account's Firefox identity has no user agent");
        headers = {host.user_agent, host.accept_language, HttpSession::accept_encoding()};
        platform = std::string(firefox_identity::os_name(identity.os));
        HttpSession::Options options;
        options.proxy = config.proxy;
        options.cookies = browser_detail::injected_cookies(cookies->as_array());
        http = std::make_unique<HttpSession>(std::move(options));
        account_id = account.id;
        account_fingerprint = fingerprint_of(account);
        auth_user = std::to_string(std::stoul(account.auth_user));
        session_state = account.storage_state;
        cookies_saved_at = Clock::now();
        last_status = "session_ready";
    }

    std::string authorization() {
        auto value = authorization_cache.authorization([&] { return http->cookies(); });
        if (value.empty()) throw Error(401, "missing_cookies", "The account's cookies cannot sign requests");
        return value;
    }
    sm::RpcContext rpc_context(bool page_context) {
        sm::RpcContext rpc;
        rpc.api_key = page ? page->api_key : std::string();
        rpc.auth_user = auth_user;
        rpc.authorization = authorization();
        if (page_context && page) {
            rpc.visit_id = page->visit_id;
            rpc.extension = page->extension;
        }
        return rpc;
    }
    browser_detail::HttpResponse rpc(std::string_view method, std::string body, const sm::RpcContext &context, std::stop_token token,
                                     std::chrono::milliseconds timeout = 30s) {
        HttpRequest request{"POST", std::string(maker_suite) + std::string(method), sm::rpc_headers(headers, context), std::move(body)};
        return http->send(std::move(request), token, timeout, 16 << 20);
    }

    // The page, loaded as the address bar loads it, then the RPCs its
    // application needs before anything else: the logging context, whose
    // fields become a header of later requests, and the benefit tier.
    void ensure_page(const Account &account, std::string_view model, std::stop_token token) {
        ensure_session(account);
        if (page) return;
        phase = "page_load";
        Page loaded;
        auto target = std::string(origin) + (auth_user == "0" ? "" : "/u/" + auth_user) + "/prompts/new_chat?" +
                      (model.empty() ? std::string() : "model=" + query_escape(model) + "&") + "temporary=true";
        loaded.navigation_start = std::chrono::system_clock::now();
        browser_detail::HttpResponse response;
        // A browser follows Google's sign-in check through accounts.google.com,
        // which refreshes the session's cookies and comes back; only a chain
        // that ends there is a sign-in.
        for (int redirect = 0;; ++redirect) {
            if (redirect > 10) incompatible("AI Studio redirected the page too often");
            // Firefox gives the top-level document its highest weight.
            response = http->send({"GET", target, sm::document_headers(headers), std::nullopt, 42, true}, token, 30s, 16 << 20);
            if (response.status < 300 || response.status >= 400) break;
            const auto next = sm::resolve_location(target, browser_detail::header_value(response.headers, "location"));
            if (next.empty()) incompatible("AI Studio redirected the page nowhere");
            // Where to, without the query, which may carry account data.
            console::emit_record({{"event", "spidermonkey_redirect"}, {"status", response.status},
                                  {"to", next.substr(0, next.find_first_of("?#"))}});
            target = next;
        }
        if (host_of(target) == "accounts.google.com") throw Error(401, "session_expired", "Google redirected the account to sign-in");
        if (target.find("/available-regions") != std::string::npos)
            throw Error(403, "region_blocked", "AI Studio is unavailable in the account's region");
        if (response.status != 200)
            throw Error(502, "upstream_error", "The AI Studio page returned HTTP " + std::to_string(response.status));
        loaded.loaded = std::chrono::system_clock::now();
        const auto since_start = [&](std::chrono::system_clock::time_point moment) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(moment - loaded.navigation_start);
        };
        const auto &timing = response.timing;
        sm::PageLoad load{.url = target,
                          .protocol = response.protocol,
                          .new_connection = timing.new_connection,
                          .lookup = whole_milliseconds(timing.resolved),
                          .connect = whole_milliseconds(timing.connected - timing.resolved),
                          .tunnel = timing.tunneled > timing.connected ? whole_milliseconds(timing.tunneled - timing.connected)
                                                                       : std::chrono::milliseconds(0),
                          .end = since_start(loaded.loaded),
                          .decoded = response.body.size()};
        std::tie(loaded.navigation, loaded.dom_loading) = sm::navigation_timing(load, random);
        // The page's subresources are not loaded here: the document's parsing
        // follows from the body's end.
        sm::document_timing(loaded.navigation, load.end, random);
        loaded.url = target;
        loaded.api_key = sm::page_api_key(response.body);
        if (loaded.api_key.empty()) incompatible("The AI Studio page carries no API key");
        loaded.globals = sm::page_globals(response.body);
        loaded.trusted_types = sm::requires_trusted_types(header_values(response.headers, "content-security-policy"));
        loaded.email = sm::page_email(response.body);
        const auto lower = [](std::string text) {
            for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        };
        if (!loaded.email.empty() && !account.email.empty() && lower(loaded.email) != lower(account.email))
            throw Error(401, "identity_mismatch", "The verified Google account differs from the selected account");
        std::string().swap(response.body);
        loaded.visit_id = sm::new_visit_id();
        page = std::move(loaded);

        phase = "logging_context";
        try {
            auto context = rpc_context(false);
            context.visit_id = page->visit_id;
            const auto logging = rpc("GetLoggingContext", "[]", context, token);
            if (auto failure = sm::page_rpc_failure("GetLoggingContext", logging.status, logging.body)) throw *failure;
            if (const auto fields = sm::encode_jspb_fields(logging.body); !fields.empty()) page->extension = base64(fields);
            refresh_tier(token, true);
        } catch (...) {
            page.reset();
            throw;
        }
        last_status = "page_ready";
        phase = "ready";
    }

    // The benefit tier, as the page reads it: kept ten minutes, asked again
    // after 30 s when the answer was not one.
    void refresh_tier(std::stop_token token, bool force = false) {
        const auto now = Clock::now();
        if (!force && (now < tier_expires || now < tier_retry)) return;
        try {
            const auto response = rpc("GetAiStudioBenefitTier", "[]", rpc_context(true), token, 8s);
            const auto read = response.status == 200 ? sm::benefit_tier(response.body) : std::string("unknown");
            tier = read;
            if (read == "unknown") {
                tier_expires = {};
                tier_retry = now + 30s;
            } else {
                tier_expires = now + 10min;
                tier_retry = {};
            }
        } catch (const Error &error) {
            if (error.code == "cancelled") throw;
            tier = "unknown";
            tier_expires = {};
            tier_retry = now + 30s;
        }
    }
    std::string fresh_tier() const { return Clock::now() < tier_expires ? tier : std::string("unknown"); }

    waa::Challenge create_challenge(std::vector<std::string> refresh, std::stop_token token) {
        json::array body{waa_request_key};
        for (auto &item : refresh) body.emplace_back(std::move(item));
        auto context = rpc_context(false);
        context.api_key = std::string(waa_api_key);
        HttpRequest request{"POST", std::string(waa_create_url), sm::waa_headers(headers, context), json::serialize(body)};
        const auto response = http->send(std::move(request), token, 30s, 8 << 20);
        if (auto failure = sm::page_rpc_failure("Waa/Create", response.status, response.body)) throw *failure;
        return waa::parse_challenge(response.body);
    }

    // The interpreter a challenge names: inline, cached by its digest, or
    // fetched as the page's <script> fetches it. Its digest is checked.
    std::string load_interpreter(const waa::Challenge &challenge, std::stop_token token) {
        if (!challenge.interpreter_javascript.empty()) {
            if (waa::interpreter_hash(challenge.interpreter_javascript) != challenge.interpreter_hash)
                throw Error(502, "waa_challenge_invalid", "The inline WAA interpreter does not match its digest");
            return challenge.interpreter_javascript;
        }
        if (!cacheable_hash(challenge.interpreter_hash)) throw Error(502, "waa_challenge_invalid", "The WAA interpreter digest is malformed");
        const auto directory = config.data_dir / "runtime" / "waa-interpreters";
        const auto file = directory / (challenge.interpreter_hash + ".js");
        std::error_code missing;
        if (fs::is_regular_file(file, missing)) {
            try {
                auto cached = read_file(file, 8 << 20);
                if (waa::interpreter_hash(cached) == challenge.interpreter_hash) return cached;
            } catch (const std::exception &) {
            }
        }
        const auto &url = challenge.interpreter_url;
        const auto server = host_of(url);
        if (!url.starts_with("https://") || !(server == "google.com" || server.ends_with(".google.com") || server.ends_with(".gstatic.com")))
            throw Error(502, "waa_challenge_invalid", "The WAA interpreter is not served by Google");
        phase = "waa_interpreter";
        const auto response = http->send({"GET", url, sm::script_headers(headers, url), std::nullopt}, token, 30s, 8 << 20);
        if (response.status != 200)
            throw Error(502, "upstream_error", "The WAA interpreter returned HTTP " + std::to_string(response.status));
        if (waa::interpreter_hash(response.body) != challenge.interpreter_hash)
            throw Error(502, "waa_challenge_invalid", "The WAA interpreter does not match its digest");
        try {
            secure_directory(directory);
            atomic_write(file, response.body);
        } catch (const std::exception &) {
            // The cache only saves a download.
        }
        return response.body;
    }

    std::unique_ptr<waa::Runtime> start_vm(const waa::Challenge &challenge, std::stop_token token) {
        auto interpreter = load_interpreter(challenge, token);
        phase = "waa_start";
        waa::Options options;
        options.challenge = challenge;
        options.interpreter = std::move(interpreter);
        auto &profile = options.profile;
        profile = host.profile;
        profile.page_url = page->url;
        profile.globals = page->globals;
        profile.trusted_types = page->trusted_types;
        profile.navigation_start = page->navigation_start;
        profile.navigation = page->navigation;
        profile.dom_loading = page->dom_loading;
        options.pointer = page->pointer;
        profile.document = {{"cookie", sm::document_cookie(http->cookies())},
                            {"lastModified", sm::last_modified(page->loaded, profile.timezone)}};
        if (const auto *origins = session_state.if_contains("origins"); origins && origins->is_array())
            for (const auto &entry : origins->as_array()) {
                if (!entry.is_object() || string_or(entry.as_object(), "origin") != origin) continue;
                if (const auto *items = entry.as_object().if_contains("localStorage"); items && items->is_array())
                    for (const auto &item : items->as_array())
                        if (item.is_object())
                            profile.local_storage[string_or(item.as_object(), "name")] = string_or(item.as_object(), "value");
            }
        // Loads the program asks for, on the runtime's threads. The runtime
        // ends them before it is destroyed, and it is destroyed before the
        // session.
        auto *session = http.get();
        const auto page_headers = headers;
        options.load_image = [session, page_headers](std::string_view url, std::stop_token stop) {
            try {
                // Firefox weighs an image of urgency 5 at 12.
                const auto response =
                    session->send({"GET", std::string(url), sm::image_headers(page_headers, url), std::nullopt, 12}, stop, 30s, 4 << 20);
                return response.status == 200 && browser_detail::header_value(response.headers, "content-type").starts_with("image/") &&
                       !response.body.empty();
            } catch (const std::exception &) {
                return false;
            }
        };
        options.load_frame = [session, page_headers](std::string_view url, std::stop_token stop) {
            std::string target(url);
            try {
                for (int redirect = 0; redirect <= redirect_limit; ++redirect) {
                    const auto response = session->send({"GET", target, sm::frame_headers(page_headers, target), std::nullopt}, stop, 30s, 8 << 20);
                    if (response.status < 300 || response.status >= 400) return true;
                    target = sm::resolve_location(target, browser_detail::header_value(response.headers, "location"));
                    if (target.empty()) return false;
                }
            } catch (const std::exception &) {
            }
            return false;
        };
        // The page's application starts its program once the load event is
        // over.
        if (const auto *end = page->navigation.if_contains("loadEventEnd"); end && end->is_int64()) {
            phase = "page_load_event";
            wait_until(page->navigation_start + std::chrono::milliseconds(end->as_int64()), token);
            phase = "waa_start";
        }
        const auto started = Clock::now();
        auto runtime = std::make_unique<waa::Runtime>(std::move(options), token);
        phase = "waa_settle";
        runtime->settle(token);
        ++vms_started;
        if (interpreter_hash != challenge.interpreter_hash)
            console::emit_record({{"event", "spidermonkey_interpreter"}, {"hash", challenge.interpreter_hash.substr(0, 12)}});
        interpreter_hash = challenge.interpreter_hash;
        console::emit_record({{"event", "spidermonkey_vm"}, {"account_id", account_id},
                              {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()}});
        return runtime;
    }

    // The page with a settled WAA program, renewed once it is 12 hours old
    // with the old program's unbound snapshot, as the page renews it.
    void ensure_vm(const Account &account, std::string_view model, std::stop_token token) {
        ensure_page(account, model, token);
        if (vm && Clock::now() - vm_started < vm_lifetime) return;
        std::vector<std::string> refresh;
        if (vm) {
            phase = "waa_refresh";
            std::string snapshot;
            try {
                snapshot = vm->refresh_snapshot(token);
            } catch (const Error &error) {
                if (error.code == "cancelled") throw;
                snapshot = "E:UCE";
            }
            refresh = {interpreter_hash, snapshot};
        }
        phase = "waa_create";
        auto challenge = create_challenge(std::move(refresh), token);
        auto next = start_vm(challenge, token);
        vm = std::move(next);
        vm_started = Clock::now();
        last_status = "gateway_ready";
        phase = "ready";
    }

    // Google rotates session cookies; keep the store's copy current. Best
    // effort; only cancellation propagates.
    void persist_cookies(const Account &account) {
        if (!http || session_state.empty()) return;
        try {
            cookies_saved_at = Clock::now();
            if (!http->take_cookie_change()) {
                cookie_refresh = "unchanged";
                return;
            }
            const auto *stored = session_state.if_contains("cookies");
            auto merged = browser_detail::refreshed_cookies(stored && stored->is_array() ? stored->as_array() : json::array{}, http->cookies());
            if (merged.empty()) {
                cookie_refresh = "failed";
                return;
            }
            if (!store.refresh_cookies(account.id, session_state, merged)) {
                cookie_refresh = "unchanged";
                return;
            }
            const auto saved = store.account(account.id);
            session_state = saved.storage_state;
            account_fingerprint = fingerprint_of(saved);
            cookie_refresh = "saved";
        } catch (const Error &error) {
            if (error.code == "cancelled") throw;
            cookie_refresh = "failed";
        }
    }

    // The CountTokens request the page sends for its prompt box once Run was
    // clicked, in the background, with the benefit tier as GenerateContent's;
    // its answer is not needed.
    void schedule_count_tokens(std::string body, const sm::RpcContext &context) {
        if (follow_up.joinable()) {
            follow_up.request_stop();
            follow_up.join();
        }
        auto *session = http.get();
        HttpRequest request{"POST", std::string(maker_suite) + "CountTokens", sm::rpc_headers(headers, context), std::move(body)};
        follow_up = std::jthread([session, request = std::move(request)](std::stop_token stop) mutable {
            std::mutex wait_mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(wait_mutex);
            if (wake.wait_for(lock, stop, count_tokens_delay, [] { return false; }) || stop.stop_requested()) return;
            try {
                session->send(std::move(request), stop, 15s, 1 << 20);
            } catch (const std::exception &) {
            }
        });
    }

    // The page, and with `program` its settled WAA program.
    void bootstrap(const Account &account, std::string_view model, bool program, std::stop_token token) {
        try {
            if (program) ensure_vm(account, model, token);
            else ensure_page(account, model, token);
        } catch (const Error &error) {
            if (error.code != "cancelled") last_status = "bootstrap_failed";
            throw;
        }
    }
    // The two halves of generate(): a proof for the request's content from
    // the WAA program, and the request sent from the page with a proof.
    struct Content {
        std::string timezone, digest, model, prompt;
        // When the account's page ran the conversation before: the number of
        // its user turns. The page runs it again with Rerun, and the prompt
        // box stays empty.
        std::optional<int> rerun;
    };
    Content content_of(const Request &request) const;
    // The same, with the conversation recorded as run by the account's page.
    Content run_content(const Request &request, const Account &account);
    std::string prove(const Content &content, std::stop_token token);
    // The page replaced the element under the pointer again as the response
    // that answered a click ended.
    void close_answer() noexcept;
    void send(const Request &request, const Account &account, const Content &content, std::string_view proof, const EventSink &sink,
              std::stop_token cancel);
};

SpiderMonkeyGateway::SpiderMonkeyGateway(const Config &config, Store &store) : impl_(std::make_unique<Impl>(config, store)) {}
SpiderMonkeyGateway::~SpiderMonkeyGateway() = default;

SpiderMonkeyGateway::Impl::Content SpiderMonkeyGateway::Impl::content_of(const Request &request) const {
    Content content;
    content.timezone = host.profile.timezone.empty() ? std::string("UTC") : host.profile.timezone;
    auto wire = encode_request(request, {}, "", content.timezone);
    content.digest = browser_detail::wire_content_hash(wire);
    content.model = wire.empty() || !wire[0].is_string() ? std::string() : std::string(wire[0].as_string());
    content.prompt = sm::page_prompt(request);
    return content;
}

SpiderMonkeyGateway::Impl::Content SpiderMonkeyGateway::Impl::run_content(const Request &request, const Account &account) {
    auto content = content_of(request);
    if (page_runs.rerun(account.id, request)) {
        content.rerun = sm::user_turns(request);
        content.prompt.clear();
    }
    return content;
}

void SpiderMonkeyGateway::Impl::close_answer() noexcept {
    if (auto *answered = std::exchange(answering, nullptr); answered && answered == vm.get()) answered->replaced();
}

std::string SpiderMonkeyGateway::Impl::prove(const Content &content, std::stop_token token) {
    // The proof binds the request's text, typed into the prompt box and run
    // with a click on Run. A conversation the page ran before runs again with
    // its last turn's Rerun, once the page has clicked; any other request
    // follows a user action.
    phase = "waa_proof";
    const bool run = !content.prompt.empty(), rerun = !run && content.rerun && page->pointer->clicked();
    running = nullptr;
    std::string proof;
    try {
        if (run) {
            proof = vm->run_proof(content.digest, content.prompt, token);
        } else if (rerun) {
            proof = vm->rerun_proof(content.digest, *content.rerun, token);
        } else {
            vm->activate(token);
            proof = vm->proof(content.digest, content.prompt, token);
        }
    } catch (const Error &error) {
        // A program that failed is not used again.
        if (error.code != "cancelled") vm.reset();
        throw;
    }
    if (!proof.starts_with('!')) {
        vm.reset();
        throw Error(502, "waa_proof_invalid", "The WAA program returned a malformed proof");
    }
    if (run || rerun) running = vm.get();
    return proof;
}

void SpiderMonkeyGateway::Impl::send(const Request &request, const Account &account, const Content &content, std::string_view proof,
                                     const EventSink &sink, std::stop_token cancel) {
    const auto chunk_bytes = std::min<std::size_t>(32768, config.limits.relay_bytes);
    if (!chunk_bytes) throw Error(500, "invalid_config", "Browser relay byte budget must be positive");
    struct Transfer {
        std::unique_ptr<browser_detail::HttpExchange> exchange;
        std::uint64_t sequence = 0;
        bool done = false;
        long status = 0;
        unsigned retry_after = 0;
        std::string error, error_body, benefit = "unknown";
    } transfer;
    bool counted = false;
    // The click the first GenerateContent answers, if the program that made
    // it is still the page's.
    auto *clicked = std::exchange(running, nullptr);
    if (clicked != vm.get()) clicked = nullptr;
    struct CloseAnswer {
        Impl &impl;
        ~CloseAnswer() { impl.close_answer(); }
    } close_answer_at_end{*this};
    browser_detail::GenerationIO io;
    io.encode = [&](std::string_view checkpoint) { return json::serialize(encode_request(request, {}, proof, content.timezone, checkpoint)); };
    io.start = [&](std::string_view body, const std::function<void()> &check) {
        check();
        transfer = {};
        phase = "request_authorization";
        refresh_tier(cancel);
        auto context = rpc_context(true);
        transfer.benefit = fresh_tier();
        context.tier = std::string(sm::tier_header(transfer.benefit));
        if (!counted && !content.prompt.empty() && !content.model.empty()) {
            counted = true;
            schedule_count_tokens(sm::count_tokens_body(content.model, content.prompt), context);
        }
        HttpRequest upload{"POST", std::string(maker_suite) + "GenerateContent", sm::rpc_headers(headers, context), std::string(body)};
        check();
        phase = "generation_start";
        transfer.exchange = http->open(std::move(upload), cancel, 60s);
        // Run turned into Stop; Rerun removed the last turn.
        if (auto *answered = std::exchange(clicked, nullptr)) {
            answered->replaced();
            answering = answered;
        }
        transfer.status = transfer.exchange->status();
        transfer.retry_after = retry_after(transfer.exchange->headers());
        if (transfer.status < 200 || transfer.status >= 300) {
            // At most 8 KiB, for classification only.
            const auto deadline = Clock::now() + 5s;
            while (transfer.error_body.size() < 8192 && Clock::now() < deadline) {
                const auto bytes = transfer.exchange->read(8192 - transfer.error_body.size(), 250ms, cancel);
                if (!bytes) continue;
                if (bytes->empty()) break;
                transfer.error_body += *bytes;
            }
            transfer.exchange.reset();
            close_answer();
            transfer.error = "upstream_http_" + std::to_string(transfer.status);
            transfer.done = true;
        }
        phase = "generation_read";
    };
    io.next = [&](std::uint64_t) -> json::value {
        auto state = sm::relay_state({.done = transfer.done,
                                      .status = transfer.status,
                                      .retry_after = transfer.retry_after,
                                      .error = transfer.error,
                                      .error_body = transfer.error_body,
                                      .benefit = transfer.benefit});
        if (transfer.done || !transfer.exchange) {
            state["done"] = true;
            return state;
        }
        const auto bytes = transfer.exchange->read(chunk_bytes, 250ms, cancel);
        if (!bytes) return state;
        if (bytes->empty()) {
            transfer.exchange.reset();
            close_answer();
            transfer.done = true;
            state["done"] = true;
            return state;
        }
        state["pending"] = sm::relay_chunk(++transfer.sequence, *bytes);
        return state;
    };
    io.completed = [&] {
        phase = "generation_complete";
        if (Clock::now() - cookies_saved_at >= 300s) persist_cookies(account);
    };
    browser_detail::run_generation(request, config.limits, config.continuation_retry_enabled, io, sink, cancel);
    phase = "ready";
}

void SpiderMonkeyGateway::generate(const Request &request, const Account &account, const EventSink &sink, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    auto &impl = *impl_;
    impl.bootstrap(account, request.model, true, cancel);
    const auto content = impl.run_content(request, account);
    const auto proof = impl.prove(content, cancel);
    impl.send(request, account, content, proof, sink, cancel);
}

std::string SpiderMonkeyGateway::experiment_proof(const Request &request, const Account &account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    auto &impl = *impl_;
    impl.bootstrap(account, request.model, true, cancel);
    return impl.prove(impl.content_of(request), cancel);
}

void SpiderMonkeyGateway::experiment_send(const Request &request, const Account &account, std::string_view proof, const EventSink &sink,
                                          std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    auto &impl = *impl_;
    // The page and its RPC context, without starting a program.
    impl.bootstrap(account, request.model, false, cancel);
    impl.send(request, account, impl.content_of(request), proof, sink, cancel);
}

json::array SpiderMonkeyGateway::models(const Account &account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    auto &impl = *impl_;
    impl.ensure_page(account, {}, cancel);
    impl.refresh_tier(cancel);
    auto context = impl.rpc_context(true);
    context.tier = std::string(sm::tier_header(impl.fresh_tier()));
    const auto response = impl.rpc("ListModels", "[]", context, cancel, 15s);
    if (auto failure = sm::page_rpc_failure("ListModels", response.status, response.body)) throw *failure;
    auto models = sm::discovered_models(response.body);
    impl.models_discovered = true;
    return models;
}

json::object SpiderMonkeyGateway::probe(const Account &account, std::stop_token cancel) {
    std::lock_guard lock(impl_->mutex);
    auto &impl = *impl_;
    impl.ensure_page(account, {}, cancel);
    const auto &email = impl.page->email;
    return {{"accountId", account.id}, {"email", email}, {"ready", true}, {"login", false}, {"identityReady", !email.empty()},
            {"identityAmbiguous", false}, {"status", email.empty() ? "identity_unverified" : "ready"}, {"generationVerified", false}};
}

json::object SpiderMonkeyGateway::diagnostics() {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {{"status", "busy"}, {"phase", "active"}, {"backend", "spidermonkey"}};
    const auto &impl = *impl_;
    json::value vm = nullptr;
    if (impl.vm)
        vm = json::object{{"ageSeconds", std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - impl.vm_started).count()},
                          {"interpreter", impl.interpreter_hash.substr(0, 12)}};
    json::value identity = nullptr;
    if (impl.http) identity = json::object{{"platform", impl.platform}, {"userAgent", impl.headers.user_agent}};
    return {{"status", impl.last_status}, {"phase", impl.phase}, {"backend", "spidermonkey"},
            {"running", impl.vm != nullptr}, {"pageLoaded", impl.page.has_value()}, {"vm", std::move(vm)},
            {"vmsStarted", impl.vms_started}, {"claimedFirefox", claimed_firefox}, {"host", waa::host_versions()},
            // Below HTTP, requests carry Firefox's fingerprints only in a
            // build with curl-impersonate.
            {"network", HttpSession::firefox_fingerprint() ? "curl-impersonate" : "libcurl"},
            {"networkStack", HttpSession::network_stack()}, {"benefitTier", impl.fresh_tier()},
            {"modelDiscovery", impl.models_discovered ? "live_rpc" : "not_loaded"}, {"cookieRefresh", impl.cookie_refresh},
            {"proxy", json::object{{"mode", impl.config.proxy.empty() ? "none" : "direct"}}}, {"identity", std::move(identity)}};
}

void SpiderMonkeyGateway::reconfigure(const Config &config) {
    std::lock_guard lock(impl_->mutex);
    impl_->config.model_aliases = config.model_aliases;
    impl_->config.model_defaults = config.model_defaults;
    impl_->config.continuation_retry_enabled = config.continuation_retry_enabled;
    impl_->config.limits.max_continuation_retries = config.limits.max_continuation_retries;
}

void SpiderMonkeyGateway::close() {
    std::lock_guard lock(impl_->mutex);
    impl_->close();
}

void SpiderMonkeyGateway::reset_session() {
    std::lock_guard lock(impl_->mutex);
    // A new page and program on the same connections and cookies.
    impl_->drop_page();
    impl_->last_status = "session_reset";
    impl_->phase = "idle";
}

bool SpiderMonkeyGateway::release_idle() {
    std::lock_guard lock(impl_->mutex);
    auto &impl = *impl_;
    if (!impl.http) return false;
    if (!impl.account_id.empty()) try {
            impl.persist_cookies(impl.store.account(impl.account_id));
        } catch (...) {
        }
    impl.close();
    impl.last_status = "idle_closed";
    return true;
}
} // namespace ais2api
