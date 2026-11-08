// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/camoufox.hpp"
#include "ais2api/browser.hpp"
#include "ais2api/log_sources.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/store.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>

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

TEST_CASE("repeated meaningless browser output stays out of the Camoufox log", "[camoufox]") {
    CHECK(camoufox_log_noise("JavaScript error: resource://gre/modules/juggler/SimpleChannel.js, line 1: SyntaxError: redeclaration of let SimpleChannel"));
    CHECK(camoufox_log_noise("JavaScript error: chrome://remote/content/webdriver-bidi/modules/root/browsingContext.sys.mjs, line 3042: TypeError: WeakMap key null must be an object or an unregistered symbol"));
    CHECK_FALSE(camoufox_log_noise("WebDriver BiDi listening on ws://127.0.0.1:49152"));
    CHECK_FALSE(camoufox_log_noise("JavaScript error: resource://gre/modules/juggler/SimpleChannel.js, line 9: TypeError: other"));
    CHECK_FALSE(camoufox_log_noise("JavaScript error: chrome://remote/content/webdriver-bidi/modules/root/browsingContext.sys.mjs, line 1: Error: real failure"));
}

TEST_CASE("Camoufox's Linux fontconfig scans the kept font groups and keeps its cache", "[camoufox]") {
    const std::string conf = "<fontconfig>\n\t<dir prefix=\"cwd\">fonts</dir>\n\t<match/>\n"
                             "\t<cachedir prefix=\"xdg\">fontconfig</cachedir>\n</fontconfig>\n";
    const std::vector<fs::path> groups{fs::u8path("/opt/a&b/fonts/L"), fs::u8path("/opt/a&b/fonts/LM")};
    const auto cache = fs::u8path("/data/runtime/camoufox-fontconfig/cache");
    CHECK(camoufox_fontconfig(conf, groups, cache) ==
          "<fontconfig>\n\t<dir>/opt/a&amp;b/fonts/L</dir>\n\t<dir>/opt/a&amp;b/fonts/LM</dir>\n\t<match/>\n"
          "\t<cachedir>/data/runtime/camoufox-fontconfig/cache</cachedir>\n</fontconfig>\n");
    // In either order.
    const std::string reversed = "<cachedir prefix=\"xdg\">fontconfig</cachedir><dir prefix=\"cwd\">fonts</dir>";
    CHECK(camoufox_fontconfig(reversed, {groups[0]}, cache) ==
          "<cachedir>/data/runtime/camoufox-fontconfig/cache</cachedir><dir>/opt/a&amp;b/fonts/L</dir>");
    const auto changed = [&](const std::string& text, std::vector<fs::path> directories) {
        try {
            camoufox_fontconfig(text, directories, cache);
            return false;
        } catch (const Error& error) {
            return error.code == "camoufox_layout_changed";
        }
    };
    CHECK(changed("<dir prefix=\"cwd\">fonts</dir>", groups));
    CHECK(changed("<cachedir prefix=\"xdg\">fontconfig</cachedir>", groups));
    CHECK(changed(conf + "<dir prefix=\"cwd\">fonts</dir>", groups));
    CHECK(changed(conf, {}));
}

