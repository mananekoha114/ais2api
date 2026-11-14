// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What a session built with curl-impersonate puts on the wire, against
// Firefox's TLS ClientHello and HTTP/2 framing.
#ifdef AIS2API_CURL_IMPERSONATE
#include "../browser/firefox_http.hpp"
#include "tls_fixture.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <catch2/catch_test_macros.hpp>
#ifdef _MSC_VER
// nghttp2.h declares its older functions with ssize_t, which MSVC lacks.
#include <BaseTsd.h>
using ssize_t = SSIZE_T;
#endif
#include <nghttp2/nghttp2.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace ais2api;
using namespace std::chrono_literals;
using browser_detail::HttpRequest;
using browser_detail::HttpSession;

namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// Firefox's values, as measured from Camoufox 158 on Windows
// (docs/reports/firefox-network-fingerprint-2026-11-12.md). Written out here
// rather than taken from src/firefox_http.cpp, so that a wrong value there
// shows.
const std::vector<std::string> firefox_ciphers{"1301", "1303", "1302", "c02b", "c02f", "cca9", "cca8", "c02c",
                                               "c030", "c013", "c014", "009c", "009d", "002f", "0035"};
const std::vector<std::string> firefox_extensions{"0000", "0017", "ff01", "000a", "000b", "0023", "0010", "0005", "0022",
                                                  "0012", "0033", "002b", "000d", "002d", "001c", "001b", "fe0d"};
const std::vector<std::string> firefox_groups{"11ec", "001d", "0017", "0018", "0019"};
const std::vector<std::string> firefox_key_shares{"11ec", "001d", "0017"};
const std::vector<std::string> firefox_signature_algorithms{"0403", "0503", "0603", "0804", "0805", "0806",
                                                            "0401", "0501", "0601", "0203", "0201"};
const std::vector<std::string> firefox_delegated_credentials{"0403", "0503", "0603", "0203"};
const std::vector<std::string> firefox_versions{"0304", "0303"};
// zlib, brotli, zstd.
const std::vector<std::string> firefox_certificate_compression{"0001", "0002", "0003"};
const std::vector<std::string> firefox_alpn{"h2", "http/1.1"};
constexpr unsigned firefox_record_size_limit = 16385;
constexpr std::string_view firefox_ja4 = "t13d1517h2_8daaf6152771_3cbfd9057e0d";
// HEADER_TABLE_SIZE, ENABLE_PUSH, INITIAL_WINDOW_SIZE, MAX_FRAME_SIZE.
const std::vector<std::pair<unsigned, std::uint32_t>> firefox_settings{{1, 65536}, {2, 0}, {4, 131072}, {5, 16384}};
constexpr std::uint32_t firefox_connection_window_update = 12517377;
constexpr std::uint32_t firefox_stream_window_update = 12451840;

std::string hex4(unsigned value) {
    char text[5];
    std::snprintf(text, sizeof text, "%04x", value & 0xffff);
    return text;
}
bool grease(unsigned value) { return (value & 0x0f0f) == 0x0a0a && (value >> 8) == (value & 0xff); }

// Reads big-endian fields off a byte string.
struct Reader {
    std::string_view bytes;
    std::size_t at = 0;
    unsigned u8() {
        REQUIRE(at < bytes.size());
        return static_cast<unsigned char>(bytes[at++]);
    }
    unsigned u16() {
        const auto high = u8();
        return high << 8 | u8();
    }
    unsigned u24() {
        const auto high = u8();
        return high << 16 | u16();
    }
    std::uint32_t u32() {
        const std::uint32_t high = u16();
        return high << 16 | u16();
    }
    std::string_view take(std::size_t size) {
        REQUIRE(at + size <= bytes.size());
        const auto part = bytes.substr(at, size);
        at += size;
        return part;
    }
    bool done() const { return at == bytes.size(); }
};
std::vector<std::string> list16(std::string_view bytes) {
    Reader reader{bytes};
    std::vector<std::string> values;
    while (!reader.done()) values.push_back(hex4(reader.u16()));
    return values;
}

