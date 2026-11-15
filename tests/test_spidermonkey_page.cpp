// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/spidermonkey.hpp"
#include "browser_generation_fixture.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
using namespace ais2api::spidermonkey_detail;
namespace {
std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const char character : bytes) {
        out += digits[static_cast<unsigned char>(character) >> 4];
        out += digits[static_cast<unsigned char>(character) & 15];
    }
    return out;
}
std::string error_code(const std::function<void()> &body) {
    try {
        body();
    } catch (const Error &error) {
        return std::to_string(error.status) + " " + error.code;
    }
    return "no error";
}
Request request_with(std::string_view contents) {
    Request request;
    request.gemini["contents"] = json::parse(contents);
    return request;
}
} // namespace

TEST_CASE("The page's inline data gives its API key and build globals", "[spidermonkey]") {
    const std::string html = R"(<script>window.WIZ_global_data = {"A":"b","WIu0Nc":"page-key","Z":1};</script>
<script>window["_F_toggles_default_MakerSuite"] = [1, 0x2a, -7,oops, 4];var _F_cssRowKey = 'boq.css';var _F_combinedSignature = 'sig-1';</script>)";
    CHECK(page_api_key(html) == "page-key");
    CHECK(page_api_key("<html></html>").empty());
    CHECK(page_api_key(R"("WIu0Nc":"unterminated)").empty());
    CHECK(page_globals(html) == json::object{{"_F_toggles_default_MakerSuite", json::array{1, 42, -7, 4}},
                                             {"_F_cssRowKey", "boq.css"}, {"_F_combinedSignature", "sig-1"}});
    CHECK(page_globals("<html></html>").empty());
}

TEST_CASE("A policy that requires Trusted Types for scripts is recognised", "[spidermonkey]") {
    CHECK(requires_trusted_types({"script-src 'self'; Require-Trusted-Types-For  'script';report-uri /csp"}));
    CHECK(requires_trusted_types({"default-src 'none'", "object-src 'none', require-trusted-types-for 'style' 'script'"}));
    CHECK_FALSE(requires_trusted_types({"script-src 'self'; trusted-types angular"}));
    CHECK_FALSE(requires_trusted_types({"require-trusted-types-for"}));
    CHECK_FALSE(requires_trusted_types({}));
}

TEST_CASE("A visit id is a prefixed Base64 UUID", "[spidermonkey]") {
    const auto id = new_visit_id();
    REQUIRE(id.size() == 3 + 48);
    CHECK(id.starts_with("v1_"));
    CHECK(id != new_visit_id());
}

TEST_CASE("A logging context becomes protobuf fields by position", "[spidermonkey]") {
    // Field 1 varint 150, field 2 "testing", field 4 true, field 5 varint 0.
    CHECK(hex(encode_jspb_fields(R"([150,"testing",null,true,0])")) == "0896011207" "74657374696e67" "2001" "2800");
    CHECK(hex(encode_jspb_fields(")]}'\n[,\"x\"]")) == "120178");
    // A negative number is the ten-byte varint of its two's complement.
    CHECK(hex(encode_jspb_fields("[-1]")) == "08ffffffffffffffffff01");
    CHECK(encode_jspb_fields("[]").empty());
    CHECK(error_code([] { encode_jspb_fields(R"({"a":1})"); }) == "502 browser_incompatible");
    CHECK(error_code([] { encode_jspb_fields("[[1]]"); }) == "502 browser_incompatible");
    CHECK(error_code([] { encode_jspb_fields("[1.5]"); }) == "502 browser_incompatible");
    CHECK(error_code([] { encode_jspb_fields("not json"); }) == "502 browser_incompatible");
}

