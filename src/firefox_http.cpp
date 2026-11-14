// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../browser/firefox_http.hpp"
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <curl/curl.h>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace ais2api::browser_detail {
namespace {
using namespace std::chrono_literals;

// A stalled consumer holds at most this much of a body; the transfer resumes
// once it has read down to the lower mark.
constexpr std::size_t pause_bytes = 256 * 1024, resume_bytes = 64 * 1024;

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto &character : out)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return out;
}
std::string_view trim(std::string_view text) {
    constexpr std::string_view blank = " \t\r\n";
    const auto begin = text.find_first_not_of(blank);
    if (begin == std::string_view::npos) return {};
    return text.substr(begin, text.find_last_not_of(blank) - begin + 1);
}
bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && lower(a) == lower(b);
}
struct Address {
    std::string scheme, host, path;
};
// The parts of an absolute URL that decide which cookies it carries.
std::optional<Address> split_address(std::string_view url) {
    const auto colon = url.find("://");
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    Address address;
    address.scheme = lower(url.substr(0, colon));
    auto rest = url.substr(colon + 3);
    const auto end = rest.find_first_of("/?#");
    auto authority = rest.substr(0, end);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        authority = authority.substr(0, close + 1);
    } else if (const auto port = authority.rfind(':'); port != std::string_view::npos) {
        authority = authority.substr(0, port);
    }
    if (authority.empty()) return std::nullopt;
    address.host = lower(authority);
    address.path = "/";
    if (end != std::string_view::npos && rest[end] == '/') {
        const auto tail = rest.substr(end);
        address.path = std::string(tail.substr(0, tail.find_first_of("?#")));
    }
    return address;
}
std::string bare_domain(std::string_view domain) {
    if (!domain.empty() && domain.front() == '.') domain.remove_prefix(1);
    return lower(domain);
}
bool domain_matches(std::string_view host, std::string_view domain) {
    return !domain.empty() && (host == domain || (host.size() > domain.size() && host.ends_with(domain) &&
                                                  host[host.size() - domain.size() - 1] == '.'));
}
bool path_matches(std::string_view request, std::string_view cookie) {
    if (cookie.empty() || cookie == "/" || request == cookie) return true;
    return request.starts_with(cookie) && (cookie.back() == '/' || request[cookie.size()] == '/');
}
std::optional<double> expiry_of(const json::object &cookie) {
    const auto value = cookie.if_contains("expires");
    if (!value || !value->is_number()) return std::nullopt;
    const auto expires = value->to_number<double>();
    // The store writes no expiry, or a value that is not positive, for a
    // session cookie.
    return expires > 0 ? std::optional(expires) : std::nullopt;
}
struct CurlInit {
    CurlInit() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT)) throw Error(500, "network_init", "Cannot initialize the HTTP client");
    }
};
void initialise_curl() { static CurlInit init; }

Error transport_error(CURLcode code, std::string_view detail, bool proxied) {
    std::string message = curl_easy_strerror(code);
    if (!detail.empty()) message += ": " + std::string(detail);
    // With a proxy, the connection that failed to open is the one to it.
    if (proxied && code == CURLE_COULDNT_CONNECT) code = CURLE_PROXY;
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_PROXY: return Error(502, "proxy_failed", "Could not reach Google through the configured proxy (" + message + ")");
    case CURLE_OPERATION_TIMEDOUT: return Error(504, "upstream_timeout", "Google did not answer in time (" + message + ")");
    default: return Error(502, "network_failed", "Could not reach Google (" + message + ")");
    }
}

