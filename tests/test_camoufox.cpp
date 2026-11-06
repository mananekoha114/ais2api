// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/camoufox.hpp"
#include "ais2api/browser.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>

using namespace ais2api;
using namespace ais2api::browser_detail;

namespace {
struct Directory {
    fs::path path = fs::temp_directory_path() / ("ais2api-camoufox-test-" + random_hex(8));
    Directory() { fs::create_directories(path); }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
void write(const fs::path& file, std::string_view text) {
    std::ofstream output(file, std::ios::binary);
    output << text;
}
}

TEST_CASE("stored cookies become BiDi cookies and come back unchanged", "[camoufox][cookies]") {
    const auto stored = json::parse(R"([
        {"name":"SAPISID","value":"a/b","domain":".google.com","path":"/","secure":true,"httpOnly":false,"sameSite":"None","expires":1893456000.75},
        {"name":"NID","value":"n","domain":".google.com","path":"/","secure":false,"httpOnly":true,"sameSite":"None"},
        {"name":"__Host-GAPS","value":"h","domain":"accounts.google.com","path":"/","secure":true,"sameSite":"Lax"},
        {"name":"CHIPS","value":"p","domain":".google.com","partitionKey":"https://google.com"},
        {"name":"","value":"x","domain":".google.com"},
        {"name":"NOVALUE","domain":".google.com"},
        {"name":"NODOMAIN","value":"x"}])").as_array();
    const auto bidi = bidi_cookies(stored);
    REQUIRE(bidi.size() == 3);
    CHECK(bidi[0] == json::parse(R"({"name":"SAPISID","value":{"type":"string","value":"a/b"},"domain":".google.com","path":"/",
        "httpOnly":false,"secure":true,"expiry":1893456000,"sameSite":"none"})"));
    // SameSite=None without Secure is invalid; the session cookie has no expiry.
    CHECK(bidi[1] == json::parse(R"({"name":"NID","value":{"type":"string","value":"n"},"domain":".google.com","path":"/","httpOnly":true,"secure":false})"));
    CHECK(string_or(bidi[2].as_object(), "sameSite") == "lax");

    const auto browser = json::parse(R"([
        {"name":"SAPISID","value":{"type":"string","value":"a/b"},"domain":".google.com","path":"/","size":10,"httpOnly":false,"secure":true,"sameSite":"none","expiry":1893456000},
        {"name":"NID","value":{"type":"string","value":"n"},"domain":".google.com","path":"/","size":4,"httpOnly":true,"secure":false,"sameSite":"default"},
        {"name":"BIN","value":{"type":"base64","value":"AAE="},"domain":".google.com","path":"/"}])").as_array();
    const auto back = stored_cookies(browser);
    REQUIRE(back.size() == 2);
    CHECK(back[0] == json::parse(R"({"name":"SAPISID","value":"a/b","domain":".google.com","path":"/","secure":true,"httpOnly":false,"sameSite":"None","expires":1893456000})"));
    CHECK(back[1] == json::parse(R"({"name":"NID","value":"n","domain":".google.com","path":"/","secure":false,"httpOnly":true})"));
    // The stored shape feeds the existing import and write-back filters.
    CHECK(import_cookies(back).size() == 2);
}

TEST_CASE("signing cookies are the ones a request to the site root carries", "[camoufox][cookies]") {
    const auto stored = json::parse(R"([
        {"name":"SAPISID","value":"1","domain":".google.com","path":"/"},
        {"name":"HOST","value":"2","domain":"aistudio.google.com","path":"/"},
        {"name":"OTHERHOST","value":"3","domain":"accounts.google.com","path":"/"},
        {"name":"DEEP","value":"4","domain":".google.com","path":"/u/0"},
        {"name":"LOOKALIKE","value":"5","domain":"evilgoogle.com","path":"/"},
        {"name":"NOPATH","value":"6","domain":"AISTUDIO.google.com"}])").as_array();
    std::vector<std::string> names;
    for (const auto& cookie : cookies_for_host(stored, "aistudio.google.com")) names.push_back(string_or(cookie.as_object(), "name"));
    CHECK(names == std::vector<std::string>{"SAPISID", "HOST", "NOPATH"});
}