TEST_CASE("document.cookie shows the page's script-visible cookies", "[spidermonkey]") {
    json::object expired{{"name", "old"}, {"value", "0"}, {"domain", ".google.com"}, {"path", "/"}, {"expires", 500}};
    const json::array stored{json::object{{"name", "SAPISID"}, {"value", "a"}, {"domain", ".google.com"}, {"path", "/"}},
                             json::object{{"name", "SID"}, {"value", "b"}, {"domain", ".google.com"}, {"path", "/"}, {"httpOnly", true}},
                             json::object{{"name", "local"}, {"value", "c"}, {"domain", "aistudio.google.com"}},
                             json::object{{"name", "elsewhere"}, {"value", "d"}, {"domain", "accounts.google.com"}, {"path", "/"}},
                             json::object{{"name", "deep"}, {"value", "e"}, {"domain", ".google.com"}, {"path", "/verify"}},
                             expired};
    CHECK(document_cookie(stored, 1000) == "SAPISID=a; local=c");
    CHECK(document_cookie(stored, 100) == "SAPISID=a; local=c; old=0");
    CHECK(document_cookie({}, 1000).empty());
}

TEST_CASE("Catalogue responses are read as the page reads them", "[spidermonkey]") {
    CHECK(benefit_tier("[]") == "free");
    CHECK(benefit_tier("[null,1]") == "free");
    CHECK(benefit_tier("[1]") == "pro");
    CHECK(benefit_tier(")]}'\n[2]") == "ultra");
    CHECK(benefit_tier("[3,\"x\"]") == "plus");
    CHECK(benefit_tier("[4]") == "unknown");
    CHECK(benefit_tier("[\"1\"]") == "unknown");
    CHECK(benefit_tier("{}") == "unknown");
    CHECK(benefit_tier("") == "unknown");

    const auto models = discovered_models(
        R"([[["models/gemini-test",null,null,"Gemini Test","A model",1000,200,["generateContent"]],["tunedModels/x"],["models/bare"],7]])");
    REQUIRE(models.size() == 2);
    CHECK(models[0].as_object() == json::object{{"id", "gemini-test"}, {"name", "models/gemini-test"}, {"displayName", "Gemini Test"},
                                                {"description", "A model"}, {"inputTokenLimit", 1000}, {"outputTokenLimit", 200},
                                                {"supportedGenerationMethods", json::array{"generateContent"}}, {"source", "live"}});
    CHECK(models[1].as_object() == json::object{{"id", "bare"}, {"name", "models/bare"}, {"displayName", "bare"}, {"description", ""},
                                                {"inputTokenLimit", 0}, {"outputTokenLimit", 0},
                                                {"supportedGenerationMethods", json::array{}}, {"source", "live"}});
    CHECK(error_code([] { discovered_models("[7,\"Region not supported.\"]"); }) == "502 browser_incompatible");
    CHECK(error_code([] { discovered_models("<html>"); }) == "502 browser_incompatible");
}

TEST_CASE("Requests the page makes say how their target relates to it", "[spidermonkey]") {
    CHECK(fetch_site("https://aistudio.google.com/generate_204") == "same-origin");
    CHECK(fetch_site("https://console.cloud.google.com/onboarding/auth_warmup?origin=x") == "same-site");
    CHECK(fetch_site("https://google.com:443/") == "same-site");
    CHECK(fetch_site("https://www.gstatic.com/x.js") == "cross-site");
    CHECK(fetch_site("https://notgoogle.com/") == "cross-site");
}

TEST_CASE("The prompt box holds the text of the last user turn", "[spidermonkey]") {
    CHECK(page_prompt(request_with(R"([{"role":"user","parts":[{"text":"first"}]},{"role":"model","parts":[{"text":"reply"}]},
                                       {"role":"user","parts":[{"text":"second "},{"inlineData":{"mimeType":"image/png","data":"AA=="}},{"text":"part"}]}])")) ==
          "second part");
    CHECK(page_prompt(request_with(R"([{"parts":[{"text":"no role"}]}])")) == "no role");
    // A function response or a model turn last: nothing is typed.
    CHECK(page_prompt(request_with(R"([{"role":"user","parts":[{"text":"q"}]},{"role":"model","parts":[{"text":"prefill"}]}])")).empty());
    CHECK(page_prompt(request_with(R"([{"role":"user","parts":[{"functionResponse":{"name":"f","response":{}}}]}])")).empty());
    CHECK(page_prompt(request_with("[]")).empty());
    CHECK(page_prompt(Request{}).empty());
}

