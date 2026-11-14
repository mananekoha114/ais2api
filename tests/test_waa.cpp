// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The parts of the WAA runtime that need no JavaScript engine.
#include "../waa/src/loop.hpp"
#include "../waa/src/text.hpp"
#include "ais2api/waa/runtime.hpp"
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <future>
#include <mutex>
#include <openssl/evp.h>
#include <thread>

using namespace ais2api;
using namespace std::chrono_literals;
namespace {
// What Waa/Create sends: the challenge's JSON, each byte lowered by 97, as
// Base64 in the second slot.
std::string obfuscated(std::string_view challenge) {
    std::string bytes(challenge);
    for (auto &byte : bytes) byte = static_cast<char>(static_cast<unsigned char>(byte) - 97);
    std::string encoded((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                     reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(size));
    return json::serialize(json::array{nullptr, encoded});
}
std::string error_code(const std::function<void()> &body) {
    try {
        body();
    } catch (const Error &error) {
        return std::to_string(error.status) + " " + error.code;
    }
    return "no error";
}
std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const char character : bytes) {
        out += digits[static_cast<unsigned char>(character) >> 4];
        out += digits[static_cast<unsigned char>(character) & 15];
    }
    return out;
}
// The order in which a loop ran its tasks.
struct Trace {
    std::mutex mutex;
    std::string order;
    void add(char task) {
        std::lock_guard lock(mutex);
        order += task;
    }
    std::string read() {
        std::lock_guard lock(mutex);
        return order;
    }
};
} // namespace

