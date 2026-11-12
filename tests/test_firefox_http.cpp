// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../browser/firefox_http.hpp"
#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <atomic>
#include <future>
#include <thread>

using namespace ais2api;
using namespace std::chrono_literals;
using browser_detail::cookie_header;
using browser_detail::header_value;
using browser_detail::HttpRequest;
using browser_detail::HttpSession;
using browser_detail::merge_set_cookie;
using Catch::Matchers::ContainsSubstring;
namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// A loopback HTTP/1.1 server that hands each request, as received, to a
// handler writing the response.
struct Server {
    struct Request {
        std::string head, body;
        // The header lines after the request line.
        std::vector<std::string> lines() const {
            std::vector<std::string> result;
            std::size_t at = head.find("\r\n");
            while (at != std::string::npos && at + 2 < head.size()) {
                const auto end = head.find("\r\n", at + 2);
                result.push_back(head.substr(at + 2, end - at - 2));
                at = end;
            }
            return result;
        }
        std::string request_line() const { return head.substr(0, head.find("\r\n")); }
    };
    using Handler = std::function<void(const Request &, tcp::socket &)>;

    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    unsigned short port = acceptor.local_endpoint().port();
    Handler handler;
    std::mutex mutex;
    std::vector<Request> requests;
    std::vector<std::jthread> connections;
    std::atomic<bool> stopping{false};
    std::jthread accepting;