TEST_CASE("An account's page runs a conversation again with Rerun", "[spidermonkey]") {
    const auto two_turns = request_with(R"([{"role":"user","parts":[{"text":"first"}]},{"role":"model","parts":[{"text":"reply"}]},
                                            {"role":"user","parts":[{"text":"second"}]}])");
    PageRuns runs;
    CHECK_FALSE(runs.rerun("account-a", two_turns));
    CHECK(runs.rerun("account-a", two_turns));
    // Another account's page never ran it.
    CHECK_FALSE(runs.rerun("account-b", two_turns));
    // A different conversation is a new run.
    CHECK_FALSE(runs.rerun("account-a", request_with(R"([{"role":"user","parts":[{"text":"other"}]}])")));
    // A model turn last is not run on the page, nor remembered.
    const auto prefill = request_with(R"([{"role":"user","parts":[{"text":"q"}]},{"role":"model","parts":[{"text":"prefill"}]}])");
    CHECK_FALSE(runs.rerun("account-a", prefill));
    CHECK_FALSE(runs.rerun("account-a", prefill));
    CHECK_FALSE(runs.rerun("account-a", Request{}));
    CHECK(user_turns(two_turns) == 2);
    CHECK(user_turns(request_with(R"([{"parts":[{"text":"no role"}]}])")) == 1);
    CHECK(user_turns(Request{}) == 0);
    // Past 1000 conversations, the oldest is forgotten.
    for (int index = 0; index < 1000; ++index) runs.rerun("account-c", request_with(R"([{"role":"user","parts":[{"text":"q)" + std::to_string(index) + R"("}]}])"));
    CHECK_FALSE(runs.rerun("account-a", two_turns));
}

TEST_CASE("The page's navigation timing follows the measured load", "[spidermonkey]") {
    using namespace std::chrono_literals;
    const auto number = [](const json::object &entry, std::string_view name) { return entry.at(name).as_int64(); };
    std::mt19937_64 random(7);
    for (int round = 0; round < 200; ++round) {
        PageLoad load{.url = "https://aistudio.google.com/prompts/new_chat",
                      .protocol = "h2",
                      .new_connection = true,
                      .lookup = 3ms,
                      .connect = 20ms,
                      .tunnel = 150ms,
                      .end = 900ms,
                      .decoded = 100000};
        auto [entry, dom_loading] = navigation_timing(load, random);
        CHECK(entry.at("name") == "https://aistudio.google.com/prompts/new_chat");
        CHECK(entry.at("nextHopProtocol") == "h2");
        CHECK(number(entry, "startTime") == 0);
        const auto fetch = number(entry, "fetchStart");
        CHECK((fetch == 0 || fetch == 1));
        CHECK(number(entry, "domainLookupStart") >= fetch);
        CHECK(number(entry, "domainLookupEnd") == number(entry, "domainLookupStart") + 3);
        CHECK(number(entry, "connectStart") - number(entry, "domainLookupEnd") <= 1);
        CHECK(number(entry, "connectEnd") == number(entry, "connectStart") + 20);
        CHECK(number(entry, "secureConnectionStart") == number(entry, "connectEnd"));
        const auto request = number(entry, "requestStart");
        CHECK(request - number(entry, "connectEnd") >= 1);
        CHECK(request - number(entry, "connectEnd") <= 4);
        // The proxy's answer to CONNECT shows before the response headers.
        CHECK(number(entry, "finalResponseHeadersStart") == std::max<std::int64_t>(request, number(entry, "connectEnd") + 150));
        CHECK(number(entry, "responseStart") == 900);
        CHECK(number(entry, "responseEnd") == 900);
        CHECK(std::abs(number(entry, "encodedBodySize") - 19430) <= 6);
        CHECK(number(entry, "transferSize") == number(entry, "encodedBodySize") + 300);
        CHECK(number(entry, "decodedBodySize") == 100000);
        CHECK(dom_loading >= 900);
        CHECK(dom_loading <= 903);

        document_timing(entry, 1200ms, random);
        const auto interactive = number(entry, "domInteractive");
        CHECK(interactive >= 1400);
        CHECK(number(entry, "domContentLoadedEventStart") == interactive);
        CHECK(number(entry, "domContentLoadedEventEnd") - interactive >= 0);
        CHECK(number(entry, "domContentLoadedEventEnd") - interactive <= 4);
        CHECK(number(entry, "domComplete") > interactive);
        CHECK(number(entry, "loadEventStart") == number(entry, "domComplete"));
        CHECK(number(entry, "loadEventEnd") - number(entry, "loadEventStart") <= 1);
        CHECK(number(entry, "duration") == number(entry, "loadEventEnd"));
    }
    // A reused connection: no lookup or connect, and the request soon after
    // the fetch.
    PageLoad reused{.url = "https://aistudio.google.com/", .protocol = "h2", .end = 5ms, .decoded = 10};
    const auto [entry, dom_loading] = navigation_timing(reused, random);
    const auto fetch = number(entry, "fetchStart");
    for (const std::string_view name : {"domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd", "secureConnectionStart"})
        CHECK(number(entry, name) == fetch);
    CHECK(number(entry, "requestStart") - fetch >= 2);
    CHECK(number(entry, "requestStart") - fetch <= 13);
    // A response faster than the request reads one millisecond later.
    CHECK(number(entry, "responseEnd") == std::max<std::int64_t>(5, number(entry, "requestStart") + 1));
    CHECK(number(entry, "finalResponseHeadersStart") == number(entry, "responseEnd"));
    CHECK(dom_loading >= number(entry, "responseEnd"));
}