#ifdef AIS2API_CURL_IMPERSONATE
// Firefox's TLS ClientHello and HTTP/2 connection preface, as measured from
// Camoufox 158 on Windows (docs/reports/firefox-network-fingerprint-2026-11-12.md).
// Set value by value rather than through curl-impersonate's own Firefox
// target, whose groups add FFDHE that Firefox does not offer.
namespace firefox {
// TLS 1.3 first, in Firefox's order: AES-128, ChaCha20, AES-256.
constexpr const char *ciphers =
    "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384:"
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:"
    "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA";
// The first three also get a key share.
constexpr const char *groups = "X25519MLKEM768:X25519:P-256:P-384:P-521";
constexpr long key_shares = 3;
constexpr const char *signature_algorithms =
    "ecdsa_secp256r1_sha256:ecdsa_secp384r1_sha384:ecdsa_secp521r1_sha512:rsa_pss_rsae_sha256:rsa_pss_rsae_sha384:"
    "rsa_pss_rsae_sha512:rsa_pkcs1_sha256:rsa_pkcs1_sha384:rsa_pkcs1_sha512:ecdsa_sha1:rsa_pkcs1_sha1";
constexpr const char *delegated_credentials = "ecdsa_secp256r1_sha256:ecdsa_secp384r1_sha384:ecdsa_secp521r1_sha512:ecdsa_sha1";
constexpr const char *certificate_compression = "zlib,brotli,zstd";
constexpr long record_size_limit = 16385;
// server_name, extended_master_secret, renegotiation_info, supported_groups,
// ec_point_formats, session_ticket, alpn, status_request,
// delegated_credentials, signed_certificate_timestamp, key_share,
// supported_versions, signature_algorithms, psk_key_exchange_modes,
// record_size_limit, compress_certificate, encrypted_client_hello. No GREASE
// and no shuffling.
constexpr const char *extension_order = "0-23-65281-10-11-35-16-5-34-18-51-43-13-45-28-27-65037";
// Without an ECH configuration for the host, Firefox sends a GREASE one.
constexpr const char *ech = "true";
// HEADER_TABLE_SIZE, ENABLE_PUSH, INITIAL_WINDOW_SIZE, MAX_FRAME_SIZE.
constexpr const char *http2_settings = "1:65536;2:0;4:131072;5:16384";
constexpr long http2_connection_window_update = 12517377;
constexpr const char *http2_pseudo_headers = "mpas";
} // namespace firefox

void set(CURL *easy, CURLoption option, long value, const char *name) {
    if (curl_easy_setopt(easy, option, value) != CURLE_OK)
        throw Error(500, "network_init", std::string("The HTTP client rejected Firefox's ") + name);
}
void set(CURL *easy, CURLoption option, const char *value, const char *name) {
    if (curl_easy_setopt(easy, option, value) != CURLE_OK)
        throw Error(500, "network_init", std::string("The HTTP client rejected Firefox's ") + name);
}
void apply_firefox(CURL *easy) {
    set(easy, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_DEFAULT), "TLS versions");
    set(easy, CURLOPT_SSL_CIPHER_LIST, firefox::ciphers, "cipher suites");
    set(easy, CURLOPT_SSL_EC_CURVES, firefox::groups, "key exchange groups");
    set(easy, CURLOPT_TLS_KEY_SHARES_LIMIT, firefox::key_shares, "key shares");
    set(easy, CURLOPT_SSL_SIGNATURE_ALGORITHMS, firefox::signature_algorithms, "signature algorithms");
    set(easy, CURLOPT_TLS_DELEGATED_CREDENTIALS, firefox::delegated_credentials, "delegated credentials");
    set(easy, CURLOPT_SSL_CERT_COMPRESSION, firefox::certificate_compression, "certificate compression");
    set(easy, CURLOPT_TLS_RECORD_SIZE_LIMIT, firefox::record_size_limit, "record size limit");
    set(easy, CURLOPT_TLS_EXTENSION_ORDER, firefox::extension_order, "extension order");
    set(easy, CURLOPT_TLS_GREASE, 0L, "GREASE setting");
    set(easy, CURLOPT_SSL_PERMUTE_EXTENSIONS, 0L, "extension order setting");
    set(easy, CURLOPT_SSL_ENABLE_ALPN, 1L, "ALPN");
    set(easy, CURLOPT_SSL_ENABLE_ALPS, 0L, "ALPS setting");
    set(easy, CURLOPT_SSL_ENABLE_TICKET, 1L, "session tickets");
    set(easy, CURLOPT_TLS_SIGNED_CERT_TIMESTAMPS, 1L, "certificate timestamps");
    set(easy, CURLOPT_TLS_STATUS_REQUEST, 1L, "status request");
    set(easy, CURLOPT_ECH, firefox::ech, "encrypted client hello");
    set(easy, CURLOPT_HTTP2_SETTINGS, firefox::http2_settings, "HTTP/2 settings");
    set(easy, CURLOPT_HTTP2_WINDOW_UPDATE, firefox::http2_connection_window_update, "HTTP/2 window");
    set(easy, CURLOPT_HTTP2_PSEUDO_HEADERS_ORDER, firefox::http2_pseudo_headers, "pseudo-header order");
    set(easy, CURLOPT_STREAM_EXCLUSIVE, 0L, "stream dependency");
    // Cookies come from the session's jar, not curl's, so CURLOPT_SPLIT_COOKIES
    // does not apply: prepare() splits them itself.
}
#endif
} // namespace