    explicit Server(Handler given) : handler(std::move(given)) {
        accepting = std::jthread([this] {
            for (;;) {
                boost::system::error_code failed;
                tcp::socket socket(io);
                acceptor.accept(socket, failed);
                if (failed || stopping) return;
                std::lock_guard lock(mutex);
                connections.emplace_back([this, socket = std::move(socket)]() mutable { serve(socket); });
            }
        });
    }
    ~Server() {
        // A connection of its own wakes the accepting thread.
        stopping = true;
        boost::system::error_code ignored;
        tcp::socket wake(io);
        wake.connect({asio::ip::address_v4::loopback(), port}, ignored);
        accepting.join();
        acceptor.close(ignored);
        std::lock_guard lock(mutex);
        connections.clear();
    }
    void serve(tcp::socket &socket) {
        std::string buffer;
        for (;;) {
            boost::system::error_code failed;
            const auto head_size = asio::read_until(socket, asio::dynamic_buffer(buffer), "\r\n\r\n", failed);
            if (failed) return;
            Request request;
            request.head = buffer.substr(0, head_size - 2);
            buffer.erase(0, head_size);
            std::size_t length = 0;
            for (const auto &line : request.lines())
                if (line.starts_with("Content-Length: ")) length = std::stoul(line.substr(16));
            if (buffer.size() < length) asio::read(socket, asio::dynamic_buffer(buffer), asio::transfer_exactly(length - buffer.size()), failed);
            if (failed) return;
            request.body = buffer.substr(0, length);
            buffer.erase(0, length);
            {
                std::lock_guard lock(mutex);
                requests.push_back(request);
            }
            try {
                handler(request, socket);
            } catch (const boost::system::system_error &) {
                return;
            }
        }
    }
    std::string url(std::string_view path) const { return "http://127.0.0.1:" + std::to_string(port) + std::string(path); }
    Request request(std::size_t index) {
        std::lock_guard lock(mutex);
        REQUIRE(index < requests.size());
        return requests[index];
    }
    static void reply(tcp::socket &socket, std::string_view status, std::string_view headers, std::string_view body) {
        asio::write(socket, asio::buffer("HTTP/1.1 " + std::string(status) + "\r\n" + std::string(headers) +
                                         "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body)));
    }
    static void chunk(tcp::socket &socket, std::string_view bytes) {
        char size[20];
        std::snprintf(size, sizeof size, "%zx\r\n", bytes.size());
        asio::write(socket, asio::buffer(std::string(size) + std::string(bytes) + "\r\n"));
    }
};

HttpSession::Options options(json::array cookies = {}) {
    HttpSession::Options result;
    result.cookies = std::move(cookies);
    result.protocols = "http,https";
    return result;
}
std::string error_of(const std::function<void()> &body) {
    try {
        body();
    } catch (const Error &error) {
        UNSCOPED_INFO(error.what());
        return std::to_string(error.status) + " " + error.code;
    }
    return "no error";
}
json::object cookie(std::string_view name, std::string_view value, std::string_view domain, std::string_view path = "/") {
    return {{"name", name}, {"value", value}, {"domain", domain}, {"path", path}};
}
} // namespace

TEST_CASE("A request carries the cookies of its host, path and scheme", "[firefox-http]") {
    auto secure = cookie("secure", "s", ".google.com");
    secure["secure"] = true;
    auto expired = cookie("expired", "e", ".google.com");
    expired["expires"] = 1000;
    auto fresh = cookie("fresh", "f", ".google.com");
    fresh["expires"] = 3000.5;
    auto session = cookie("session", "x", ".google.com");
    session["expires"] = -1;
    const json::array stored{cookie("root", "1", ".google.com"),
                             cookie("host", "2", "aistudio.google.com"),
                             cookie("deep", "3", ".google.com", "/prompts/new"),
                             cookie("section", "4", ".google.com", "/prompts"),
                             cookie("other", "5", ".example.com"),
                             secure, expired, fresh, session, json::value("not a cookie")};
    CHECK(cookie_header(stored, "https://aistudio.google.com/prompts/new_chat?x=1", 2000) == "section=4; root=1; host=2; secure=s; fresh=f; session=x");
    CHECK(cookie_header(stored, "https://aistudio.google.com/prompts/new/chat#top", 2000) ==
          "deep=3; section=4; root=1; host=2; secure=s; fresh=f; session=x");
    // Not a parent domain's host cookie, nor a secure cookie over HTTP.
    CHECK(cookie_header(stored, "http://www.GOOGLE.com:8080", 2000) == "root=1; fresh=f; session=x");
    CHECK(cookie_header(stored, "https://notgoogle.com/", 2000).empty());
    CHECK(cookie_header(stored, "https://user:pass@accounts.google.com/", 4000) == "root=1; secure=s; session=x");
    CHECK(cookie_header(stored, "not a url", 2000).empty());
}

TEST_CASE("Set-Cookie headers update the stored cookies", "[firefox-http]") {
    json::array stored{cookie("keep", "1", ".google.com")};
    const std::string url = "https://aistudio.google.com/app/prompts/new_chat";

    REQUIRE(merge_set_cookie(stored, "SID=abc; Domain=.Google.com; Path=/; Secure; HttpOnly; SameSite=none; Max-Age=600", url, 1000));
    REQUIRE(stored.size() == 2);
    CHECK(stored[1].as_object() == json::object{{"name", "SID"}, {"value", "abc"}, {"domain", ".google.com"}, {"path", "/"}, {"secure", true},
                                                {"httpOnly", true}, {"sameSite", "None"}, {"expires", 1600}});
    // The same cookie again changes nothing; a new value replaces it in place.
    CHECK_FALSE(merge_set_cookie(stored, "SID=abc; Domain=google.com; Path=/; Secure; HttpOnly; SameSite=None; Max-Age=600", url, 1000));
    CHECK(merge_set_cookie(stored, "SID=def; Domain=google.com; Path=/", url, 1000));
    REQUIRE(stored.size() == 2);
    CHECK(string_or(stored[1].as_object(), "value") == "def");
    CHECK_FALSE(stored[1].as_object().contains("expires"));

    SECTION("a cookie without Domain belongs to the host, under the request's directory") {
        REQUIRE(merge_set_cookie(stored, "local = v ; Expires=Wed, 21 Oct 2037 07:28:00 GMT", url, 1000));
        const auto &local = stored[2].as_object();
        CHECK(string_or(local, "name") == "local");
        CHECK(string_or(local, "value") == "v");
        CHECK(string_or(local, "domain") == "aistudio.google.com");
        CHECK(string_or(local, "path") == "/app/prompts");
        CHECK(local.at("expires").as_int64() == 2139722880);
        CHECK(cookie_header(stored, "https://aistudio.google.com/app/prompts/x", 1000) == "local=v; keep=1; SID=def");
        CHECK(cookie_header(stored, "https://aistudio.google.com/app", 1000) == "keep=1; SID=def");
    }
    SECTION("an expiry in the past deletes") {
        CHECK(merge_set_cookie(stored, "SID=; Domain=google.com; Path=/; Max-Age=0", url, 1000));
        CHECK(stored.size() == 1);
        CHECK(merge_set_cookie(stored, "keep=gone; Domain=google.com; Expires=Thu, 01 Jan 1970 00:00:10 GMT", "https://aistudio.google.com/", 1000));
        CHECK(stored.empty());
        // Nothing to delete.
        CHECK_FALSE(merge_set_cookie(stored, "keep=gone; Max-Age=-1", url, 1000));
    }
    SECTION("what a host may not set is ignored") {
        CHECK_FALSE(merge_set_cookie(stored, "evil=1; Domain=example.com", url, 1000));
        CHECK_FALSE(merge_set_cookie(stored, "evil=1; Domain=studio.google.com", url, 1000));
        CHECK_FALSE(merge_set_cookie(stored, "no pair", url, 1000));
        CHECK_FALSE(merge_set_cookie(stored, "=value", url, 1000));
        CHECK_FALSE(merge_set_cookie(stored, "a=b", "nonsense", 1000));
        CHECK(stored.size() == 2);
    }
}

TEST_CASE("A session sends exactly the headers it was given", "[firefox-http]") {
    Server server([](const Server::Request &request, tcp::socket &socket) {
        if (request.request_line().starts_with("GET /moved"))
            Server::reply(socket, "302 Found", "Location: https://accounts.google.com/ServiceLogin\r\n", "");
        else
            Server::reply(socket, "200 OK", "Set-Cookie: fresh=new; Path=/\r\nSet-Cookie: other=2; Path=/; Max-Age=60\r\nX-Reply: yes\r\n",
                          "echo:" + request.body);
    });
    HttpSession session(options({cookie("sid", "abc", "127.0.0.1"), cookie("elsewhere", "no", ".example.com")}));

    HttpRequest get;
    get.url = server.url("/page?x=1");
    get.headers = {{"User-Agent", "Mozilla/5.0 Firefox/152.0"}, {"Accept", "text/html"}, {"Accept-Language", "en-US,en;q=0.5"},
                   {"Accept-Encoding", "gzip, deflate"}, {"Cookie", ""}, {"Sec-Fetch-Dest", "document"}};
    auto response = session.send(get);
    CHECK(response.status == 200);
    CHECK(response.body == "echo:");
    CHECK(header_value(response.headers, "x-reply") == "yes");
    CHECK(header_value(response.headers, "missing").empty());
    auto seen = server.request(0);
    CHECK(seen.request_line() == "GET /page?x=1 HTTP/1.1");
    CHECK(seen.lines() == std::vector<std::string>{"Host: 127.0.0.1:" + std::to_string(server.port), "User-Agent: Mozilla/5.0 Firefox/152.0",
                                                   "Accept: text/html", "Accept-Language: en-US,en;q=0.5", "Accept-Encoding: gzip, deflate",
                                                   "Cookie: sid=abc", "Sec-Fetch-Dest: document"});
    // The response's cookies are in the jar for the next request.
    CHECK(session.take_cookie_change());
    CHECK_FALSE(session.take_cookie_change());
    CHECK(session.cookies().size() == 4);

    HttpRequest post;
    post.method = "POST";
    post.url = server.url("/rpc");
    post.headers = {{"Content-Type", "application/json+protobuf"}, {"X-Goog-AuthUser", "0"}, {"Cookie", ""}};
    post.body = "[1,2,3]";
    response = session.send(post);
    CHECK(response.body == "echo:[1,2,3]");
    seen = server.request(1);
    CHECK(seen.request_line() == "POST /rpc HTTP/1.1");
    CHECK(seen.lines() == std::vector<std::string>{"Host: 127.0.0.1:" + std::to_string(server.port), "Content-Type: application/json+protobuf",
                                                   "X-Goog-AuthUser: 0", "Cookie: sid=abc; fresh=new; other=2", "Content-Length: 7"});
    // Setting the same cookies again is no change.
    CHECK_FALSE(session.take_cookie_change());

    SECTION("without cookies for the host the Cookie header is left out, and a bare POST has no content type") {
        HttpSession empty(options());
        HttpRequest bare;
        bare.method = "POST";
        bare.url = server.url("/bare");
        bare.headers = {{"Cookie", ""}};
        bare.body = "";
        CHECK(empty.send(bare).status == 200);
        CHECK(server.request(2).lines() == std::vector<std::string>{"Host: 127.0.0.1:" + std::to_string(server.port), "Content-Length: 0"});
    }
    SECTION("a redirect is returned, not followed") {
        HttpRequest moved;
        moved.url = server.url("/moved");
        response = session.send(moved);
        CHECK(response.status == 302);
        CHECK(header_value(response.headers, "Location") == "https://accounts.google.com/ServiceLogin");
    }
    SECTION("requests from several threads share the session") {
        std::vector<std::future<std::string>> replies;
        for (int index = 0; index < 8; ++index)
            replies.push_back(std::async(std::launch::async, [&, index] {
                HttpRequest request;
                request.method = "POST";
                request.url = server.url("/rpc");
                request.body = std::to_string(index);
                return session.send(request).body;
            }));
        for (int index = 0; index < 8; ++index) CHECK(replies[index].get() == "echo:" + std::to_string(index));
    }
}

TEST_CASE("A streamed response is read as it arrives", "[firefox-http]") {
    std::promise<void> second;
    const auto second_sent = second.get_future().share();
    Server server([&](const Server::Request &request, tcp::socket &socket) {
        asio::write(socket, asio::buffer(std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n")));
        if (request.request_line().starts_with("GET /large")) {
            // More than a stalled reader is allowed to hold.
            const std::string block(64 * 1024, 'x');
            for (int index = 0; index < 32; ++index) Server::chunk(socket, block);
        } else {
            Server::chunk(socket, "first");
            second_sent.wait();
            Server::chunk(socket, "second");
        }
        Server::chunk(socket, "");
    });
    HttpSession session(options());
    HttpRequest request;

    SECTION("piece by piece") {
        request.url = server.url("/stream");
        const auto exchange = session.open(request);
        CHECK(exchange->status() == 200);
        CHECK(header_value(exchange->headers(), "content-type") == "text/plain");
        CHECK(exchange->read(4, 5s) == std::string("firs"));
        CHECK(exchange->read(100, 5s) == std::string("t"));
        CHECK_FALSE(exchange->read(100, 50ms));
        second.set_value();
        CHECK(exchange->read(100, 5s) == std::string("second"));
        CHECK(exchange->read(100, 5s) == std::string());
        CHECK(exchange->read(100, 5s) == std::string());
    }
    SECTION("with a reader slower than the server") {
        second.set_value();
        request.url = server.url("/large");
        const auto exchange = session.open(request);
        std::this_thread::sleep_for(300ms);
        std::size_t total = 0;
        for (;;) {
            const auto bytes = exchange->read(32768, 5s);
            REQUIRE(bytes);
            if (bytes->empty()) break;
            REQUIRE(bytes->find_first_not_of('x') == std::string::npos);
            total += bytes->size();
        }
        CHECK(total == 32u * 64 * 1024);
    }
    SECTION("until its reader gives up") {
        request.url = server.url("/stream");
        std::stop_source source;
        const auto exchange = session.open(request);
        CHECK(exchange->read(100, 5s) == std::string("first"));
        std::jthread canceller([&] {
            std::this_thread::sleep_for(100ms);
            source.request_stop();
        });
        CHECK(error_of([&] { exchange->read(100, 10s, source.get_token()); }) == "499 cancelled");
        second.set_value();
    }
}

TEST_CASE("A session reports what kept a response from arriving", "[firefox-http]") {
    std::promise<void> release;
    const auto released = release.get_future().share();
    Server silent([&](const Server::Request &, tcp::socket &) { released.wait(); });
    HttpRequest request;
    request.url = silent.url("/");

    SECTION("no answer in time") {
        HttpSession session(options());
        CHECK(error_of([&] { session.open(request, {}, 200ms); }) == "504 upstream_timeout");
        CHECK(error_of([&] { session.send(request, {}, 200ms); }) == "504 upstream_timeout");
    }
    SECTION("a caller that cancels") {
        HttpSession session(options());
        std::stop_source source;
        std::jthread canceller([&] {
            std::this_thread::sleep_for(100ms);
            source.request_stop();
        });
        CHECK(error_of([&] { session.send(request, source.get_token()); }) == "499 cancelled");
        CHECK(error_of([&] { session.send(request, source.get_token()); }) == "499 cancelled");
    }
    SECTION("nothing listening") {
        unsigned short closed_port = 0;
        {
            asio::io_context io;
            tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
            closed_port = acceptor.local_endpoint().port();
        }
        HttpSession direct(options());
        request.url = "http://127.0.0.1:" + std::to_string(closed_port) + "/";
        CHECK(error_of([&] { direct.send(request); }) == "502 network_failed");
        auto proxied_options = options();
        proxied_options.proxy = "http://127.0.0.1:" + std::to_string(closed_port);
        HttpSession proxied(std::move(proxied_options));
        request.url = silent.url("/");
        CHECK(error_of([&] { proxied.send(request); }) == "502 proxy_failed");
    }
    SECTION("a scheme the session does not speak") {
        HttpSession session{HttpSession::Options{}};
        CHECK(error_of([&] { session.send(request); }) == "502 network_failed");
    }
    release.set_value();
}

TEST_CASE("Encoded responses are decoded", "[firefox-http]") {
    const auto codings = HttpSession::accept_encoding();
    REQUIRE_THAT(codings, ContainsSubstring("gzip, deflate"));
    // gzip of "hello".
    static constexpr unsigned char gzip[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xcb, 0x48, 0xcd,
                                             0xc9, 0xc9, 0x07, 0x00, 0x86, 0xa6, 0x10, 0x36, 0x05, 0x00, 0x00, 0x00};
    Server server([](const Server::Request &, tcp::socket &socket) {
        Server::reply(socket, "200 OK", "Content-Encoding: gzip\r\n", std::string_view(reinterpret_cast<const char *>(gzip), sizeof gzip));
    });
    HttpSession session(options());
    HttpRequest request;
    request.url = server.url("/");
    request.headers = {{"Accept-Encoding", codings}};
    CHECK(session.send(request).body == "hello");
    CHECK(server.request(0).lines().back() == "Accept-Encoding: " + codings);
}