TEST_CASE("Firefox navigation errors are told apart like Chromium ones", "[camoufox]") {
    CHECK(firefox_navigation_failure("NS_ERROR_PROXY_CONNECTION_REFUSED")->code == "proxy_failed");
    CHECK(firefox_navigation_failure("Error: NS_ERROR_PROXY_AUTHENTICATION_FAILED")->code == "proxy_failed");
    CHECK(firefox_navigation_failure("NS_ERROR_UNKNOWN_PROXY_HOST")->code == "proxy_failed");
    for (auto text : {"NS_ERROR_UNKNOWN_HOST", "NS_ERROR_CONNECTION_REFUSED", "NS_ERROR_NET_TIMEOUT", "NS_ERROR_NET_RESET",
                      "NS_ERROR_NET_INTERRUPT", "NS_ERROR_OFFLINE", "SSL_ERROR_BAD_CERT_DOMAIN", "SEC_ERROR_UNKNOWN_ISSUER"})
        CHECK(firefox_navigation_failure(std::string("Navigation failed: ") + text)->code == "network_failed");
    CHECK(firefox_navigation_failure("NS_BINDING_ABORTED") == std::nullopt);
    CHECK(firefox_navigation_failure("") == std::nullopt);
}

TEST_CASE("the BiDi endpoint is read from the browser's own announcement", "[camoufox]") {
    CHECK(bidi_endpoint_port("WebDriver BiDi listening on ws://127.0.0.1:49152") == 49152);
    CHECK(bidi_endpoint_port("WebDriver BiDi listening on ws://localhost:9222/") == 9222);
    CHECK(bidi_endpoint_port("WebDriver BiDi listening on ws://127.0.0.1:9222 \r") == 9222);
    CHECK(bidi_endpoint_port("ws://192.168.1.2:9222") == std::nullopt);
    CHECK(bidi_endpoint_port("ws://127.0.0.1:0") == std::nullopt);
    CHECK(bidi_endpoint_port("ws://127.0.0.1:65536") == std::nullopt);
    CHECK(bidi_endpoint_port("ws://127.0.0.1:12ab") == std::nullopt);
    CHECK(bidi_endpoint_port("Read port: ws://127.0.0.1:") == std::nullopt);
    CHECK(bidi_endpoint_port("console.log: nothing here") == std::nullopt);
}

TEST_CASE("Camoufox configuration pieces never split a UTF-8 character", "[camoufox]") {
    CHECK(camoufox_environment("").empty());
    const auto small = camoufox_environment(R"({"a":1})");
    REQUIRE(small.size() == 1);
    CHECK(small[0] == std::pair<std::string, std::string>{"CAMOU_CONFIG_1", R"({"a":1})"});
    // 2046 ASCII bytes and a 3-byte character straddling the 2047-byte limit.
    const std::string text = std::string(2046, 'x') + "\xE4\xB8\xAD" + std::string(3000, 'y');
    const auto pieces = camoufox_environment(text);
    REQUIRE(pieces.size() == 3);
    CHECK(pieces[0].first == "CAMOU_CONFIG_1");
    CHECK(pieces[0].second.size() == 2046);
    CHECK(pieces[1].second.starts_with("\xE4\xB8\xAD"));
    CHECK(pieces[2].first == "CAMOU_CONFIG_3");
    std::string joined;
    for (const auto& [name, value] : pieces) {
        CHECK(value.size() <= 2047);
        joined += value;
    }
    CHECK(joined == text);
}

