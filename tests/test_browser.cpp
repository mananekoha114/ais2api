// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/browser.hpp"
#include "../browser/json_text.hpp"
#include "../browser/rpc_error.hpp"
#include "../browser/google_auth.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <map>

using namespace ais2api;
TEST_CASE("Browser bridge is embedded with a matching digest and required host methods", "[browser]") {
    const auto source = browser_detail::embedded_bridge_source();
    REQUIRE_FALSE(source.empty());
    REQUIRE(source.size() <= 256 * 1024);
    REQUIRE(sha256(source) == browser_detail::embedded_bridge_sha256());
    REQUIRE(source.find("identity, bootstrap, finishBootstrap, run, snapshot, append, start, ack, next, abort, discoverModels") != std::string_view::npos);
    REQUIRE(source.find("__ais2bridge") == std::string_view::npos);
}
TEST_CASE("Google authorization signs synthetic cookies with exact SHA1 hashes", "[google-auth]") {
    browser_detail::GoogleAuthorizationCache cache;
    auto cookies = [] { return json::array{
        json::object{{"name", "SAPISID"}, {"value", "main"}},
        json::object{{"name", "__Secure-1PAPISID"}, {"value", "one"}},
        json::object{{"name", "__Secure-3PAPISID"}, {"value", "three"}}}; };
    CHECK(cache.authorization(cookies, 1000) ==
        "SAPISIDHASH 1000_5ad6c62f8abc233b0ecd01af67be382176ab72f7 "
        "SAPISID1PHASH 1000_282bb37481778fc55df8d7abb9b09a07d2ee28da "
        "SAPISID3PHASH 1000_6edd2b3b30552dd953e82d9b7cb4dc7dfc275dab");
    cache.clear();
    CHECK(cache.authorization([] { return json::array{json::object{{"name", "__Secure-1PAPISID"}, {"value", "one"}}}; }, 1000) ==
        "SAPISIDHASH 1000_282bb37481778fc55df8d7abb9b09a07d2ee28da "
        "SAPISID1PHASH 1000_282bb37481778fc55df8d7abb9b09a07d2ee28da "
        "SAPISID3PHASH 1000_282bb37481778fc55df8d7abb9b09a07d2ee28da");
    cache.clear();
    CHECK(cache.authorization([] { return json::array{json::object{{"name", "SAPISID"}, {"value", "main"}}}; }, 1000) ==
        "SAPISIDHASH 1000_5ad6c62f8abc233b0ecd01af67be382176ab72f7 "
        "SAPISID1PHASH 1000_5ad6c62f8abc233b0ecd01af67be382176ab72f7 "
        "SAPISID3PHASH 1000_5ad6c62f8abc233b0ecd01af67be382176ab72f7");
}

TEST_CASE("Google authorization refreshes on TTL expiry missing cookies and isolation", "[google-auth]") {
    browser_detail::GoogleAuthorizationCache cache;
    const auto now = Clock::now();
    int calls = 0;
    json::array cookies{json::object{{"name", "SAPISID"}, {"value", "main"}, {"expires", 1100}}};
    auto refresh = [&] { ++calls; return cookies; };
    REQUIRE_FALSE(cache.authorization(refresh, 1000, now).empty());
    CHECK_FALSE(cache.authorization(refresh, 1001, now + std::chrono::seconds(59)).empty());
    CHECK(calls == 1);
    CHECK_FALSE(cache.authorization(refresh, 1002, now + std::chrono::seconds(60)).empty());
    CHECK(calls == 2);
    CHECK(cache.authorization(refresh, 1100, now + std::chrono::seconds(61)).empty());
    CHECK(calls == 3);
    CHECK(cache.authorization(refresh, 1101, now + std::chrono::seconds(62)).empty());
    CHECK(calls == 4);
    cookies = {json::object{{"name", "SAPISID"}, {"value", "main"}},
               json::object{{"name", "__Secure-3PAPISID"}, {"value", "three"}, {"expires", 1200}}};
    REQUIRE_FALSE(cache.authorization(refresh, 1199, now + std::chrono::seconds(63)).empty());
    CHECK(calls == 5);
    CHECK_FALSE(cache.authorization(refresh, 1200, now + std::chrono::seconds(64)).empty());
    CHECK(calls == 6);
    cache.clear(); cookies = {json::object{{"name", "unrelated"}, {"value", "ignored"}}, json::object{{"name", "SAPISID"}, {"value", ""}}};
    CHECK(cache.authorization(refresh, 1201, now + std::chrono::seconds(65)).empty());
    CHECK(calls == 7);
    cookies = {json::object{{"name", "SAPISID"}, {"value", "main"}, {"expires", 0}}};
    CHECK(cache.authorization(refresh, 1202, now + std::chrono::seconds(66)).empty());
    CHECK(calls == 8);
    cookies = {json::object{{"name", "SAPISID"}, {"value", "main"}, {"expires", -1}}};
    CHECK_FALSE(cache.authorization(refresh, 1203, now + std::chrono::seconds(67)).empty());
    CHECK(calls == 9);
}