TEST_CASE("document.lastModified is the load time in the page's zone", "[spidermonkey]") {
    using namespace std::chrono;
    // 2026-03-04 05:06:07 UTC.
    const auto time = sys_days{year{2026} / March / 4} + hours{5} + minutes{6} + seconds{7};
    CHECK(last_modified(time, "UTC") == "03/04/2026 05:06:07");
    CHECK(last_modified(time, "") == "03/04/2026 05:06:07");
    CHECK(last_modified(time, "No/Such_Zone") == "03/04/2026 05:06:07");
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    CHECK(last_modified(time, "Asia/Tokyo") == "03/04/2026 14:06:07");
    // Across midnight, backwards.
    CHECK(last_modified(time, "America/Los_Angeles") == "03/03/2026 21:06:07");
#endif
}

TEST_CASE("The page names its signed-in account", "[spidermonkey]") {
    CHECK(page_email(R"(window.WIZ_global_data = {"S06Grb":"1","oPEP7c":"someone@example.com","W3Yyqf":"2"};)") == "someone@example.com");
    // MSVC 14.44 cannot take a raw string holding \" as a macro argument.
    constexpr std::string_view quoted = R"({"oPEP7c":"quote\"d@example.com"})";
    CHECK(page_email(quoted) == "quote\"d@example.com");
    CHECK(page_email(R"({"oPEP7c":"unterminated)").empty());
    CHECK(page_email("<html></html>").empty());
}

TEST_CASE("A redirect's Location resolves against the request", "[spidermonkey]") {
    constexpr std::string_view base = "https://aistudio.google.com/u/1/prompts/new_chat?model=x";
    CHECK(resolve_location(base, "https://accounts.google.com/ServiceLogin?continue=y") == "https://accounts.google.com/ServiceLogin?continue=y");
    CHECK(resolve_location(base, "//console.cloud.google.com/a") == "https://console.cloud.google.com/a");
    CHECK(resolve_location(base, "/prompts/new_chat") == "https://aistudio.google.com/prompts/new_chat");
    CHECK(resolve_location(base, "?model=y") == "https://aistudio.google.com/u/1/prompts/new_chat?model=y");
    CHECK(resolve_location(base, "other") == "https://aistudio.google.com/u/1/prompts/other");
    CHECK(resolve_location("https://aistudio.google.com", "a") == "https://aistudio.google.com/a");
    CHECK(resolve_location(base, "").empty());
}