TEST_CASE("A Waa/Create response is decoded into its challenge", "[waa]") {
    const std::string fields = R"(["message",null,["//www.google.com/js/bg/HASH.js"],"HASH","PROGRAM","botguard",null,"[1,2]"])";
    const auto challenge = waa::parse_challenge(obfuscated(fields));
    CHECK(challenge.message_id == "message");
    CHECK(challenge.interpreter_javascript.empty());
    CHECK(challenge.interpreter_url == "https://www.google.com/js/bg/HASH.js");
    CHECK(challenge.interpreter_hash == "HASH");
    CHECK(challenge.program == "PROGRAM");
    CHECK(challenge.global_name == "botguard");
    CHECK(challenge.client_experiments_state_blob == "[1,2]");

    SECTION("in the clear, with the interpreter inline") {
        const auto clear = waa::parse_challenge(R"([["m",[null,"source"],["https://example.test/i.js"],"H","P","g",null,null]])");
        CHECK(clear.interpreter_javascript == "source");
        CHECK(clear.interpreter_url == "https://example.test/i.js");
        CHECK(clear.client_experiments_state_blob.empty());
    }
    SECTION("or refused") {
        CHECK(error_code([] { waa::parse_challenge("{}"); }) == "502 waa_challenge_invalid");
        CHECK(error_code([] { waa::parse_challenge("[]"); }) == "502 waa_challenge_invalid");
        CHECK(error_code([] { waa::parse_challenge(R"([null,"not base64!"])"); }) == "502 waa_challenge_invalid");
        CHECK(error_code([] { waa::parse_challenge(obfuscated(R"(["m",null,null,"H","P"])")); }) == "502 waa_challenge_invalid");
        // No program.
        CHECK(error_code([] { waa::parse_challenge(obfuscated(R"(["m",null,null,"H","","g",null,null])")); }) ==
              "502 waa_challenge_invalid");
    }
}

TEST_CASE("An interpreter is named by its SHA-256 as unpadded URL-safe Base64", "[waa]") {
    CHECK(waa::interpreter_hash("") == "47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU");
    CHECK(waa::interpreter_hash("abc") == "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0");
}

TEST_CASE("Encoding labels resolve as the Encoding Standard names them", "[waa]") {
    CHECK(waa::encoding_name(" UTF8\n") == "utf-8");
    CHECK(waa::encoding_name("latin1") == "windows-1252");
    CHECK(waa::encoding_name("utf-16") == "utf-16le");
    CHECK(waa::encoding_name("UTF-16BE") == "utf-16be");
    CHECK(waa::encoding_name("shift_jis").empty());
    CHECK(waa::encoding_name("").empty());
}

TEST_CASE("Text is decoded the way TextDecoder decodes it", "[waa]") {
    using waa::decode_text;
    CHECK(decode_text("utf-8", "h\xC3\xA9llo \xF0\x9F\x98\x80", false) == std::string("h\xC3\xA9llo \xF0\x9F\x98\x80"));
    // One replacement character for each maximal invalid prefix.
    CHECK(decode_text("utf-8", "a\xFF" "b", false) == std::string("a\xEF\xBF\xBD" "b"));
    CHECK(decode_text("utf-8", "\xE4\xB8", false) == std::string("\xEF\xBF\xBD"));
    CHECK(decode_text("utf-8", "\xF0\x9F\x98" "A", false) == std::string("\xEF\xBF\xBD" "A"));
    CHECK(decode_text("utf-8", "\xC0\xAF", false) == std::string("\xEF\xBF\xBD\xEF\xBF\xBD"));
    // A surrogate is not a scalar value.
    CHECK(decode_text("utf-8", "\xED\xA0\x80", false) == std::string("\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
    CHECK_FALSE(decode_text("utf-8", "a\xFF", true));
    CHECK(decode_text("utf-8", "plain", true) == std::string("plain"));

    using namespace std::string_literals;
    CHECK(decode_text("utf-16le", "h\0\xE9\0"s, false) == std::string("h\xC3\xA9"));
    CHECK(decode_text("utf-16le", "\x3D\xD8\x00\xDE"s, false) == std::string("\xF0\x9F\x98\x80"));
    CHECK(decode_text("utf-16le", "\x3D\xD8" "a\0"s, false) == std::string("\xEF\xBF\xBD" "a"));
    CHECK(decode_text("utf-16le", "a\0b"s, false) == std::string("a\xEF\xBF\xBD"));
    CHECK_FALSE(decode_text("utf-16le", "a\0b"s, true));
    CHECK(decode_text("utf-16be", "\0h\0\xE9"s, false) == std::string("h\xC3\xA9"));
    CHECK(decode_text("windows-1252", "\x80\xE9", false) == std::string("\xE2\x82\xAC\xC3\xA9"));
    CHECK_FALSE(decode_text("shift_jis", "a", false));
}

TEST_CASE("The host's digests and random values have their standard forms", "[waa]") {
    CHECK(hex(waa::digest_bytes("SHA-1", "abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(waa::digest_bytes("SHA-256", "abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(waa::digest_bytes("SHA-384", "abc").size() == 48);
    CHECK(waa::digest_bytes("SHA-512", "abc").size() == 64);

    CHECK(waa::random_bytes(0).empty());
    CHECK(waa::random_bytes(24).size() == 24);
    const auto uuid = waa::random_uuid();
    REQUIRE(uuid.size() == 36);
    CHECK(uuid[8] == '-');
    CHECK(uuid[13] == '-');
    CHECK(uuid[14] == '4');
    CHECK(uuid[18] == '-');
    CHECK(std::string_view("89ab").find(uuid[19]) != std::string_view::npos);
    CHECK(uuid[23] == '-');
    CHECK(uuid != waa::random_uuid());
}

TEST_CASE("Profile JSON lists every object's keys in byte order", "[waa]") {
    const auto value = json::parse(R"({"b":1,"a":{"d":null,"c":[true,"x",{"z":0,"Y":-2}]},"B":"é"})");
    CHECK(waa::sorted_json(value) == "{\"B\":\"\xC3\xA9\",\"a\":{\"c\":[true,\"x\",{\"Y\":-2,\"z\":0}],\"d\":null},\"b\":1}");
}

TEST_CASE("The event loop runs tasks and timers in the order they became ready", "[waa]") {
    Trace trace;
    waa::EventLoop loop;
    std::promise<void> finished;
    REQUIRE(loop.post([&] {
        // Both timers fall due while this task runs; the second task is
        // queued after them.
        loop.schedule(20ms, [&] { trace.add('c'); });
        loop.schedule(5ms, [&] { trace.add('b'); });
        loop.schedule(0ms, [&] { trace.add('a'); });
        std::this_thread::sleep_for(60ms);
        loop.post([&] { trace.add('d'); });
        loop.schedule(30ms, [&] {
            trace.add('e');
            finished.set_value();
        });
    }));
    REQUIRE(finished.get_future().wait_for(5s) == std::future_status::ready);
    // A task posted with no delay keeps its place in the queue: 'a' was
    // queued when the timers were not yet due.
    CHECK(trace.read() == "abcde");
}

TEST_CASE("The event loop survives a failing task and runs its hooks", "[waa]") {
    Trace trace;
    std::promise<void> finished;
    {
        waa::EventLoop loop;
        loop.post([&] {
            loop.after_task = [&] { trace.add('.'); };
            loop.cleanup = [&] { trace.add('!'); };
            trace.add('a');
        });
        loop.post([&] {
            trace.add('b');
            throw std::runtime_error("dropped");
        });
        loop.post([&] {
            trace.add('c');
            finished.set_value();
        });
        REQUIRE(finished.get_future().wait_for(5s) == std::future_status::ready);
    }
    CHECK(trace.read() == "a.b.c.!");
}

TEST_CASE("A closed event loop drops what was queued and takes no more", "[waa]") {
    Trace trace;
    waa::EventLoop loop;
    std::promise<void> running, release;
    loop.post([&] {
        running.set_value();
        release.get_future().wait();
        trace.add('a');
    });
    loop.post([&] { trace.add('b'); });
    loop.schedule(1ms, [&] { trace.add('c'); });
    running.get_future().wait();
    const auto closed = loop.closed();
    CHECK_FALSE(closed.stop_requested());
    loop.close();
    CHECK(closed.stop_requested());
    CHECK_FALSE(loop.post([&] { trace.add('d'); }));
    // The running task is not interrupted.
    CHECK_FALSE(loop.wait_exit(20ms));
    release.set_value();
    CHECK(loop.wait_exit(5s));
    loop.join();
    CHECK(trace.read() == "a");
}