TEST_CASE("Google authorization propagates cancellation and tolerates refresh errors", "[google-auth]") {
    browser_detail::GoogleAuthorizationCache cache;
    const auto now = Clock::now();
    auto failed = []() -> json::array { throw Error(502, "browser_disconnected", "Synthetic failure"); };
    CHECK(cache.authorization(failed, 1000, now).empty());
    auto cookies = [] { return json::array{json::object{{"name", "SAPISID"}, {"value", "main"}, {"expires", 1100}}}; };
    const auto expected = cache.authorization(cookies, 1000, now);
    REQUIRE_FALSE(expected.empty());
    CHECK(cache.authorization(failed, 1000, now + std::chrono::seconds(60)) == expected);
    try {
        cache.authorization([]() -> json::array { throw Error(499, "cancelled", "Synthetic cancellation"); }, 1000, now + std::chrono::seconds(61));
        FAIL("Expected cancellation");
    } catch (const Error& error) { CHECK(error.code == "cancelled"); }
    CHECK(cache.authorization(failed, 1100, now + std::chrono::seconds(62)).empty());
}

TEST_CASE("Google authorization replaces rotated cookies and never resurrects removed credentials", "[google-auth]") {
    browser_detail::GoogleAuthorizationCache cache;
    const auto now = Clock::now();
    json::array cookies{json::object{{"name", "SAPISID"}, {"value", "main"}, {"httpOnly", true}}};
    auto refresh = [&] { return cookies; };
    const auto first = cache.authorization(refresh, 1000, now);
    REQUIRE_FALSE(first.empty());
    cookies = {json::object{{"name", "SAPISID"}, {"value", "rotated"}},
               json::object{{"name", "__Secure-1PAPISID"}, {"value", "rotated-one"}},
               json::object{{"name", "__Secure-3PAPISID"}, {"value", "rotated-three"}}};
    CHECK(cache.authorization(refresh, 1000, now + std::chrono::seconds(59)) == first);
    CHECK(cache.authorization(refresh, 1000, now + std::chrono::seconds(60)) ==
        "SAPISIDHASH 1000_d7f3071ec191694f8737f36b487393a2c0784a74 "
        "SAPISID1PHASH 1000_bde977b8ade6faebd21d740edf3b73812d19620b "
        "SAPISID3PHASH 1000_caa233cd7c71647c95dadf105d97ec7763f5ba62");
    cookies = {};
    CHECK(cache.authorization(refresh, 1000, now + std::chrono::seconds(120)).empty());
    auto failed = []() -> json::array { throw Error(502, "browser_disconnected", "Synthetic failure"); };
    CHECK(cache.authorization(failed, 1000, now + std::chrono::seconds(121)).empty());
    cookies = {json::object{{"name", "__Secure-3PAPISID"}, {"value", "third-party-only"}}};
    CHECK(cache.authorization(refresh, 1000, now + std::chrono::seconds(122)).empty());
}

TEST_CASE("request body pieces never split a UTF-8 sequence", "[browser]") {
    using browser_detail::utf8_prefix;
    const std::string text = "ab\xe4\xbd\xa0" "c\xf0\x9f\x98\x80";  // ab, 3-byte, c, 4-byte
    CHECK(utf8_prefix(text, 100) == text.size());
    CHECK(utf8_prefix(text, 2) == 2);
    CHECK(utf8_prefix(text, 3) == 2);
    CHECK(utf8_prefix(text, 4) == 2);
    CHECK(utf8_prefix(text, 5) == 5);
    CHECK(utf8_prefix(text, 6) == 6);
    CHECK(utf8_prefix(text, 7) == 6);
    CHECK(utf8_prefix(text, 9) == 6);
    // Every split reassembles the original text.
    for (std::size_t limit = 4; limit <= text.size(); ++limit) {
        std::string joined;
        for (std::string_view rest(text); !rest.empty();) {
            const auto size = utf8_prefix(rest, limit);
            REQUIRE(size > 0);
            joined.append(rest.substr(0, size));
            rest.remove_prefix(size);
        }
        CHECK(joined == text);
    }
    // An invalid run of continuation bytes is cut at the limit.
    CHECK(utf8_prefix(std::string(8, '\x80'), 4) == 4);
}