struct ClientHello {
    std::vector<std::string> ciphers, extensions, groups, key_shares, signature_algorithms, delegated_credentials, versions,
        certificate_compression, alpn;
    std::string server_name;
    unsigned record_size_limit = 0;
    bool any_grease = false;

    std::string ja4() const {
        const auto count = [](std::size_t size) {
            char text[3];
            std::snprintf(text, sizeof text, "%02zu", size);
            return std::string(text);
        };
        auto sorted_ciphers = ciphers;
        std::sort(sorted_ciphers.begin(), sorted_ciphers.end());
        std::vector<std::string> sorted_extensions;
        for (const auto &extension : extensions)
            if (extension != "0000" && extension != "0010") sorted_extensions.push_back(extension);
        std::sort(sorted_extensions.begin(), sorted_extensions.end());
        const auto join = [](const std::vector<std::string> &values) {
            std::string text;
            for (const auto &value : values) text += (text.empty() ? "" : ",") + value;
            return text;
        };
        const auto version = std::find(versions.begin(), versions.end(), "0304") != versions.end() ? "13" : "12";
        const auto first_alpn = alpn.empty() ? std::string("00") : std::string{alpn[0].front(), alpn[0].back()};
        return "t" + std::string(version) + (server_name.empty() ? "i" : "d") + count(ciphers.size()) + count(extensions.size()) +
               first_alpn + "_" + sha256(join(sorted_ciphers)).substr(0, 12) + "_" +
               sha256(join(sorted_extensions) + "_" + join(signature_algorithms)).substr(0, 12);
    }
};

ClientHello parse_client_hello(std::string_view handshake) {
    ClientHello hello;
    Reader reader{handshake};
    REQUIRE(reader.u8() == 1); // ClientHello
    Reader body{reader.take(reader.u24())};
    body.u16(); // legacy version
    body.take(32);
    body.take(body.u8()); // session id
    for (const auto &cipher : list16(body.take(body.u16()))) {
        if (grease(std::stoul(cipher, nullptr, 16))) hello.any_grease = true;
        else hello.ciphers.push_back(cipher);
    }
    body.take(body.u8()); // compression methods
    Reader extensions{body.take(body.u16())};
    while (!extensions.done()) {
        const auto type = extensions.u16();
        const auto data = extensions.take(extensions.u16());
        if (grease(type)) {
            hello.any_grease = true;
            continue;
        }
        hello.extensions.push_back(hex4(type));
        Reader field{data};
        switch (type) {
        case 0: { // server_name
            Reader names{field.take(field.u16())};
            names.u8();
            hello.server_name = std::string(names.take(names.u16()));
            break;
        }
        case 10: hello.groups = list16(field.take(field.u16())); break;
        case 13: hello.signature_algorithms = list16(field.take(field.u16())); break;
        case 16: {
            Reader protocols{field.take(field.u16())};
            while (!protocols.done()) hello.alpn.emplace_back(protocols.take(protocols.u8()));
            break;
        }
        case 27: {
            Reader algorithms{field.take(field.u8())};
            while (!algorithms.done()) hello.certificate_compression.push_back(hex4(algorithms.u16()));
            break;
        }
        case 28: hello.record_size_limit = field.u16(); break;
        case 34: hello.delegated_credentials = list16(field.take(field.u16())); break;
        case 43: {
            Reader versions{field.take(field.u8())};
            while (!versions.done()) hello.versions.push_back(hex4(versions.u16()));
            break;
        }
        case 51: {
            Reader shares{field.take(field.u16())};
            while (!shares.done()) {
                hello.key_shares.push_back(hex4(shares.u16()));
                shares.take(shares.u16());
            }
            break;
        }
        default: break;
        }
    }
    for (auto *values : {&hello.groups, &hello.key_shares, &hello.signature_algorithms, &hello.versions})
        std::erase_if(*values, [&](const std::string &value) {
            const bool is_grease = grease(std::stoul(value, nullptr, 16));
            hello.any_grease = hello.any_grease || is_grease;
            return is_grease;
        });
    return hello;
}