std::string header_value(const HttpHeaders &headers, std::string_view name) {
    for (const auto &[key, value] : headers)
        if (same_name(key, name)) return value;
    return {};
}

std::vector<std::string> cookie_pairs(const json::array &stored, std::string_view url, std::int64_t now) {
    const auto address = split_address(url);
    if (!address) return {};
    std::vector<const json::object *> matched;
    for (const auto &value : stored) {
        if (!value.is_object()) continue;
        const auto &cookie = value.as_object();
        if (string_or(cookie, "name").empty() || !domain_matches(address->host, bare_domain(string_or(cookie, "domain")))) continue;
        if (!path_matches(address->path, string_or(cookie, "path"))) continue;
        if (bool_or(cookie, "secure") && address->scheme != "https") continue;
        if (const auto expires = expiry_of(cookie); expires && *expires <= static_cast<double>(now)) continue;
        matched.push_back(&cookie);
    }
    // Longer paths first; otherwise the order they were stored in.
    std::stable_sort(matched.begin(), matched.end(), [](const json::object *a, const json::object *b) {
        return string_or(*a, "path").size() > string_or(*b, "path").size();
    });
    std::vector<std::string> pairs;
    for (const auto *cookie : matched) pairs.push_back(string_or(*cookie, "name") + "=" + string_or(*cookie, "value"));
    return pairs;
}

std::string cookie_header(const json::array &stored, std::string_view url, std::int64_t now) {
    std::string header;
    for (const auto &pair : cookie_pairs(stored, url, now)) header += (header.empty() ? "" : "; ") + pair;
    return header;
}

bool merge_set_cookie(json::array &stored, std::string_view header, std::string_view url, std::int64_t now) {
    const auto address = split_address(url);
    if (!address) return false;
    const auto first = header.substr(0, header.find(';'));
    const auto equals = first.find('=');
    if (equals == std::string_view::npos) return false;
    const auto name = trim(first.substr(0, equals)), value = trim(first.substr(equals + 1));
    if (name.empty()) return false;

    std::string domain, path, same_site;
    std::optional<std::int64_t> expires, max_age;
    bool secure = false, http_only = false;
    auto rest = header.size() > first.size() ? header.substr(first.size() + 1) : std::string_view{};
    while (!rest.empty()) {
        const auto end = rest.find(';');
        const auto attribute = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const auto split = attribute.find('=');
        const auto key = lower(trim(attribute.substr(0, split)));
        const auto text = split == std::string_view::npos ? std::string_view{} : trim(attribute.substr(split + 1));
        if (key == "domain") {
            domain = bare_domain(text);
        } else if (key == "path") {
            if (text.starts_with('/')) path = std::string(text);
        } else if (key == "expires") {
            if (const auto time = curl_getdate(std::string(text).c_str(), nullptr); time != -1) expires = static_cast<std::int64_t>(time);
        } else if (key == "max-age") {
            std::int64_t seconds = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size()) max_age = seconds;
        } else if (key == "secure") {
            secure = true;
        } else if (key == "httponly") {
            http_only = true;
        } else if (key == "samesite") {
            const auto mode = lower(text);
            same_site = mode == "strict" ? "Strict" : mode == "lax" ? "Lax" : mode == "none" ? "None" : "";
        }
    }
    // A cookie may only name the host that set it or a parent domain of it.
    if (!domain.empty() && !domain_matches(address->host, domain)) return false;
    const auto stored_domain = domain.empty() ? address->host : "." + domain;
    if (path.empty()) {
        const auto slash = address->path.rfind('/');
        path = slash == 0 || slash == std::string::npos ? "/" : address->path.substr(0, slash);
    }
    // Max-Age wins over Expires.
    if (max_age) expires = *max_age <= 0 ? 0 : now + *max_age;
    const bool expired = expires && *expires <= now;

    const auto found = std::find_if(stored.begin(), stored.end(), [&](const json::value &item) {
        if (!item.is_object()) return false;
        const auto &cookie = item.as_object();
        const auto cookie_path = string_or(cookie, "path");
        return string_or(cookie, "name") == name && bare_domain(string_or(cookie, "domain")) == bare_domain(stored_domain) &&
               (cookie_path.empty() ? "/" : cookie_path) == path;
    });
    if (expired) {
        if (found == stored.end()) return false;
        stored.erase(found);
        return true;
    }
    json::object cookie{{"name", name}, {"value", value}, {"domain", stored_domain}, {"path", path}, {"secure", secure}, {"httpOnly", http_only}};
    if (!same_site.empty()) cookie["sameSite"] = same_site;
    if (expires) cookie["expires"] = *expires;
    if (found == stored.end()) {
        stored.push_back(std::move(cookie));
        return true;
    }
    if (found->as_object() == cookie) return false;
    *found = std::move(cookie);
    return true;
}