TEST_CASE("account verification distinguishes delayed identity from a different Google account") {
    using browser_detail::IdentityVerdict;
    using browser_detail::identity_verdict;
    json::object identity{{"ready", true}, {"identityReady", false}, {"email", ""}};
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::pending);
    identity["email"] = "expected@example.com";
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::pending);
    identity["identityReady"] = true;
    CHECK(identity_verdict(identity, "EXPECTED@example.com") == IdentityVerdict::verified);
    CHECK(identity_verdict(identity, " expected@example.com\r\n") == IdentityVerdict::verified);
    CHECK(identity_verdict(identity, "other@example.com") == IdentityVerdict::mismatch);
    identity["identityAmbiguous"] = true;
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::ambiguous);
    identity["identityAmbiguous"] = false; identity["email"] = "";
    CHECK(identity_verdict(identity, "expected@example.com") == IdentityVerdict::pending);
    identity["email"] = "one@example.com";
    CHECK(identity_verdict(identity, "") == IdentityVerdict::verified);
}

TEST_CASE("import cookie refresh retains only supported Google cookie fields") {
    json::array input{
        json::object{{"name", "SID"}, {"value", "test-only"}, {"domain", ".google.com"}, {"path", "/"}, {"secure", true}, {"httpOnly", true}, {"sameSite", "Lax"}, {"expires", 123456789.0}, {"priority", "High"}},
        json::object{{"name", "session"}, {"value", ""}, {"domain", "accounts.google.com"}, {"expires", -1}, {"sameSite", "invalid"}},
        json::object{{"name", "bad"}, {"value", "private"}, {"domain", "evilgoogle.com"}},
        json::object{{"name", "bad"}, {"value", "private"}, {"domain", "google.com.example.com"}},
        json::object{{"name", "bad"}, {"domain", "google.com"}},
        json::object{{"name", ""}, {"value", "private"}, {"domain", "google.com"}}
    };
    auto cookies = browser_detail::import_cookies(input);
    REQUIRE(cookies.size() == 2);
    const auto& persistent = cookies[0].as_object();
    CHECK(persistent.size() == 8);
    CHECK_FALSE(persistent.contains("priority"));
    CHECK_FALSE(persistent.contains("partitionKey"));
    CHECK(string_or(persistent, "domain") == ".google.com");
    const auto& session = cookies[1].as_object();
    CHECK_FALSE(session.contains("expires"));
    CHECK_FALSE(session.contains("sameSite"));
    CHECK(string_or(session, "value").empty());
}

TEST_CASE("import cookie refresh never downgrades partitioned cookies") {
    json::array input{
        json::object{{"name", "partitioned"}, {"value", "test-only"}, {"domain", ".google.com"}, {"partitionKey", json::object{{"topLevelSite", "https://google.com"}}}},
        json::object{{"name", "opaque"}, {"value", "test-only"}, {"domain", "accounts.google.com"}, {"partitionKeyOpaque", true}},
        json::object{{"name", "unknown-key"}, {"value", "test-only"}, {"domain", ".google.com"}, {"partitionKey", nullptr}},
        json::object{{"name", "__Host-test"}, {"value", "test-only"}, {"domain", "accounts.google.com"}, {"path", "/"}, {"secure", true}, {"partitionKeyOpaque", false}}
    };
    std::size_t skipped = 99;
    auto cookies = browser_detail::import_cookies(input, &skipped);
    REQUIRE(cookies.size() == 1);
    CHECK(skipped == 3);
    CHECK(string_or(cookies[0].as_object(), "name") == "__Host-test");
    CHECK(string_or(cookies[0].as_object(), "domain") == "accounts.google.com");
    CHECK(bool_or(cookies[0].as_object(), "secure"));
    browser_detail::import_cookies({}, &skipped);
    CHECK(skipped == 0);
}

TEST_CASE("HTTP 200 RPC status envelopes retain scheduler error categories") {
    for (const auto& [wire, status] : std::vector<std::pair<std::string, int>>{
        {"[[[8,\"quota exhausted\"]]]", 429},
        {"[null,[7,\"model permission denied\"]]", 403},
        {"[,[16,\"credentials expired\"]]", 401},
        {")]}'\n[[[8,\"try later\",[]]]]", 429},
        {"{\"error\":{\"code\":429,\"message\":\"quota\"}}", 429},
        {"{\"error\":{\"status\":\"UNAUTHENTICATED\"}}", 401}
    }) {
        auto error = browser_detail::rpc_text_error(wire, 17);
        REQUIRE(error.has_value()); CHECK(error->status == status); CHECK(error->retry_after == 17);
        CHECK(std::string(error->what()).find("credentials expired") == std::string::npos);
    }
    CHECK_FALSE(browser_detail::rpc_text_error("[[[0,\"success\"]]]"));
    CHECK_FALSE(browser_detail::rpc_text_error("[[[8,\"partial"));
    auto http = browser_detail::rpc_status_error(503, 400000);
    REQUIRE(http); CHECK(http->status == 503); CHECK(http->retry_after == 86400);
}