TEST_CASE("AI Studio's refusals and RPC failures are told apart", "[spidermonkey]") {
    const auto failure = [](long status, std::string_view body) {
        const auto error = page_rpc_failure("GetLoggingContext", status, body);
        return error ? std::to_string(error->status) + " " + error->code : std::string("none");
    };
    CHECK(failure(200, R"([7,"Region not supported."])") == "403 region_blocked");
    CHECK(failure(403, ")]}'\n[7,\"Account not supported.\"]") == "403 account_unsupported");
    CHECK(failure(200, R"([150,"testing",null,true])") == "none");
    CHECK(failure(200, "") == "none");
    CHECK(failure(401, R"([16,"Request had invalid authentication credentials."])") == "401 upstream_auth");
    CHECK(failure(429, "") == "429 rate_limit");
    CHECK(failure(302, "") == "502 upstream_error");
}

TEST_CASE("The page counts the tokens of its prompt box", "[spidermonkey]") {
    constexpr std::string_view expected = R"(["models/gemini-3.1-flash-lite",[[[[null,"hi \"there\""]],"user"]]])";
    CHECK(count_tokens_body("models/gemini-3.1-flash-lite", "hi \"there\"") == expected);
    CHECK(tier_header("plus") == "TIER0");
    CHECK(tier_header("pro") == "TIER1");
    CHECK(tier_header("ultra") == "TIER2");
    CHECK(tier_header("free").empty());
    CHECK(tier_header("unknown").empty());
}

TEST_CASE("Requests carry Firefox's headers in Firefox's order", "[spidermonkey]") {
    const PageHeaders page{"Mozilla/5.0 Firefox/152.0", "de-DE,de;q=0.9", "gzip, deflate, br, zstd"};
    const auto names = [](const Headers &headers) {
        std::string joined;
        for (const auto &[name, value] : headers) joined += (joined.empty() ? "" : ",") + name;
        return joined;
    };
    const auto value = [](const Headers &headers, std::string_view name) {
        for (const auto &[key, text] : headers)
            if (key == name) return text;
        return std::string("absent");
    };
    const auto document = document_headers(page);
    CHECK(names(document) == "User-Agent,Accept,Accept-Language,Accept-Encoding,Cookie,Upgrade-Insecure-Requests,Sec-Fetch-Dest,"
                             "Sec-Fetch-Mode,Sec-Fetch-Site,Sec-Fetch-User,Priority,TE");
    CHECK(value(document, "Accept-Language") == "de-DE,de;q=0.9");
    CHECK(value(document, "Cookie").empty());
    CHECK(value(document, "Priority") == "u=0, i");

    const auto frame = frame_headers(page, "https://console.cloud.google.com/onboarding/auth_warmup");
    CHECK(value(frame, "Sec-Fetch-Dest") == "iframe");
    CHECK(value(frame, "Sec-Fetch-Site") == "same-site");
    CHECK(value(frame, "Referer") == "https://aistudio.google.com/");
    const auto image = image_headers(page, "https://aistudio.google.com/generate_204");
    CHECK(value(image, "Sec-Fetch-Site") == "same-origin");
    CHECK(value(image, "Priority") == "u=5, i");
    CHECK(value(image, "Accept") == "image/avif,image/jxl,image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5");
    CHECK(value(script_headers(page, "https://www.google.com/js/bg/x.js"), "Sec-Fetch-Dest") == "script");

    // The page's grpc-web XHRs as Firefox sends them (Camoufox 158,
    // 2026-11-14): no Priority, and the body's length before Origin.
    RpcContext rpc{"page-key", "2", "SAPISIDHASH 1_a"};
    const auto plain = rpc_headers(page, rpc);
    CHECK(names(plain) == "User-Agent,Accept,Accept-Language,Accept-Encoding,Referer,X-Goog-Api-Key,X-Goog-AuthUser,Authorization,"
                          "Content-Type,X-User-Agent,Content-Length,Origin,Cookie,Sec-Fetch-Dest,Sec-Fetch-Mode,Sec-Fetch-Site,TE");
    CHECK(value(plain, "Content-Length").empty());
    CHECK(value(plain, "Accept") == "*/*");
    rpc.visit_id = "v1_x";
    rpc.extension = "CAE=";
    rpc.tier = "TIER1";
    const auto generate = rpc_headers(page, rpc);
    CHECK(names(generate) == "User-Agent,Accept,Accept-Language,Accept-Encoding,Referer,X-AIStudio-G1-Tier,X-AIStudio-Visit-Id,"
                             "X-Goog-Ext-519733851-Bin,X-Goog-Api-Key,X-Goog-AuthUser,Authorization,Content-Type,X-User-Agent,"
                             "Content-Length,Origin,Cookie,Sec-Fetch-Dest,Sec-Fetch-Mode,Sec-Fetch-Site,TE");
    CHECK(value(generate, "X-Goog-AuthUser") == "2");
    CHECK(value(generate, "X-AIStudio-G1-Tier") == "TIER1");
    CHECK(value(generate, "Content-Type") == "application/json+protobuf");

    // Waa/Create puts Authorization before X-Goog-AuthUser and takes no
    // page values.
    const auto waa = waa_headers(page, rpc);
    CHECK(names(waa) == "User-Agent,Accept,Accept-Language,Accept-Encoding,Referer,X-Goog-Api-Key,Authorization,X-Goog-AuthUser,"
                        "Content-Type,X-User-Agent,Content-Length,Origin,Cookie,Sec-Fetch-Dest,Sec-Fetch-Mode,Sec-Fetch-Site,TE");
    CHECK(value(waa, "X-Goog-Api-Key") == "page-key");
}