TEST_CASE("Xvfb runs as Camoufox's launcher runs it and reports its display", "[camoufox]") {
    const auto args = xvfb_arguments();
    const auto pair = [&](std::string_view option, std::string_view value) {
        for (std::size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == option && args[i + 1] == value) return true;
        return false;
    };
    CHECK(pair("-displayfd", "1"));
    CHECK(pair("-screen", "0"));
    CHECK(pair("0", "1x1x24"));
    CHECK(pair("-nolisten", "tcp"));
    CHECK(pair("+extension", "GLX"));
    CHECK(pair("-extension", "COMPOSITE"));
    CHECK(pair("-extension", "RENDER"));
    CHECK(std::find(args.begin(), args.end(), "-ac") != args.end());
    CHECK(xvfb_display("99") == 99u);
    CHECK(xvfb_display("0") == 0u);
    CHECK_FALSE(xvfb_display(""));
    CHECK_FALSE(xvfb_display(" 1"));
    CHECK_FALSE(xvfb_display("123456"));
    CHECK_FALSE(xvfb_display("_XSERVTransmkdir: Owner of /tmp/.X11-unix should be set to root"));
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

TEST_CASE("identity salts mix the installation secret, the account and its seed", "[camoufox][identity]") {
    const auto salt = identity_salt("secret", "acc_1", 12345);
    CHECK(salt == identity_salt("secret", "acc_1", 12345));
    // The first 64 bits of SHA-256 over "secret", "acc_1" and "12345", each
    // followed by a NUL but the last.
    CHECK(salt == std::stoull(sha256(std::string("secret") + '\0' + "acc_1" + '\0' + "12345").substr(0, 16), nullptr, 16));
    CHECK(salt != identity_salt("other", "acc_1", 12345));
    CHECK(salt != identity_salt("secret", "acc_2", 12345));
    CHECK(salt != identity_salt("secret", "acc_1", 12346));
    // The separators keep the fields apart.
    CHECK(identity_salt("a", "bc", 1) != identity_salt("ab", "c", 1));
}

TEST_CASE("the Camoufox gateway needs the fpgen model and Camoufox", "[camoufox][identity]") {
    Directory directory;
    Config config;
    config.data_dir = directory.path;
    config.browser_executable = directory.path / "camoufox.exe";
    write(config.browser_executable, "");
    Store store(config.data_dir);
    CamoufoxGateway gateway(config, store);
    Account account;
    account.id = "account-1";
    auto failure = [](auto&& call) {
        try { call(); } catch (const Error& error) { return error.code + ": " + error.what(); }
        return std::string("no error");
    };
    // No process is started for any refusal: generating an identity needs the
    // model setup installs. A dev bundle embeds it.
#ifndef AIS2API_DEV_BUNDLE
    CHECK(failure([&] { gateway.probe(account); }).starts_with("browser_not_configured: fpgen model is not installed"));
    CHECK(failure([&] { gateway.verify_identity("account-1"); }).starts_with("browser_not_configured: fpgen model"));
#endif
    // A stored identity needs no model: the launch stops at the missing
    // Camoufox files instead.
    account.id = "acc_stored";
    account.email = "stored@example.com";
    account.auth_user = "0";
    account.storage_state = {{"cookies", json::array{json::object{{"name", "SID"}, {"value", "v"}, {"domain", ".google.com"}, {"path", "/"}}}}};
    store.put_account(account);
    store.save_firefox_identity(account.id, {{"format", 1}, {"seed", store.fingerprint(account.id).seed}, {"salt", "0123456789abcdef"},
                                             {"os", "windows"}, {"config", json::object{}}, {"prefs", json::object{}}});
    const auto launch = failure([&] { gateway.probe(account); });
    INFO(launch);
    CHECK(launch.starts_with("browser_not_configured: Camoufox "));
    CHECK(launch.find("is missing beside the executable") != std::string::npos);
    // CloakBrowser's executable is not Camoufox: the backend looks for the
    // build setup installs instead.
    config.browser_executable = directory.path / "chrome.exe";
    write(config.browser_executable, "");
    CamoufoxGateway chromium(config, store);
    CHECK(failure([&] { chromium.verify_runtime(); }) == "browser_not_configured: Camoufox is not installed: run ais2api setup");
    config.browser_executable = directory.path / "missing" / "camoufox.exe";
    CamoufoxGateway missing(config, store);
    CHECK(failure([&] { missing.verify_runtime(); }).starts_with("browser_not_configured: browser_executable names a Camoufox"));
    CHECK_FALSE(bool_or(gateway.diagnostics(), "running"));
    CHECK(string_or(gateway.diagnostics(), "backend") == "camoufox");
}

TEST_CASE("Camoufox browser logs are log sources like Chromium ones", "[camoufox]") {
    Directory directory;
    fs::create_directories(directory.path / "logs");
    write(directory.path / "logs" / "browser-1.log", "a");
    write(directory.path / "logs" / "camoufox-2.log", "b");
    write(directory.path / "logs" / "camoufox-2-identity.json", "{}");
    const auto listed = log_sources(directory.path);
    std::vector<std::string> ids;
    for (const auto& source : listed.at("sources").as_array()) ids.push_back(string_or(source.as_object(), "id"));
    CHECK(ids == std::vector<std::string>{"browser-1.log", "camoufox-2.log"});
}

// Hidden by default: select it with a prepared Camoufox executable. It opens
// only a local page; no cookies, account probe or Google request.
TEST_CASE("Camoufox runs the bridge with BYOB reads and trusted keys", "[.camoufox-runtime]") {
    const auto executable = std::getenv("AIS2API_TEST_CAMOUFOX");
    REQUIRE(executable != nullptr);
    REQUIRE(fs::is_regular_file(fs::u8path(executable)));
    Directory directory;
    Config config;
    config.data_dir = directory.path;
    config.browser_executable = fs::u8path(executable);
    config.headless = std::getenv("AIS2API_TEST_CAMOUFOX_HEADED") == nullptr;
    Store store(config.data_dir);
    CamoufoxGateway gateway(config, store);
    const auto result = gateway.verify_runtime();
    INFO(json::serialize(result));
    CHECK(string_or(result, "product").starts_with("camoufox/"));
    CHECK(string_or(result, "protocol_version") == "webdriver-bidi");
    CHECK(bool_or(result, "isolated_context"));
    CHECK_FALSE(bool_or(result, "google_verified"));
    const auto& page = result.at("page").as_object();
    CHECK(bool_or(page, "bridge"));
    CHECK(page.at("webdriver") == json::value(false));
    CHECK(bool_or(page, "byobReader"));
    CHECK(bool_or(page, "trustedKey"));
    CHECK_FALSE(bool_or(gateway.diagnostics(), "running"));
    // The launch's profile is gone; the Linux fontconfig it wrote stays.
    for (const auto& entry : fs::directory_iterator(config.data_dir / "runtime")) {
        const auto name = entry.path().filename().string();
        CHECK((!name.starts_with("camoufox-") || name == "camoufox-fontconfig"));
    }
    bool retained_log = false;
    for (const auto& entry : fs::directory_iterator(config.data_dir / "logs"))
        retained_log = retained_log || (entry.is_regular_file() && entry.path().filename().string().starts_with("camoufox-"));
    CHECK(retained_log);
}

namespace {
// Copies the fpgen model from AIS2API_TEST_FPGEN_MODEL where setup installs
// it, and stores an account in the region given.
Account runtime_account(const Config& config, Store& store, const std::string& timezone, const std::string& locale) {
    const auto model = std::getenv("AIS2API_TEST_FPGEN_MODEL");
    REQUIRE(model != nullptr);
    const auto pin = fpgen_model_pin();
    const auto installed = fpgen_model_directory(config.data_dir, pin);
    fs::create_directories(installed);
    for (const auto& [name, digest] : pin.file_sha256) fs::copy_file(fs::u8path(model) / name, installed / name);
    Account account;
    account.id = "acc_runtime";
    account.email = "runtime@example.com";
    account.auth_user = "0";
    account.storage_state = {{"cookies", json::array{json::object{{"name", "SID"}, {"value", "v"}, {"domain", ".google.com"}, {"path", "/"}}}}};
    store.put_account(account);
    store.set_fingerprint_region(account.id, timezone, locale);
    return account;
}
#ifndef _WIN32
// The families a Linux identity renders with a face from the Linux font
// groups: those of the Ubuntu bases and of the bundles a draw adds.
std::set<std::string> linux_font_faces() {
    std::set<std::string> families;
    const auto catalog = fs::path(AIS2API_SOURCE_DIR) / "browser" / "camoufox";
    for (const auto* file : {"font-bases.json", "font-groups.json"}) {
        const auto document = json::parse(read_file(catalog / file, 1 << 22));
        for (const auto& entry : document.at("lin").as_array())
            for (const auto& family : entry.at("fonts").as_array()) families.emplace(family.as_string());
    }
    return families;
}
#endif
// Families no identity of this OS has. The Camoufox build bundles each of them.
#ifdef _WIN32
const std::vector<std::string> foreign_fonts{"PingFang SC", "Helvetica Neue", "Apple Color Emoji", "Noto Sans", "DejaVu Sans", "Ubuntu"};
#else
const std::vector<std::string> foreign_fonts{"PingFang SC", "Helvetica Neue", "Apple Color Emoji", "Segoe UI", "Microsoft YaHei", "Segoe UI Emoji"};
#endif
} // namespace

// Hidden by default: select it with a prepared Camoufox executable and the
// fpgen model's directory. It opens only local pages.
TEST_CASE("a generated identity is what the page reads", "[.camoufox-runtime]") {
    const auto executable = std::getenv("AIS2API_TEST_CAMOUFOX");
    REQUIRE(executable != nullptr);
    Directory directory;
    Config config;
    config.data_dir = directory.path;
    config.browser_executable = fs::u8path(executable);
    config.headless = std::getenv("AIS2API_TEST_CAMOUFOX_HEADED") == nullptr;
    Store store(config.data_dir);
    const auto account = runtime_account(config, store, "Asia/Shanghai", "zh-CN");
    CamoufoxGateway gateway(config, store);

    const auto result = gateway.verify_identity(account.id, {}, foreign_fonts);
    INFO(json::serialize(result));
    const auto& expected = result.at("expected").as_object();
    const auto& page = result.at("page").as_object();
    for (const auto& item : expected) {
        INFO(std::string(item.key()));
        if (!item.value().is_null()) CHECK(page.at(item.key()) == item.value());
    }
    CHECK(string_or(page, "language") == "zh-CN");
    CHECK(string_or(page, "timezone") == "Asia/Shanghai");
    CHECK_FALSE(page.at("voices").as_array().empty());
    const auto& identity = result.at("identity").as_object();
    CHECK(string_or(identity, "hostGpu") == "unchanged");
    CHECK(identity.at("droppedProperties").as_array().empty());
    // The page finds only fonts the identity claims.
    const auto& claimed = result.at("claimedFonts").as_array();
    const auto& detected = page.at("fonts").as_object().at("detected").as_array();
    CHECK_FALSE(detected.empty());
    for (const auto& font : detected) {
        INFO(json::serialize(font));
        CHECK(std::find(claimed.begin(), claimed.end(), font) != claimed.end());
    }
    // Canvas and WebGL read a drawing back alike each time and launch after
    // launch: none of the random readback that keeps a Linux host from
    // presenting another OS.
    const auto& readback = page.at("readback").as_object();
    for (const auto* kind : {"canvas", "webgl"}) {
        INFO(kind);
        const auto& reads = readback.at(kind).as_array();
        CHECK_FALSE(reads.at(0).is_null());
        CHECK(reads.at(0) == reads.at(1));
    }

    // Stored with the seed, and launched again as it was.
    const auto stored = store.firefox_identity(account.id);
    REQUIRE(stored);
    const auto again = gateway.verify_identity(account.id);
    CHECK(again.at("expected") == result.at("expected"));
    CHECK(again.at("page").at("readback") == page.at("readback"));
    CHECK(store.firefox_identity(account.id) == stored);
}

// Hidden by default: select it with the Camoufox setup installed
// (AIS2API_TEST_CAMOUFOX), the full build it came from
// (AIS2API_TEST_CAMOUFOX_FULL, as camoufox-dev.py fetch leaves it) and the
// fpgen model's directory. It opens only local pages.
TEST_CASE("Camoufox without its bundled fonts renders text as the full build does", "[.camoufox-fonts]") {
    const auto trimmed = std::getenv("AIS2API_TEST_CAMOUFOX");
    const auto full = std::getenv("AIS2API_TEST_CAMOUFOX_FULL");
    REQUIRE(trimmed != nullptr);
    REQUIRE(full != nullptr);
    REQUIRE(fs::is_directory(fs::u8path(full).parent_path() / "fonts"));
    std::set<std::string> kept;
    for (const auto& entry : fs::directory_iterator(fs::u8path(trimmed).parent_path() / "fonts"))
        kept.insert(path_utf8(entry.path().filename()));
#ifdef _WIN32
    // setup keeps only the emoji font.
    REQUIRE(kept == std::set<std::string>{"TwemojiMozilla.ttf"});
#else
    // setup keeps the font groups a Linux identity reads.
    REQUIRE(kept == std::set<std::string>{"L", "LM", "LMW", "LW", "TwemojiMozilla.ttf", "groups.json"});
#endif
    Directory directory;
    Config config;
    config.data_dir = directory.path;
    config.headless = std::getenv("AIS2API_TEST_CAMOUFOX_HEADED") == nullptr;
    Store store(config.data_dir);
    // Japanese text falls back across more families than English.
    const auto account = runtime_account(config, store, "Asia/Tokyo", "ja-JP");
    const auto read = [&](const char* executable) {
        config.browser_executable = fs::u8path(executable);
        CamoufoxGateway gateway(config, store);
        return gateway.verify_identity(account.id, {}, foreign_fonts);
    };
    // The same stored identity in both.
    const auto a = read(trimmed), b = read(full);
    CHECK(a.at("expected") == b.at("expected"));
    const auto& page = a.at("page").as_object();
    const auto& reference = b.at("page").as_object();
    CHECK(page.size() == reference.size());
    for (const auto& item : page) {
        INFO(std::string(item.key()));
        if (item.key() != "fonts") CHECK(item.value() == reference.at(item.key()));
    }
    const auto& fonts = page.at("fonts").as_object();
    const auto& reference_fonts = reference.at("fonts").as_object();
    const auto missing = [](const json::value& found, const json::value& from) {
        json::array result;
        for (const auto& font : from.as_array())
            if (std::find(found.as_array().begin(), found.as_array().end(), font) == found.as_array().end()) result.push_back(font);
        return json::serialize(result);
    };
    INFO("found only in the full build: " << missing(fonts.at("detected"), reference_fonts.at("detected")));
    INFO("found only without the bundled fonts: " << missing(reference_fonts.at("detected"), fonts.at("detected")));
#ifdef _WIN32
    const auto leaked = [](std::string_view) { return false; };
    CHECK(fonts.at("detected") == reference_fonts.at("detected"));
#else
    // On Linux the full build is no exact reference. Firefox activates the
    // bundled fonts directory recursively, so there the other OSes' groups
    // reach a Linux identity too: Camoufox's FONTS.md describes this leak
    // (Windows' Calibri and Cambria). Trimmed, only the Linux groups remain.
    // A family with a Linux face renders as in the full build; one without
    // may lose the leaked face.
    const auto faces = linux_font_faces();
    const auto leaked = [&](std::string_view family) {
        return family != "Twemoji Mozilla" && !faces.contains(std::string(family));
    };
    const auto& detected = fonts.at("detected").as_array();
    const auto& reference_detected = reference_fonts.at("detected").as_array();
    CHECK(missing(reference_fonts.at("detected"), fonts.at("detected")) == "[]");
    for (const auto& font : reference_detected) {
        INFO("found only in the full build: " << json::serialize(font));
        if (std::find(detected.begin(), detected.end(), font) == detected.end()) CHECK(leaked(font.as_string()));
    }
#endif
    for (const auto* key : {"widths", "families"})
        for (const auto& width : reference_fonts.at(key).as_object()) {
            INFO(key << ": " << std::string(width.key()));
            if (std::string_view(key) == "families" && leaked(width.key())) continue;
            CHECK(fonts.at(key).as_object().at(width.key()) == width.value());
        }
}