TEST_CASE("AI Studio internal errors are told apart from other server errors") {
    auto internal = browser_detail::rpc_text_error(R"([[[13,"Internal error encountered."]]])");
    REQUIRE(internal.has_value());
    CHECK(internal->status == 500);
    CHECK(internal->code == "upstream_internal");
    auto other = browser_detail::rpc_text_error(R"([[[13,"Backend failed"]]])");
    REQUIRE(other.has_value());
    CHECK(other->code == "upstream_error");
}

TEST_CASE("generated tool arguments cannot masquerade as RPC errors") {
    json::array part{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     json::array{"tool", json::object{{"error", {{"code", 8}, {"message", "user data"}}}, {"tuple", json::array{16, "user value"}}}}};
    json::array content{json::array{part}, "model"};
    json::array candidate{json::value(content)};
    json::array wire{json::value(json::array{json::value(candidate)})};
    CHECK_FALSE(browser_detail::rpc_error(wire));
}

TEST_CASE("content-less grounding chunks cannot masquerade as RPC errors", "[grounding]") {
    // A finish or grounding chunk carries a candidate without content.
    for (auto wire : {R"([[[null,1,null,null,[],null,null,null,null,[[8,"source title"],[16,"segment"]]]],null,[1,2,3]])",
                      R"([[[null,null,null,null,null,null,null,null,null,[3,"query"]]]])",
                      R"([[[[null,"model"],1,null,null,null,[5,"metadata"]]]])"})
        CHECK_FALSE(browser_detail::rpc_text_error(wire));
    // Deeper wrapping of a real status envelope is still recognized.
    auto wrapped = browser_detail::rpc_text_error(R"([[[[8,"quota exhausted"]]]])");
    REQUIRE(wrapped.has_value());
    CHECK(wrapped->status == 429);
}

TEST_CASE("RPC recovery metadata distinguishes route glitches quota windows and ordinary errors", "[upstream-policy]") {
    auto route = browser_detail::rpc_text_error(R"([[5,"Ambiguous request for service '' and method '/GenerativeService.GenerateContent'"]])");
    REQUIRE(route); CHECK(route->status == 404); CHECK(route->code == "transient_rpc_route");
    CHECK(std::string(route->what()).find("GenerativeService") == std::string::npos);
    for (auto message : {"Ambiguous request for service ListModels", "GenerateContent model was not found"}) {
        auto ordinary = browser_detail::rpc_status_error(404, 0, message);
        REQUIRE(ordinary); CHECK(ordinary->code == "upstream_error");
    }
    for (const auto& [message, scope] : std::vector<std::pair<std::string, std::string>>{
             {"Quota per minute exceeded", "minute"}, {"quota PerDay exceeded", "day"}, {"quota exhausted", "unknown"}}) {
        auto limited = browser_detail::rpc_error(json::object{{"error", {{"code", 429}, {"message", message}}}}, 900);
        REQUIRE(limited); CHECK(limited->quota_scope == scope); CHECK(limited->retry_after == 900);
        CHECK(std::string(limited->what()).find(message) == std::string::npos);
    }
    auto status = browser_detail::rpc_error(json::object{{"error", {{"status", "NOT_FOUND"}, {"message", "Ambiguous request for service GenerateContent"}}}});
    REQUIRE(status); CHECK(status->code == "transient_rpc_route");
    auto details = browser_detail::rpc_text_error(R"({"error":{"code":429,"message":"Resource exhausted","details":[{"quotaId":"GenerateRequestsPerMinutePerModel"}]}})");
    REQUIRE(details); CHECK(details->quota_scope == "minute");
    auto wire_details = browser_detail::rpc_text_error(R"([[8,"Resource exhausted",[{"quotaId":"GenerateRequestsPerDayPerModel"}]]])");
    REQUIRE(wire_details); CHECK(wire_details->quota_scope == "day");
}