TEST_CASE("An identity's configuration becomes the host's values", "[spidermonkey]") {
    const json::object config{{"navigator.userAgent", "Mozilla/5.0 (X11; Linux x86_64; rv:152.0) Gecko/20100101 Firefox/152.0"},
                              {"navigator.hardwareConcurrency", 8}, {"navigator.language", "de-DE"},
                              {"headers.Accept-Language", "de-DE,de;q=0.9"}, {"screen.width", 1920}, {"screen.height", 1080},
                              {"screen.availLeft", 12}, {"window.outerWidth", 1600}, {"window.outerHeight", 900},
                              {"window.devicePixelRatio", 1.5}, {"timezone", "Europe/Berlin"}, {"webGl:vendor", "x"}, {"fonts", json::array{"Arial"}}};
    const auto host = host_identity(config);
    CHECK(host.user_agent == "Mozilla/5.0 (X11; Linux x86_64; rv:152.0) Gecko/20100101 Firefox/152.0");
    CHECK(host.accept_language == "de-DE,de;q=0.9");
    CHECK(host.profile.locale == "de-DE");
    CHECK(host.profile.timezone == "Europe/Berlin");
    CHECK(host.profile.has_focus);
    CHECK(host.profile.navigator == json::object{{"userAgent", "Mozilla/5.0 (X11; Linux x86_64; rv:152.0) Gecko/20100101 Firefox/152.0"},
                                                 {"hardwareConcurrency", 8}, {"language", "de-DE"}, {"doNotTrack", "unspecified"},
                                                 {"globalPrivacyControl", false}});
    CHECK(host.profile.screen == json::object{{"width", 1920}, {"height", 1080}, {"availLeft", 12}, {"availTop", 0}});
    CHECK(host.profile.window == json::object{{"outerWidth", 1600}, {"outerHeight", 900}, {"devicePixelRatio", 1.5},
                                              {"innerWidth", 1600.0}, {"innerHeight", 815.0}});
    // The identity's own inner size stands.
    const auto sized = host_identity({{"window.outerWidth", 1600}, {"window.outerHeight", 900}, {"window.innerWidth", 1584}, {"window.innerHeight", 790}});
    CHECK(sized.profile.window == json::object{{"outerWidth", 1600}, {"outerHeight", 900}, {"innerWidth", 1584}, {"innerHeight", 790}});

    const auto bare = host_identity({});
    CHECK(bare.profile.locale == "en-US");
    CHECK(bare.accept_language == "en-US,en;q=0.5");
    CHECK(bare.user_agent.empty());
    CHECK(bare.profile.window.empty());
}