TEST_CASE("user.js sets string, boolean and integer preferences in name order", "[camoufox]") {
    const auto prefs = json::parse(R"({"network.proxy.type":1,"network.proxy.http":"127.0.0.1","remote.active-protocols":1,
        "browser.shell.checkDefaultBrowser":false,"quoted":"a\"b\\c"})").as_object();
    CHECK(firefox_user_js(prefs) ==
          "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
          "user_pref(\"network.proxy.http\", \"127.0.0.1\");\n"
          "user_pref(\"network.proxy.type\", 1);\n"
          "user_pref(\"quoted\", \"a\\\"b\\\\c\");\n"
          "user_pref(\"remote.active-protocols\", 1);\n");
    auto code = [](const char* text) {
        try { firefox_user_js(json::parse(text).as_object()); } catch (const Error& error) { return error.code; }
        return std::string("accepted");
    };
    CHECK(code(R"({"a":1.5})") == "browser_launch");
    CHECK(code(R"({"a":4294967296})") == "browser_launch");
    CHECK(code(R"({"a":"line\nbreak"})") == "browser_launch");
    CHECK(code(R"({"a":[1]})") == "browser_launch");
    CHECK(code(R"({"a":-5})") == "accepted");
}

TEST_CASE("identity pools are validated and digested", "[camoufox][identity]") {
    Directory directory;
    const auto file = directory.path / "identity-pool.json";
    CHECK_THROWS_AS(load_identity_pool(file), Error);
    write(file, R"({"schema":1,"camoufox":{"tag":"v158.0.1-beta.41"},"identities":[
        {"config":{"navigator.userAgent":"UA-1"},"prefs":{"a":true}},{"config":{"navigator.userAgent":"UA-2"}}]})");
    const auto pool = load_identity_pool(file);
    REQUIRE(pool.identities.size() == 2);
    CHECK(string_or(pool.camoufox, "tag") == "v158.0.1-beta.41");
    CHECK(pool.sha256.size() == 64);
    for (auto invalid : {R"({"schema":2,"identities":[{"config":{"a":1}}]})", R"({"schema":1,"identities":[]})",
                         R"({"schema":1,"identities":[{"config":{}}]})", R"({"schema":1,"identities":[{"config":{"a":1},"prefs":[]}]})", "not json"}) {
        write(file, invalid);
        try {
            load_identity_pool(file);
            FAIL("accepted an invalid pool");
        } catch (const Error& error) { CHECK(error.code == "browser_not_configured"); }
    }
}

TEST_CASE("launch configuration applies the account locale and timezone", "[camoufox][identity]") {
    const auto identity = json::parse(R"({"navigator.userAgent":"UA","locale:region":"US","timezone":"America/New_York"})").as_object();
    const auto chinese = camoufox_launch_config(identity, "zh-CN", "Asia/Shanghai");
    CHECK(string_or(chinese, "navigator.userAgent") == "UA");
    CHECK(string_or(chinese, "navigator.language") == "zh-CN");
    CHECK(chinese.at("navigator.languages") == json::array{"zh-CN", "zh"});
    CHECK(string_or(chinese, "headers.Accept-Language") == "zh-CN,zh;q=0.9");
    CHECK(string_or(chinese, "locale:language") == "zh");
    CHECK(string_or(chinese, "locale:region") == "CN");
    CHECK(string_or(chinese, "locale:all") == "zh-CN, zh");
    CHECK(string_or(chinese, "timezone") == "Asia/Shanghai");
    CHECK(bool_or(chinese, "allowMainWorld"));
    const auto scripted = camoufox_launch_config(identity, "zh-Hant-TW", "Asia/Taipei");
    CHECK(string_or(scripted, "locale:language") == "zh");
    CHECK(string_or(scripted, "locale:region") == "TW");
    const auto bare = camoufox_launch_config(identity, "de", "Europe/Berlin");
    CHECK(bare.at("navigator.languages") == json::array{"de"});
    CHECK(string_or(bare, "headers.Accept-Language") == "de");
    CHECK_FALSE(bare.contains("locale:region"));
    const auto unchanged = camoufox_launch_config(identity, "", "");
    CHECK(string_or(unchanged, "timezone") == "America/New_York");
    CHECK_FALSE(unchanged.contains("navigator.language"));
}