TEST_CASE("snapshot hash follows encoded contents including expanded attachments") {
    const auto wire = json::parse(R"([null,[[[[null,"thought",null,null,null,null,null,null,null,null,null,null,1],[null,""],[null,null,["image/png","YWJj"]],[null,null,null,null,null,null,null,[1,"ignored code"]],[null,null,null,null,null,null,null,null,[1,"ignored output"]],[null,null,null,null,null,null,null,null,null,null,["fn"]]],"model"]],null,null,null,[[[null,"excluded system"]],"user"]])").as_array();
    REQUIRE(browser_detail::wire_content_hash(wire) == sha256("thought  YWJj   "));
    Request request;
    request.model = "test";
    request.gemini = json::parse(R"({"contents":[{"role":"user","parts":[{"functionResponse":{"name":"fn","response":{},"parts":[{"text":"attached"},{"inlineData":{"mimeType":"image/png","data":"YWJj"}}]}}]}]})").as_object();
    REQUIRE(browser_detail::wire_content_hash(encode_request(request, {}, "")) == sha256(" attached YWJj"));
}

TEST_CASE("verified cookie write-back keeps unexported cookies and refuses signed-out exports", "[cookie-refresh]") {
    const json::array stored{
        json::object{{"name", "SID"}, {"value", "old"}, {"domain", ".google.com"}},
        json::object{{"name", "SAPISID"}, {"value", "old"}, {"domain", ".google.com"}},
        json::object{{"name", "partitioned"}, {"value", "keep"}, {"domain", ".google.com"},
                     {"partitionKey", json::object{{"topLevelSite", "https://google.com"}}}},
        json::object{{"name", "VISITOR"}, {"value", "keep"}, {"domain", ".youtube.com"}}};
    const json::array browser{
        json::object{{"name", "SID"}, {"value", "new"}, {"domain", ".google.com"}, {"httpOnly", true}},
        json::object{{"name", "SAPISID"}, {"value", "new"}, {"domain", ".google.com"}},
        json::object{{"name", "__Secure-1PSIDTS"}, {"value", "rotated"}, {"domain", ".google.com"}},
        json::object{{"name", "tracker"}, {"value", "drop"}, {"domain", ".example.com"}}};
    auto merged = browser_detail::refreshed_cookies(stored, browser);
    std::map<std::string, std::string> values;
    for (const auto& cookie : merged)
        values[string_or(cookie.as_object(), "name")] = string_or(cookie.as_object(), "value");
    CHECK(values == std::map<std::string, std::string>{
        {"SID", "new"}, {"SAPISID", "new"}, {"__Secure-1PSIDTS", "rotated"}, {"partitioned", "keep"}, {"VISITOR", "keep"}});
    // A signed-out browser keeps no API/session pair: never persist it.
    CHECK(browser_detail::refreshed_cookies(stored, json::array{browser[2]}).empty());
    CHECK(browser_detail::refreshed_cookies(stored, json::array{browser[0]}).empty());
    CHECK(browser_detail::refreshed_cookies(stored, json::array{browser[1]}).empty());
}

TEST_CASE("cookie injection drops volatile conflicting cookies but never the whole state", "[cookie-refresh]") {
    const json::array stored{
        json::object{{"name", "SID"}, {"value", "keep"}, {"domain", ".google.com"}, {"secure", true}, {"expires", 0}},
        json::object{{"name", "OSID"}, {"value", "drop"}, {"domain", "aistudio.google.com"}},
        json::object{{"name", "__Secure-OSID"}, {"value", "drop"}, {"domain", "aistudio.google.com"}},
        json::object{{"name", "SIDCC"}, {"value", "drop"}, {"domain", ".google.com"}},
        json::object{{"name", "__Secure-1PSIDCC"}, {"value", "drop"}, {"domain", ".google.com"}},
        json::object{{"name", "__Secure-3PSIDCC"}, {"value", "drop"}, {"domain", ".google.com"}},
        json::object{{"name", "SAPISID"}, {"value", "keep"}, {"domain", ".google.com"}, {"sameParty", true}}};
    auto injected = browser_detail::injected_cookies(stored);
    std::vector<std::string> names;
    for (const auto& cookie : injected) names.push_back(string_or(cookie.as_object(), "name"));
    CHECK(names == std::vector<std::string>{"SID", "SAPISID"});
    // Unsupported fields and non-positive expiry are not passed to the browser.
    CHECK_FALSE(injected[0].as_object().contains("expires"));
    CHECK_FALSE(injected[1].as_object().contains("sameParty"));
    CHECK(injected[0].as_object().at("secure").as_bool());
    // A state made only of these cookies is injected unchanged.
    CHECK(browser_detail::injected_cookies(json::array{stored[1], stored[3]}).size() == 2);
}
