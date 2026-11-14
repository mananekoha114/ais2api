// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The HTTP side of a page that is not there: one account's connections,
// cookies and request headers, for the backend that talks to Google without
// a browser. Built with AIS2API_CURL_IMPERSONATE, the transport is
// curl-impersonate's libcurl, set to Firefox's TLS ClientHello and HTTP/2
// framing; otherwise it is plain libcurl, with libcurl's fingerprints.
#pragma once
#include "ais2api/common.hpp"
#include <memory>

namespace ais2api::browser_detail {
// The cookies a request to `url` carries, as name=value pairs in the order of
// the Cookie header, from cookies in the account store's shape. Expired
// cookies are left out.
std::vector<std::string> cookie_pairs(const json::array &stored, std::string_view url, std::int64_t now = unix_seconds());
// The Cookie header a request to `url` carries; empty without any cookies.
std::string cookie_header(const json::array &stored, std::string_view url, std::int64_t now = unix_seconds());
// Applies one Set-Cookie header of a response from `url`. Returns whether the
// cookies changed.
bool merge_set_cookie(json::array &stored, std::string_view header, std::string_view url, std::int64_t now = unix_seconds());

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;
// The first value of a header, compared without case; empty when absent.
std::string header_value(const HttpHeaders &headers, std::string_view name);

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    // Sent in this order, with nothing added but Host and, without one here,
    // Content-Length. A Cookie header with an empty value stands for the
    // session's cookies and is left out when there are none; with Firefox's
    // fingerprints, an https request carries each cookie in a header of its
    // own, as Firefox does over HTTP/2. A Content-Length header with an empty
    // value stands for the body's length, so that it can be placed.
    HttpHeaders headers;
    std::optional<std::string> body;
    // The HTTP/2 priority weight of the request's stream. Firefox gives the
    // top-level document 42, a script, XHR or fetch of normal priority 22 and
    // an image of urgency 5 12.
    int weight = 22;
    // Through a proxy, records when the proxy answered the CONNECT of a new
    // connection (libcurl's verbose trace, which costs a little).
    bool trace_tunnel = false;
};
// How a response's connection came about, by libcurl's timers.
struct HttpTiming {
    // Whether the request opened a connection rather than reusing one; only
    // then are the stages below its own.
    bool new_connection = false;
    // From the request's start: the name resolved, the TCP connection (to the
    // proxy, when there is one) made and, with HttpRequest::trace_tunnel, the
    // proxy's answer to CONNECT received. Zero for a stage that did not
    // happen.
    std::chrono::microseconds resolved{}, connected{}, tunneled{};
};
struct HttpResponse {
    long status = 0;
    HttpHeaders headers;
    std::string body;
    // The protocol's ALPN name: "h2", "http/1.1" or "h3".
    std::string protocol;
    HttpTiming timing;
};

// A response as it arrives. Destroying it abandons the transfer.
class HttpExchange {
  public:
    virtual ~HttpExchange() = default;
    virtual long status() const = 0;
    virtual const HttpHeaders &headers() const = 0;
    // The next bytes of the body, at most `limit`: nothing when none arrived
    // within `wait`, an empty string once the body is complete. A consumer
    // that stops reading stops the transfer after a bounded buffer.
    virtual std::optional<std::string> read(std::size_t limit, std::chrono::milliseconds wait, std::stop_token cancel = {}) = 0;
};

// Connections are reused and, over HTTP/2, shared by concurrent requests.
// Redirects are not followed. Failures are 502 proxy_failed or network_failed,
// 504 upstream_timeout, or 499 cancelled. Safe to use from several threads.
class HttpSession {
  public:
    struct Options {
        // One proxy URL; empty connects directly, whatever the environment says.
        std::string proxy;
        json::array cookies;
        std::filesystem::path ca_file;
        // What URLs may name; tests add "http".
        std::string protocols = "https";
    };
    explicit HttpSession(Options options);
    ~HttpSession();
    HttpSession(const HttpSession &) = delete;
    HttpSession &operator=(const HttpSession &) = delete;

    // Sends the request and returns once the response's headers arrived.
    std::unique_ptr<HttpExchange> open(HttpRequest request, std::stop_token cancel = {},
                                       std::chrono::milliseconds timeout = std::chrono::seconds(60));
    // The whole response; 502 network_failed when its body exceeds `limit`.
    HttpResponse send(HttpRequest request, std::stop_token cancel = {}, std::chrono::milliseconds timeout = std::chrono::seconds(60),
                      std::size_t limit = 64 * 1024 * 1024);

    json::array cookies() const;
    // Whether a response changed the cookies since the last call.
    bool take_cookie_change();
    // The content codings this build decodes, as Firefox lists them.
    static std::string accept_encoding();
    // Whether requests carry Firefox's TLS and HTTP/2 fingerprints.
    static bool firefox_fingerprint();
    // The libcurl, TLS library and HTTP/2 library this build sends with.
    static std::string network_stack();

    // The implementation, which the exchanges of a session share with it.
    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};
} // namespace ais2api::browser_detail