namespace {
// One request and what has arrived of its response. The session's thread
// owns the curl handle; the mutex guards everything a reader sees.
struct Transfer {
    HttpRequest request;
    CURL *easy = nullptr;
    curl_slist *header_list = nullptr;
    char error[CURL_ERROR_SIZE] = {};
    bool proxied = false;
    std::function<void(std::string_view)> set_cookie;

    // What ended the transfer early; call with the mutex held.
    Error failure() const { return transport_error(result, error, proxied); }

    std::mutex mutex;
    std::condition_variable_any changed;
    long status = 0;
    HttpHeaders headers;
    bool headers_complete = false, finished = false, paused = false;
    CURLcode result = CURLE_OK;
    std::deque<std::string> chunks;
    std::size_t consumed = 0; // of the first chunk
    std::size_t buffered = 0;

    ~Transfer() {
        if (easy) curl_easy_cleanup(easy);
        curl_slist_free_all(header_list);
    }
    static std::size_t on_header(char *data, std::size_t size, std::size_t count, void *self) {
        static_cast<Transfer *>(self)->header(std::string_view(data, size * count));
        return size * count;
    }
    static std::size_t on_body(char *data, std::size_t size, std::size_t count, void *self) {
        return static_cast<Transfer *>(self)->body(std::string_view(data, size * count));
    }
    void header(std::string_view line) {
        line = trim(line);
        std::unique_lock lock(mutex);
        if (line.starts_with("HTTP/")) {
            // A new head: an interim response's headers are not the answer's.
            headers.clear();
            status = 0;
            if (const auto space = line.find(' '); space != std::string_view::npos) {
                const auto code = line.substr(space + 1, 3);
                std::from_chars(code.data(), code.data() + code.size(), status);
            }
            return;
        }
        if (line.empty()) {
            if (status >= 200) {
                headers_complete = true;
                lock.unlock();
                changed.notify_all();
            }
            return;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) return;
        std::string name(trim(line.substr(0, colon))), value(trim(line.substr(colon + 1)));
        const bool cookie = same_name(name, "set-cookie");
        headers.emplace_back(std::move(name), value);
        lock.unlock();
        if (cookie && set_cookie) set_cookie(value);
    }
    std::size_t body(std::string_view bytes) {
        {
            std::lock_guard lock(mutex);
            if (buffered >= pause_bytes) {
                // curl hands these bytes over again when the transfer resumes.
                paused = true;
                return CURL_WRITEFUNC_PAUSE;
            }
            chunks.emplace_back(bytes);
            buffered += bytes.size();
        }
        changed.notify_all();
        return bytes.size();
    }
    void finish(CURLcode code) {
        {
            std::lock_guard lock(mutex);
            if (finished) return;
            finished = true;
            result = code;
        }
        changed.notify_all();
    }
};
} // namespace