// The ClientHello a session sends to a loopback server that answers nothing.
ClientHello capture_client_hello() {
    asio::io_context io;
    tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = acceptor.local_endpoint().port();
    std::string handshake, failure;
    std::jthread server([&] {
        try {
            tcp::socket socket(io);
            acceptor.accept(socket);
            // TLS records of the handshake type until the ClientHello is whole.
            while (handshake.size() < 4 || handshake.size() < 4 + (static_cast<std::size_t>(static_cast<unsigned char>(handshake[1])) << 16 |
                                                                        static_cast<unsigned char>(handshake[2]) << 8 |
                                                                        static_cast<unsigned char>(handshake[3]))) {
                std::array<unsigned char, 5> header{};
                asio::read(socket, asio::buffer(header));
                if (header[0] != 0x16) throw std::runtime_error("not a TLS handshake record");
                std::string record(static_cast<std::size_t>(header[3]) << 8 | header[4], '\0');
                asio::read(socket, asio::buffer(record));
                handshake += record;
            }
        } catch (const std::exception &error) {
            failure = error.what();
        }
    });
    HttpSession session({});
    // A host name, so that the ClientHello names it as Firefox's does.
    CHECK_THROWS_AS(session.send({"GET", "https://localhost:" + std::to_string(port) + "/", {}, std::nullopt}, {}, 10s), Error);
    server.join();
    INFO(failure);
    REQUIRE(failure.empty());
    return parse_client_hello(handshake);
}

struct Frame {
    unsigned type = 0, flags = 0;
    std::uint32_t stream = 0;
    std::string payload;
};
constexpr unsigned frame_data = 0, frame_headers = 1, frame_settings = 4, frame_window_update = 8;
constexpr unsigned flag_end_stream = 0x1, flag_ack = 0x1, flag_end_headers = 0x4, flag_padded = 0x8, flag_priority = 0x20;

std::string frame(unsigned type, unsigned flags, std::uint32_t stream, std::string_view payload) {
    std::string bytes{static_cast<char>(payload.size() >> 16), static_cast<char>(payload.size() >> 8), static_cast<char>(payload.size()),
                      static_cast<char>(type),           static_cast<char>(flags),         static_cast<char>(stream >> 24),
                      static_cast<char>(stream >> 16),   static_cast<char>(stream >> 8),   static_cast<char>(stream)};
    return bytes + std::string(payload);
}

// A loopback HTTP/2 server over TLS that answers every request with an empty
// 200 and records the frames the client sends, until the client goes away.
struct Http2Server {
    test::SelfSigned certificate = test::self_signed_localhost();
    fs::path ca_file = fs::temp_directory_path() / ("ais2api-h2-" + random_hex(8) + ".pem");
    asio::io_context io;
    asio::ssl::context tls{asio::ssl::context::tls_server};
    tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    unsigned short port = acceptor.local_endpoint().port();
    std::mutex mutex;
    std::vector<Frame> frames;
    std::string failure;
    std::jthread thread;