namespace {
// Feeds scripted relay states to the production generation loop.
struct Relay {
    Request request = normalize_gemini("gemini-4-synthetic",
                                       json::parse(R"({"contents":[{"parts":[{"text":"ping"}]}]})").as_object(), false,
                                       Config{});
    std::vector<json::value> polls;
    std::vector<std::uint64_t> acknowledgments;
    std::size_t position = 0;
    std::string text;

    void run() {
        browser_detail::GenerationIO io;
        io.encode = [this](std::string_view checkpoint) {
            return json::serialize(encode_request(request, {}, "!synthetic-snapshot", "UTC", checkpoint));
        };
        io.start = [](std::string_view, const std::function<void()> &check) { check(); };
        io.next = [this](std::uint64_t acknowledgment) {
            acknowledgments.push_back(acknowledgment);
            return polls.at(position++);
        };
        browser_detail::run_generation(request, Limits{}, true, io, [this](GenerationEvent event) {
            if (event.kind != GenerationEvent::Kind::chunk) return;
            if (const auto candidates = event.data.if_contains("candidates"))
                for (const auto &candidate : candidates->as_array())
                    if (const auto content = candidate.as_object().if_contains("content"))
                        for (const auto &part : content->at("parts").as_array()) text += string_or(part.as_object(), "text");
        }, {});
    }
};
} // namespace

TEST_CASE("SpiderMonkey relay states drive the shared generation loop", "[spidermonkey]") {
    // Built from the gateway's unsigned counters, these numbers used to be
    // uint64, and the first chunk failed with "Browser relay sequence mismatch".
    CHECK(relay_chunk(1, "x").at("sequence").is_int64());
    CHECK(relay_state({.status = 200, .retry_after = 5}).at("status").is_int64());
    CHECK(relay_state({.status = 200, .retry_after = 5}).at("retryAfter").is_int64());

    const auto body = generation_fixture::stream(generation_fixture::reply("pong", 1, 1));
    const RelayState open{.status = 200, .benefit = "pro"};
    Relay relay;
    relay.polls.push_back(relay_state(open)); // nothing has arrived yet
    for (const auto &[sequence, bytes] : {std::pair{1, std::string_view(body).substr(0, 7)}, std::pair{2, std::string_view(body).substr(7)}}) {
        auto state = relay_state(open);
        state["pending"] = relay_chunk(static_cast<std::uint64_t>(sequence), bytes);
        relay.polls.push_back(std::move(state));
    }
    auto finished = open;
    finished.done = true;
    relay.polls.push_back(relay_state(finished));

    relay.run();
    CHECK(relay.position == relay.polls.size());
    CHECK(relay.acknowledgments == std::vector<std::uint64_t>{0, 0, 1, 2});
    CHECK(relay.text == "pong");
}

TEST_CASE("SpiderMonkey relay states carry Retry-After into upstream errors", "[spidermonkey]") {
    Relay relay;
    relay.polls.push_back(relay_state(
        {.done = true, .status = 429, .retry_after = 30, .error = "upstream_http_429", .error_body = "", .benefit = "pro"}));
    try {
        relay.run();
        FAIL("a 429 relay state must fail the generation");
    } catch (const Error &error) {
        CHECK(error.status == 429);
        CHECK(error.code == "rate_limit");
        CHECK(error.retry_after == 30);
    }
}