struct HttpSession::Impl : std::enable_shared_from_this<HttpSession::Impl> {
    explicit Impl(Options given) : options(std::move(given)), jar(std::move(options.cookies)) {
        initialise_curl();
        multi = curl_multi_init();
        if (!multi) throw Error(500, "network_init", "Cannot allocate the HTTP client");
        // One connection per origin carries concurrent requests when the
        // server speaks HTTP/2; HTTP/1.1 gets Firefox's six.
        curl_multi_setopt(multi, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_MULTIPLEX));
        curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~Impl() {
        worker.request_stop();
        curl_multi_wakeup(multi);
        worker.join();
        curl_multi_cleanup(multi);
    }

    // Runs `command` on the session's thread; dropped once it has stopped.
    void post(std::function<void()> command) {
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            commands.push_back(std::move(command));
        }
        curl_multi_wakeup(multi);
    }
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::deque<std::function<void()>> ready;
            {
                std::lock_guard lock(mutex);
                ready.swap(commands);
            }
            for (auto &command : ready) command();
            int running = 0;
            curl_multi_perform(multi, &running);
            int left = 0;
            while (const auto message = curl_multi_info_read(multi, &left))
                if (message->msg == CURLMSG_DONE) complete(message->easy_handle, message->data.result);
            curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
        }
        {
            std::lock_guard lock(mutex);
            stopped = true;
            commands.clear();
        }
        while (!active.empty()) complete(active.begin()->first, CURLE_ABORTED_BY_CALLBACK);
    }
    void complete(CURL *easy, CURLcode code) {
        const auto found = active.find(easy);
        if (found == active.end()) return;
        const auto transfer = std::move(found->second);
        active.erase(found);
        curl_multi_remove_handle(multi, easy);
        transfer->finish(code);
    }
    void start(const std::shared_ptr<Transfer> &transfer) {
        if (const auto code = curl_multi_add_handle(multi, transfer->easy); code != CURLM_OK) {
            transfer->finish(CURLE_FAILED_INIT);
            return;
        }
        active.emplace(transfer->easy, transfer);
    }
    void abandon(const std::shared_ptr<Transfer> &transfer) {
        post([this, transfer] { complete(transfer->easy, CURLE_ABORTED_BY_CALLBACK); });
    }
    void resume(const std::shared_ptr<Transfer> &transfer) {
        post([this, transfer] {
            if (!active.contains(transfer->easy)) return;
            {
                std::lock_guard lock(transfer->mutex);
                transfer->paused = false;
            }
            curl_easy_pause(transfer->easy, CURLPAUSE_CONT);
        });
    }
    std::shared_ptr<Transfer> prepare(HttpRequest request, std::chrono::milliseconds total);

    Options options;
    CURLM *multi = nullptr;
    std::mutex mutex;
    std::deque<std::function<void()>> commands;
    bool stopped = false;
    // The session's thread only.
    std::map<CURL *, std::shared_ptr<Transfer>> active;
    std::mutex jar_mutex;
    json::array jar;
    bool jar_changed = false;
    std::jthread worker;
};