    Http2Server() {
        atomic_write(ca_file, certificate.certificate);
        tls.use_certificate_chain(asio::buffer(certificate.certificate));
        tls.use_private_key(asio::buffer(certificate.key), asio::ssl::context::pem);
        SSL_CTX_set_alpn_select_cb(
            tls.native_handle(),
            [](SSL *, const unsigned char **out, unsigned char *size, const unsigned char *offered, unsigned offered_size, void *) {
                static const unsigned char h2[] = {2, 'h', '2'};
                unsigned char *selected = nullptr;
                if (SSL_select_next_proto(&selected, size, h2, sizeof h2, offered, offered_size) != OPENSSL_NPN_NEGOTIATED)
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                *out = selected;
                return SSL_TLSEXT_ERR_OK;
            },
            nullptr);
        thread = std::jthread([this] { serve(); });
    }
    ~Http2Server() {
        // A connection of its own wakes a server still waiting for one.
        boost::system::error_code ignored_error;
        tcp::socket wake(io);
        wake.connect({asio::ip::address_v4::loopback(), port}, ignored_error);
        thread = {};
        std::error_code ignored;
        fs::remove(ca_file, ignored);
    }
    std::string url(std::string_view path) const { return "https://localhost:" + std::to_string(port) + std::string(path); }
    HttpSession::Options options(json::array cookies = {}) const {
        HttpSession::Options result;
        result.ca_file = ca_file;
        result.cookies = std::move(cookies);
        return result;
    }
    void serve() {
        try {
            tcp::socket socket(io);
            acceptor.accept(socket);
            asio::ssl::stream<tcp::socket> stream(std::move(socket), tls);
            stream.handshake(asio::ssl::stream_base::server);
            std::string preface(24, '\0');
            asio::read(stream, asio::buffer(preface));
            if (preface != "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") throw std::runtime_error("no HTTP/2 preface");
            asio::write(stream, asio::buffer(frame(frame_settings, 0, 0, {})));
            for (;;) {
                std::array<unsigned char, 9> header{};
                boost::system::error_code closed;
                asio::read(stream, asio::buffer(header), closed);
                if (closed) return;
                Frame received;
                received.type = header[3];
                received.flags = header[4];
                received.stream = (static_cast<std::uint32_t>(header[5]) << 24 | header[6] << 16 | header[7] << 8 | header[8]) & 0x7fffffff;
                received.payload.resize(static_cast<std::size_t>(header[0]) << 16 | header[1] << 8 | header[2]);
                asio::read(stream, asio::buffer(received.payload), closed);
                if (closed) return;
                {
                    std::lock_guard lock(mutex);
                    frames.push_back(received);
                }
                if (received.type == frame_settings && !(received.flags & flag_ack))
                    asio::write(stream, asio::buffer(frame(frame_settings, flag_ack, 0, {})));
                // An indexed ":status: 200" ends each request's stream.
                if ((received.type == frame_headers || received.type == frame_data) && (received.flags & flag_end_stream))
                    asio::write(stream, asio::buffer(frame(frame_headers, flag_end_headers | flag_end_stream, received.stream, "\x88")));
            }
        } catch (const std::exception &error) {
            std::lock_guard lock(mutex);
            failure = error.what();
        }
    }
    // Waits for the client to go away, then returns what it sent.
    std::vector<Frame> finish() {
        if (thread.joinable()) thread.join();
        std::lock_guard lock(mutex);
        INFO(failure);
        CHECK(failure.empty());
        return frames;
    }
};