std::shared_ptr<Transfer> HttpSession::Impl::prepare(HttpRequest request, std::chrono::milliseconds total) {
    auto transfer = std::make_shared<Transfer>();
    transfer->request = std::move(request);
    const auto &given = transfer->request;
    transfer->easy = curl_easy_init();
    if (!transfer->easy) throw Error(500, "network_init", "Cannot allocate an HTTP request");
    const auto easy = transfer->easy;
    transfer->proxied = !options.proxy.empty();
    transfer->set_cookie = [this, url = given.url](std::string_view header) {
        std::lock_guard lock(jar_mutex);
        if (merge_set_cookie(jar, header, url)) jar_changed = true;
    };

    bool accept = false, accept_encoding = false, content_type = false;
    const bool split_cookies = HttpSession::firefox_fingerprint() && lower(given.url).starts_with("https://");
    const auto add = [&](const std::string &line) {
        const auto list = curl_slist_append(transfer->header_list, line.c_str());
        if (!list) throw Error(500, "network_init", "Cannot allocate an HTTP request");
        transfer->header_list = list;
    };
    for (const auto &[name, value] : given.headers) {
        accept = accept || same_name(name, "accept");
        accept_encoding = accept_encoding || same_name(name, "accept-encoding");
        content_type = content_type || same_name(name, "content-type");
        if (same_name(name, "cookie") && value.empty()) {
            std::lock_guard lock(jar_mutex);
            // Over HTTP/2, which Google always negotiates, Firefox sends each
            // cookie in a header of its own; over HTTP/1.1, one header.
            if (split_cookies) {
                for (const auto &pair : cookie_pairs(jar, given.url)) add(name + ": " + pair);
            } else if (const auto cookies = cookie_header(jar, given.url); !cookies.empty()) {
                add(name + ": " + cookies);
            }
            continue;
        }
        if (same_name(name, "content-length") && value.empty()) {
            add(name + ": " + std::to_string(given.body ? given.body->size() : 0));
            continue;
        }
        add(name + ": " + value);
    }
    // Nothing of curl's own: no default Accept or Accept-Encoding, no form
    // content type, no Expect: 100-continue.
    if (!accept) add("Accept:");
    if (!accept_encoding) add("Accept-Encoding:");
    if (given.body && !content_type) add("Content-Type:");
    add("Expect:");

    curl_easy_setopt(easy, CURLOPT_URL, given.url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, options.protocols.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->header_list);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
#ifdef AIS2API_CURL_IMPERSONATE
    apply_firefox(easy);
#endif
    curl_easy_setopt(easy, CURLOPT_STREAM_WEIGHT, static_cast<long>(given.weight));
    curl_easy_setopt(easy, CURLOPT_PIPEWAIT, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    // Decodes what the response is encoded with; the Accept-Encoding header
    // itself is the caller's.
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    // The configured proxy, or none: the environment's proxy variables do
    // not apply to an account's traffic.
    curl_easy_setopt(easy, CURLOPT_PROXY, options.proxy.c_str());
    curl_easy_setopt(easy, CURLOPT_NOPROXY, "");
    curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 20L);
    if (total.count() > 0) curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(total.count()));
    if (!options.ca_file.empty()) curl_easy_setopt(easy, CURLOPT_CAINFO, options.ca_file.string().c_str());
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, transfer->error);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &Transfer::on_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, transfer.get());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Transfer::on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
    if (given.body) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, given.body->data());
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(given.body->size()));
    }
    if (given.method == "HEAD") curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    else if (given.method != (given.body ? "POST" : "GET")) curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, given.method.c_str());
    return transfer;
}

namespace {
class Exchange final : public HttpExchange {
  public:
    Exchange(std::shared_ptr<HttpSession::Impl> session, std::shared_ptr<Transfer> transfer)
        : session_(std::move(session)), transfer_(std::move(transfer)) {}
    ~Exchange() override { session_->abandon(transfer_); }
    long status() const override { return status_; }
    const HttpHeaders &headers() const override { return headers_; }

    // Waits for the response's headers.
    void await_headers(std::stop_token cancel, std::chrono::milliseconds timeout) {
        auto &transfer = *transfer_;
        std::unique_lock lock(transfer.mutex);
        const auto arrived = [&] { return transfer.headers_complete || transfer.finished; };
        const bool in_time = timeout.count() > 0 ? transfer.changed.wait_for(lock, cancel, timeout, arrived)
                                                 : (transfer.changed.wait(lock, cancel, arrived), true);
        if (!arrived()) {
            lock.unlock();
            check_cancel(cancel);
            if (!in_time) throw Error(504, "upstream_timeout", "Google did not answer in time");
        }
        if (transfer.finished && transfer.result != CURLE_OK) throw transfer.failure();
        status_ = transfer.status;
        headers_ = transfer.headers;
    }
    std::optional<std::string> read(std::size_t limit, std::chrono::milliseconds wait, std::stop_token cancel) override {
        auto &transfer = *transfer_;
        std::unique_lock lock(transfer.mutex);
        transfer.changed.wait_for(lock, cancel, wait, [&] { return !transfer.chunks.empty() || transfer.finished; });
        if (transfer.chunks.empty()) {
            if (!transfer.finished) {
                lock.unlock();
                check_cancel(cancel);
                return std::nullopt;
            }
            if (transfer.result != CURLE_OK) throw transfer.failure();
            return std::string();
        }
        std::string bytes;
        while (!transfer.chunks.empty() && bytes.size() < limit) {
            auto &front = transfer.chunks.front();
            const auto take = std::min(limit - bytes.size(), front.size() - transfer.consumed);
            bytes.append(front, transfer.consumed, take);
            transfer.consumed += take;
            if (transfer.consumed == front.size()) {
                transfer.chunks.pop_front();
                transfer.consumed = 0;
            }
        }
        transfer.buffered -= bytes.size();
        const bool resume = transfer.paused && transfer.buffered <= resume_bytes;
        lock.unlock();
        if (resume) session_->resume(transfer_);
        return bytes;
    }

  private:
    std::shared_ptr<HttpSession::Impl> session_;
    std::shared_ptr<Transfer> transfer_;
    long status_ = 0;
    HttpHeaders headers_;
};
} // namespace

HttpSession::HttpSession(Options options) : impl_(std::make_shared<Impl>(std::move(options))) {}
HttpSession::~HttpSession() = default;

std::unique_ptr<HttpExchange> HttpSession::open(HttpRequest request, std::stop_token cancel, std::chrono::milliseconds timeout) {
    check_cancel(cancel);
    // The body may take as long as its reader allows; only the wait for the
    // headers is bounded here.
    auto transfer = impl_->prepare(std::move(request), 0ms);
    auto exchange = std::make_unique<Exchange>(impl_, transfer);
    impl_->post([impl = impl_.get(), transfer] { impl->start(transfer); });
    exchange->await_headers(cancel, timeout);
    return exchange;
}

HttpResponse HttpSession::send(HttpRequest request, std::stop_token cancel, std::chrono::milliseconds timeout, std::size_t limit) {
    check_cancel(cancel);
    auto transfer = impl_->prepare(std::move(request), timeout);
    Exchange exchange(impl_, transfer);
    impl_->post([impl = impl_.get(), transfer] { impl->start(transfer); });
    exchange.await_headers(cancel, 0ms);
    HttpResponse response{exchange.status(), exchange.headers(), {}};
    for (;;) {
        const auto bytes = exchange.read(pause_bytes, 1000ms, cancel);
        if (!bytes) continue;
        if (bytes->empty()) return response;
        if (response.body.size() + bytes->size() > limit) throw Error(502, "network_failed", "Google sent a response larger than expected");
        response.body += *bytes;
    }
}

json::array HttpSession::cookies() const {
    std::lock_guard lock(impl_->jar_mutex);
    return impl_->jar;
}

bool HttpSession::take_cookie_change() {
    std::lock_guard lock(impl_->jar_mutex);
    return std::exchange(impl_->jar_changed, false);
}

std::string HttpSession::accept_encoding() {
    initialise_curl();
    const auto features = curl_version_info(CURLVERSION_NOW)->features;
    std::string codings;
    if (features & CURL_VERSION_LIBZ) codings = "gzip, deflate";
    if (features & CURL_VERSION_BROTLI) codings += codings.empty() ? "br" : ", br";
    if (features & CURL_VERSION_ZSTD) codings += codings.empty() ? "zstd" : ", zstd";
    return codings;
}

bool HttpSession::firefox_fingerprint() {
#ifdef AIS2API_CURL_IMPERSONATE
    return true;
#else
    return false;
#endif
}

std::string HttpSession::network_stack() {
    initialise_curl();
    const auto *info = curl_version_info(CURLVERSION_NOW);
    std::string stack = "libcurl/" + std::string(info->version);
    if (info->ssl_version) stack += " " + std::string(info->ssl_version);
    if (info->nghttp2_version) stack += " nghttp2/" + std::string(info->nghttp2_version);
    return stack;
}
} // namespace ais2api::browser_detail