struct HeaderBlock {
    std::uint32_t stream = 0;
    bool priority = false, exclusive = false;
    std::uint32_t depends_on = 0;
    unsigned weight = 0;
    std::vector<std::pair<std::string, std::string>> fields;
};
// The request headers of each HEADERS frame, decoded in order.
std::vector<HeaderBlock> header_blocks(const std::vector<Frame> &frames) {
    nghttp2_hd_inflater *inflater = nullptr;
    REQUIRE(nghttp2_hd_inflate_new(&inflater) == 0);
    std::unique_ptr<nghttp2_hd_inflater, decltype(&nghttp2_hd_inflate_del)> owner(inflater, nghttp2_hd_inflate_del);
    std::vector<HeaderBlock> blocks;
    for (const auto &received : frames) {
        if (received.type != frame_headers) continue;
        // One frame per header block: no CONTINUATION.
        REQUIRE((received.flags & flag_end_headers));
        HeaderBlock block;
        block.stream = received.stream;
        Reader reader{received.payload};
        std::size_t padding = 0;
        if (received.flags & flag_padded) padding = reader.u8();
        if (received.flags & flag_priority) {
            block.priority = true;
            const auto dependency = reader.u32();
            block.exclusive = dependency >> 31;
            block.depends_on = dependency & 0x7fffffff;
            block.weight = reader.u8() + 1;
        }
        const auto fragment = received.payload.substr(reader.at, received.payload.size() - reader.at - padding);
        auto in = reinterpret_cast<const std::uint8_t *>(fragment.data());
        std::size_t left = fragment.size();
        for (;;) {
            nghttp2_nv field{};
            int flags = 0;
            const auto used = nghttp2_hd_inflate_hd3(inflater, &field, &flags, in, left, 1);
            REQUIRE(used >= 0);
            in += used;
            left -= static_cast<std::size_t>(used);
            if (flags & NGHTTP2_HD_INFLATE_EMIT)
                block.fields.emplace_back(std::string(reinterpret_cast<const char *>(field.name), field.namelen),
                                          std::string(reinterpret_cast<const char *>(field.value), field.valuelen));
            if (flags & NGHTTP2_HD_INFLATE_FINAL) {
                nghttp2_hd_inflate_end_headers(inflater);
                break;
            }
            if (!(flags & NGHTTP2_HD_INFLATE_EMIT) && left == 0) break;
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}
std::vector<std::string> names_of(const HeaderBlock &block) {
    std::vector<std::string> names;
    for (const auto &[name, value] : block.fields) names.push_back(name);
    return names;
}

json::object cookie(std::string_view name, std::string_view value) {
    return {{"name", name}, {"value", value}, {"domain", "localhost"}, {"path", "/"}};
}

// A page load and a page RPC on one connection, as the SpiderMonkey backend
// sends them.
std::vector<Frame> page_and_rpc(Http2Server &server) {
    {
        HttpSession session(server.options({cookie("SID", "one"), cookie("HSID", "two")}));
        HttpRequest document{"GET", server.url("/prompts/new_chat"),
                             {{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:152.0) Gecko/20100101 Firefox/152.0"},
                              {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                              {"Accept-Language", "en-US,en;q=0.9"},
                              {"Accept-Encoding", "gzip, deflate, br, zstd"},
                              {"Cookie", ""},
                              {"Upgrade-Insecure-Requests", "1"},
                              {"Sec-Fetch-Dest", "document"},
                              {"Sec-Fetch-Mode", "navigate"},
                              {"Sec-Fetch-Site", "none"},
                              {"Sec-Fetch-User", "?1"},
                              {"Priority", "u=0, i"},
                              {"TE", "trailers"}},
                             std::nullopt,
                             42};
        REQUIRE(session.send(std::move(document), {}, 10s).status == 200);
        HttpRequest rpc{"POST", server.url("/$rpc/Service/Method"),
                        {{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:152.0) Gecko/20100101 Firefox/152.0"},
                         {"Accept", "*/*"},
                         {"Content-Type", "application/json+protobuf"},
                         {"Content-Length", ""},
                         {"Origin", "https://aistudio.google.com"},
                         {"Cookie", ""},
                         {"Sec-Fetch-Dest", "empty"},
                         {"TE", "trailers"}},
                        std::string("[]")};
        REQUIRE(session.send(std::move(rpc), {}, 10s).status == 200);
    }
    return server.finish();
}
} // namespace

TEST_CASE("The ClientHello is Firefox's", "[firefox-http][fingerprint]") {
    const auto hello = capture_client_hello();
    CHECK(hello.ciphers == firefox_ciphers);
    CHECK(hello.extensions == firefox_extensions);
    CHECK(hello.groups == firefox_groups);
    CHECK(hello.key_shares == firefox_key_shares);
    CHECK(hello.signature_algorithms == firefox_signature_algorithms);
    CHECK(hello.delegated_credentials == firefox_delegated_credentials);
    CHECK(hello.versions == firefox_versions);
    CHECK(hello.certificate_compression == firefox_certificate_compression);
    CHECK(hello.alpn == firefox_alpn);
    CHECK(hello.record_size_limit == firefox_record_size_limit);
    CHECK(hello.server_name == "localhost");
    CHECK_FALSE(hello.any_grease);
    CHECK(hello.ja4() == firefox_ja4);
}

TEST_CASE("The HTTP/2 connection opens as Firefox's does", "[firefox-http][fingerprint]") {
    Http2Server server;
    const auto frames = page_and_rpc(server);
    REQUIRE(frames.size() >= 3);

    // SETTINGS, then the connection's WINDOW_UPDATE, then the first request.
    REQUIRE(frames[0].type == frame_settings);
    std::vector<std::pair<unsigned, std::uint32_t>> settings;
    Reader reader{frames[0].payload};
    while (!reader.done()) {
        const auto id = reader.u16();
        settings.emplace_back(id, reader.u32());
    }
    CHECK(settings == firefox_settings);
    REQUIRE(frames[1].type == frame_window_update);
    CHECK(frames[1].stream == 0);
    CHECK(Reader{frames[1].payload}.u32() == firefox_connection_window_update);
    CHECK(frames[2].type == frame_headers);
}

TEST_CASE("HTTP/2 requests carry Firefox's priorities and header order", "[firefox-http][fingerprint]") {
    Http2Server server;
    const auto blocks = header_blocks(page_and_rpc(server));
    REQUIRE(blocks.size() == 2);
    const auto &document = blocks[0], &rpc = blocks[1];

    // Firefox gives every request a weight on its HEADERS frame, depending
    // on nothing and never exclusive.
    CHECK(document.priority);
    CHECK_FALSE(document.exclusive);
    CHECK(document.depends_on == 0);
    CHECK(document.weight == 42);
    CHECK(rpc.priority);
    CHECK_FALSE(rpc.exclusive);
    CHECK(rpc.depends_on == 0);
    CHECK(rpc.weight == 22);

    // The pseudo-headers in Firefox's order, then the given headers in
    // theirs, with each cookie in a header of its own.
    CHECK(names_of(document) == std::vector<std::string>{":method", ":path", ":authority", ":scheme", "user-agent", "accept",
                                                         "accept-language", "accept-encoding", "cookie", "cookie",
                                                         "upgrade-insecure-requests", "sec-fetch-dest", "sec-fetch-mode",
                                                         "sec-fetch-site", "sec-fetch-user", "priority", "te"});
    CHECK(document.fields[8].second == "SID=one");
    CHECK(document.fields[9].second == "HSID=two");
    // The body's length where the request put it.
    CHECK(names_of(rpc) == std::vector<std::string>{":method", ":path", ":authority", ":scheme", "user-agent", "accept", "content-type",
                                                    "content-length", "origin", "cookie", "cookie", "sec-fetch-dest", "te"});
    CHECK(rpc.fields[7].second == "2");
}

// Firefox opens each stream's window to 12 MiB with a WINDOW_UPDATE right
// after its HEADERS. The prebuilt curl-impersonate sends none there (it caps a
// stream's window at 1 MiB and has no option for it), so this stays a known
// difference until it is built with a patch.
TEST_CASE("Each HTTP/2 stream's window opens as Firefox's does", "[firefox-http][fingerprint][!mayfail]") {
    Http2Server server;
    const auto frames = page_and_rpc(server);
    const auto first = std::find_if(frames.begin(), frames.end(), [](const Frame &item) { return item.type == frame_headers; });
    REQUIRE(first != frames.end());
    // The client's own frames between may only be connection-wide ones (an
    // acknowledgement of the server's SETTINGS).
    const auto update = std::find_if(std::next(first), frames.end(), [&](const Frame &item) { return item.stream == first->stream; });
    REQUIRE(update != frames.end());
    UNSCOPED_INFO("first frame on the stream after its HEADERS: type " << update->type);
    REQUIRE(update->type == frame_window_update);
    CHECK(Reader{update->payload}.u32() == firefox_stream_window_update);
}
#endif
