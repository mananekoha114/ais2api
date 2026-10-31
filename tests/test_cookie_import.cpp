// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/cookie_import.hpp"
#include "ais2api/usage.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <set>
#include <thread>
using namespace ais2api;
using namespace std::chrono_literals;
namespace {
struct ImportBackend : GenerationBackend {
    std::vector<json::object> replies;
    std::vector<Account> candidates;
    std::set<std::size_t> unauthorized;
    std::chrono::milliseconds delay{};
    int fail_at = -1;
    bool closed = false;
    bool expand_state = false;
    std::string failure_code = "secret";
    void generate(const Request &, const Account &, const EventSink &, std::stop_token) override {}
    json::array models(const Account &, std::stop_token = {}) override { return {}; }
    json::object probe(const Account &, std::stop_token = {}) override { return {}; }
    json::object probe_import(Account &a, std::stop_token token = {}) override {
        check_cancel(token);
        std::this_thread::sleep_for(delay);
        candidates.push_back(a);
        if (expand_state) a.storage_state.at("cookies").as_array()[0].as_object()["value"] = std::string(1024 * 1024, 'x');
        if (static_cast<int>(candidates.size()) == fail_at)
            throw Error(503, failure_code, "private-cookie-backend-secret");
        if (unauthorized.contains(candidates.size()))
            throw Error(401, "login", "private-cookie");
        if (candidates.size() > replies.size()) throw Error(401, "login", "private-cookie");
        return replies[candidates.size() - 1];
    }
    json::object diagnostics() override { return {}; }
    void close() override { closed = true; }
};
json::object verified(std::string email) {
    return {{"ready", true}, {"login", false}, {"identityAmbiguous", false}, {"email", email}};
}
json::object sign_in() { return {{"login", true}}; }
json::object request() { return {{"cookies", "SID=private-cookie; __Host-ONE=skip; OSID=host-cookie"}}; }
struct Temp {
    fs::path path = fs::temp_directory_path() / ("ais-import-" + random_hex(8));
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
}
TEST_CASE("cookie push enumerates verified identities and safely scopes raw cookies") {
    ImportBackend backend;
    backend.replies = {verified("one@example.com"), verified("two@example.com"), verified("ONE@example.com")};
    auto result = probe_cookie_import(request(), 8, backend);
    REQUIRE(result.accounts.size() == 2);
    CHECK(result.details.at("attempted") == 3);
    CHECK(result.details.at("skipped_host_cookies") == 1);
    CHECK_FALSE(backend.closed);
    auto cookies = result.accounts[0].storage_state.at("cookies").as_array();
    REQUIRE(cookies.size() == 2);
    CHECK(cookies[1].as_object().at("domain") == "aistudio.google.com");
    CHECK(result.accounts[1].auth_user == "1");
}
TEST_CASE("cookie push continues after an unauthenticated auth_user index") {
    ImportBackend backend;
    backend.replies = {verified("zero@example.com"), json::object{}, verified("two@example.com")};
    backend.unauthorized = {2};
    auto result = probe_cookie_import(request(), 3, backend);
    REQUIRE(result.accounts.size() == 2);
    CHECK(result.accounts[0].auth_user == "0");
    CHECK(result.accounts[1].auth_user == "2");
    CHECK(result.details.at("attempted") == 3);
}
TEST_CASE("cookie push stops after three consecutive sign-in responses") {
    ImportBackend backend;
    backend.replies = {verified("zero@example.com"), sign_in(), verified("two@example.com"),
                       sign_in(), sign_in(), sign_in()};
    auto result = probe_cookie_import(request(), 8, backend);
    REQUIRE(result.accounts.size() == 2);
    CHECK(result.accounts[0].auth_user == "0");
    CHECK(result.accounts[1].auth_user == "2");
    CHECK(result.details.at("attempted") == 6);
}
TEST_CASE("cookie push stops after three consecutive unauthorized probes") {
    ImportBackend backend;
    backend.replies = {verified("zero@example.com"), json::object{}, json::object{}, json::object{}};
    backend.unauthorized = {2, 3, 4};
    auto result = probe_cookie_import(request(), 8, backend);
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].auth_user == "0");
    CHECK(result.details.at("attempted") == 4);
}
TEST_CASE("cookie push keeps verified accounts instead of starting a probe past the deadline") {
    ImportBackend backend;
    backend.replies = {verified("zero@example.com"), verified("one@example.com")};
    // The first probe takes longer than the time left after it, so the scan
    // cannot fit a second one. Slow machines only make the margin larger.
    backend.delay = 600ms;
    auto result = probe_cookie_import(request(), 8, backend, {}, Clock::now() + 1s);
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].auth_user == "0");
    CHECK(result.details.at("attempted") == 1);
    CHECK(result.details.at("deadline_reached") == true);
    CHECK(result.details.at("truncated") == true);
}
TEST_CASE("cookie push deadline never skips the search for a first account") {
    ImportBackend backend;
    backend.replies = {sign_in(), verified("one@example.com"), verified("two@example.com")};
    auto result = probe_cookie_import(request(), 8, backend, {}, Clock::now() - 1s);
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].auth_user == "1");
    CHECK(result.details.at("attempted") == 2);
    CHECK(result.details.at("deadline_reached") == true);
}
TEST_CASE("cookie push with a distant deadline scans as without one") {
    ImportBackend backend;
    backend.replies = {verified("zero@example.com"), sign_in(), verified("two@example.com")};
    auto result = probe_cookie_import(request(), 3, backend, {}, Clock::now() + 1h);
    REQUIRE(result.accounts.size() == 2);
    CHECK(result.details.at("attempted") == 3);
    CHECK(result.details.at("deadline_reached") == false);
    CHECK(result.details.at("discarded_origins") == 0);
}
TEST_CASE("cookie push accepts storage files with local state and imports cookies only") {
    ImportBackend backend;
    backend.replies = {verified("one@example.com")};
    json::object cookie{{"name", "SID"}, {"value", "private-cookie"}, {"domain", ".google.com"},
                        {"path", "/"}, {"expires", -1}, {"httpOnly", true}, {"secure", true},
                        {"sameSite", "Lax"}};
    json::object origin{{"origin", "https://aistudio.google.com"},
                        {"localStorage", json::array{json::object{{"name", "synthetic-key"},
                                                                  {"value", "synthetic-value"}}}}};
    json::object auth_file{{"cookies", json::array{cookie}},
                           {"origins", json::array{origin}},
                           {"accountName", "one@example.com"}};
    auto result = probe_cookie_import({{"storage_state", auth_file}, {"auth_user", "0"}}, 8, backend);
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].storage_state.at("cookies").as_array().size() == 1);
    CHECK_FALSE(result.accounts[0].storage_state.if_contains("origins"));
    CHECK_FALSE(result.accounts[0].storage_state.if_contains("accountName"));
    CHECK(backend.candidates[0].storage_state == result.accounts[0].storage_state);
    CHECK(result.details.at("discarded_origins") == 1);
}
TEST_CASE("structured cookie push preserves exact metadata and explicit identity") {
    ImportBackend backend;
    backend.replies = {verified("one@example.com")};
    json::object cookie{{"name", "SID"}, {"value", "private-cookie"}, {"domain", "accounts.google.com"},
                        {"path", "/specific"}, {"secure", false}, {"httpOnly", false}, {"sameSite", "Strict"}, {"expires", 1234}};
    json::object input{{"cookies", json::array{cookie}}, {"auth_user", "7"}, {"email", "ONE@example.com"}};
    auto result = probe_cookie_import(input, 8, backend);
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].auth_user == "7");
    CHECK(result.accounts[0].storage_state.at("cookies").as_array()[0] == cookie);
    cookie["partitionKey"] = "https://google.com";
    CHECK_THROWS(probe_cookie_import({{"cookies", json::array{cookie}}}, 8, backend));
    cookie.erase("partitionKey"); cookie["firstPartyDomain"] = "google.com";
    CHECK_THROWS(probe_cookie_import({{"cookies", json::array{cookie}}}, 8, backend));
    cookie.erase("firstPartyDomain"); cookie["domain"] = "evilgoogle.com";
    CHECK_THROWS(probe_cookie_import({{"cookies", json::array{cookie}}}, 8, backend));
}
TEST_CASE("legacy userscript header keeps Google RPC authentication cookies script readable") {
    ImportBackend backend; backend.replies = {verified("one@example.com")};
    auto result = probe_cookie_import({{"cookies", "SID=s; APISID=a; SAPISID=p; __Secure-1PAPISID=one; __Secure-3PAPISID=three; HSID=h; SSID=t"}, {"auth_user", "0"}}, 8, backend);
    const auto &cookies = result.accounts[0].storage_state.at("cookies").as_array();
    REQUIRE(cookies.size() == 7);
    for (std::size_t i = 0; i < cookies.size(); ++i) {
        CHECK(bool_or(cookies[i].as_object(), "httpOnly") == (i >= 5));
        CHECK(bool_or(cookies[i].as_object(), "secure"));
        CHECK(cookies[i].as_object().at("domain") == ".google.com");
    }
    backend.candidates.clear();
    auto exact = json::object{{"name", "SAPISID"}, {"value", "synthetic"}, {"domain", ".google.com"}, {"httpOnly", true}};
    result = probe_cookie_import({{"cookies", json::array{exact}}, {"auth_user", "0"}}, 8, backend);
    CHECK(bool_or(result.accounts[0].storage_state.at("cookies").as_array()[0].as_object(), "httpOnly"));
}
TEST_CASE("metadata-bearing cookie text preserves Netscape and JSON provenance") {
    const std::string netscape = "\xEF\xBB\xBF# Netscape HTTP Cookie File\n"
        "aistudio.google.com\tFALSE\t/\tTRUE\t0\t__Host-Synthetic\thost\n"
        "#HttpOnly_accounts.google.com\tFALSE\t/specific\tTRUE\t1234\tSAPISID\tsecret\n"
        "accounts.google.com\tFALSE\t/\tTRUE\t0\tOSID\tosid\n";
    auto state = Store::normalize_storage_state(json::string(netscape));
    for (const auto &input : {json::value(netscape), json::value(json::serialize(state)),
                              json::value(json::serialize(state.at("cookies"))), json::value(state)}) {
        ImportBackend backend; backend.replies = {verified("one@example.com")};
        auto result = probe_cookie_import({{"cookies", input}, {"auth_user", "0"}}, 8, backend);
        CHECK(result.accounts[0].storage_state == state);
        CHECK(backend.candidates[0].storage_state == state);
        CHECK(result.details.at("skipped_host_cookies") == 0);
    }
    // Semicolons in structured values are data, not a count of header credentials.
    state.at("cookies").as_array()[0].as_object()["value"] = std::string(600, ';');
    ImportBackend backend; backend.replies = {verified("one@example.com")};
    CHECK_NOTHROW(probe_cookie_import({{"cookies", json::serialize(state)}, {"auth_user", "0"}}, 8, backend));
}
TEST_CASE("wrapped raw cookie headers retain compensation and discard outer local storage") {
    json::object wrapped{{"cookies", "SID=x; OSID=y; __Host-X=z"}};
    for (const auto &input : {json::value(wrapped), json::value(json::serialize(wrapped))}) {
        ImportBackend backend; backend.replies = {verified("one@example.com")};
        auto result = probe_cookie_import({{"storage_state", input}, {"auth_user", "0"}}, 8, backend);
        const auto &cookies = result.accounts[0].storage_state.at("cookies").as_array();
        REQUIRE(cookies.size() == 2);
        CHECK_FALSE(bool_or(cookies[0].as_object(), "httpOnly"));
        CHECK(cookies[1].as_object().at("domain") == "aistudio.google.com");
        CHECK(result.details.at("skipped_host_cookies") == 1);
    }
    wrapped["origins"] = json::array{json::object{{"origin", "https://google.com"}}};
    for (const auto &input : {json::value(wrapped), json::value(json::serialize(wrapped))}) {
        ImportBackend backend; backend.replies = {verified("one@example.com")};
        auto result = probe_cookie_import({{"storage_state", input}, {"auth_user", "0"}}, 8, backend);
        REQUIRE(result.accounts.size() == 1);
        CHECK_FALSE(result.accounts[0].storage_state.if_contains("origins"));
    }
}
TEST_CASE("JSON cookie text retains structured import security validation") {
    const json::object base{{"name", "OSID"}, {"value", "synthetic"}, {"domain", "accounts.google.com"}};
    for (const auto &field : {"partitionKey", "partitionKeyOpaque", "partitioned", "firstPartyDomain", "domain"}) {
        auto cookie = base;
        if (std::string_view(field) == "partitionKeyOpaque" || std::string_view(field) == "partitioned")
            cookie[field] = true;
        else cookie[field] = std::string_view(field) == "domain" ? "evilgoogle.com" : "https://google.com";
        json::object state{{"cookies", json::array{cookie}}};
        for (const auto &input : {json::value(state), json::value(json::serialize(state))}) {
            ImportBackend backend;
            CHECK_THROWS(probe_cookie_import({{"cookies", input}}, 8, backend));
            CHECK(backend.candidates.empty());
        }
    }
    for (const auto &cookies : {json::value(json::array{base}), json::value("SID=synthetic")}) {
        json::object state{{"cookies", cookies}, {"origins", json::array{json::object{{"origin", "https://google.com"}}}}};
        for (const auto &input : {json::value(state), json::value(json::serialize(state))}) {
            ImportBackend backend; backend.replies = {verified("one@example.com")};
            auto result = probe_cookie_import({{"storage_state", input}, {"auth_user", "0"}}, 8, backend);
            REQUIRE(result.accounts.size() == 1);
            CHECK_FALSE(result.accounts[0].storage_state.if_contains("origins"));
        }
    }
    ImportBackend nested_backend; nested_backend.replies = {verified("one@example.com")};
    json::object nested{{"cookies", json::array{base}}, {"origins", json::array{json::object{{"origin", "https://google.com"}}}}};
    auto result = probe_cookie_import({{"storage_state", json::object{{"cookies", json::serialize(nested)}}}, {"auth_user", "0"}}, 8, nested_backend);
    REQUIRE(result.accounts.size() == 1);
    CHECK_FALSE(result.accounts[0].storage_state.if_contains("origins"));
    ImportBackend invalid_backend;
    CHECK_THROWS(probe_cookie_import({{"cookies", "evilgoogle.com\tFALSE\t/\tTRUE\t0\tOSID\tsynthetic\n"}}, 8, invalid_backend));
    CHECK(invalid_backend.candidates.empty());
}
TEST_CASE("cookie import distinguishes safe browser failures without leaking exception text") {
    for (const auto &[code, status] : std::vector<std::pair<std::string, int>>{
        {"browser_not_configured", 503}, {"browser_launch", 502}, {"browser_timeout", 504},
        {"browser_disconnected", 502}, {"browser_incompatible", 502}}) {
        ImportBackend backend; backend.fail_at = 1; backend.failure_code = code;
        try { probe_cookie_import(request(), 8, backend); FAIL("expected failure"); }
        catch (const Error &e) {
            CHECK(e.code == "cookie_" + code); CHECK(e.status == status);
            CHECK(std::string(e.what()).find("private-cookie") == std::string::npos);
        }
    }
}
TEST_CASE("cookie push rejects ambiguous identities infrastructure errors and cancellation atomically") {
    Temp temp; Store store(temp.path);
    ImportBackend backend;
    backend.replies = {verified("one@example.com"), verified("")};
    CHECK_THROWS(probe_cookie_import(request(), 8, backend));
    CHECK(store.accounts().empty());
    backend.candidates.clear(); backend.fail_at = 2;
    try { probe_cookie_import(request(), 8, backend); FAIL("expected failure"); }
    catch (const Error &e) { CHECK(e.code == "cookie_probe_failed"); CHECK(std::string(e.what()).find("private-cookie") == std::string::npos); }
    CHECK(store.accounts().empty());
    std::stop_source stop; stop.request_stop();
    try { probe_cookie_import(request(), 8, backend, stop.get_token()); FAIL("expected cancellation"); }
    catch (const Error &e) { CHECK(e.code == "cancelled"); }
    CHECK_THROWS(probe_cookie_import({{"cookies", "SID=x"}, {"unknown", true}}, 8, backend));
    CHECK_THROWS(probe_cookie_import(request(), 0, backend));
}
TEST_CASE("verified account merge reuses email identity and preserves cooldown selection and stats") {
    Temp temp; Store store(temp.path);
    ImportBackend backend; backend.replies = {verified("one@example.com")};
    auto result = probe_cookie_import({{"cookies", "SID=private-cookie"}, {"auth_user", "0"}}, 8, backend);
    auto first = result.accounts[0]; first.name = "Personal name";
    store.merge_verified_accounts({first}); store.select_account(first.id);
    store.success(first.id, "model");
    RequestUsage usage(store, "import-test-request", "gemini");
    usage.configure("model", false); usage.start(); usage.normalized("model", {}); usage.begin_attempt(first.id);
    usage.event({{"usageMetadata", json::object{{"promptTokenCount", 2}, {"candidatesTokenCount", 0}, {"totalTokenCount", 2}}},
                 {"candidates", json::array{json::object{{"finishReason", "STOP"}}}}}, false);
    usage.end_attempt(); usage.producer_done(); usage.http_done(200, "", true);
    store.failure(first.id, "model", "429", 120);
    auto refreshed = first; refreshed.id = "different-id"; refreshed.email = "ONE@example.com";
    refreshed.name = "Automatic name"; refreshed.auth_user = "3";
    auto merged = store.merge_verified_accounts({refreshed});
    REQUIRE(store.accounts().size() == 1);
    CHECK(string_or(merged[0].as_object(), "id") == first.id);
    CHECK(store.account(first.id).name == "Personal name");
    CHECK(store.account(first.id).auth_user == "3");
    CHECK_FALSE(store.choose("model").has_value());
    CHECK(store.stats().at("records").as_array().size() == 1);
    auto bad = refreshed; bad.email.clear();
    CHECK_THROWS(store.merge_verified_accounts({refreshed, bad}));
    CHECK(store.accounts().size() == 1);
    auto unrelated = first; unrelated.email = "two@example.com";
    store.merge_verified_accounts({unrelated});
    CHECK(store.accounts().size() == 2);
}

TEST_CASE("cookie import bounds normalized and refreshed credentials and sanitizes partition counts") {
    ImportBackend backend;
    auto reply = verified("one@example.com"); reply["skippedPartitionCookies"] = 3;
    reply["untrusted"] = "private-cookie";
    backend.replies = {reply};
    auto input = request(); input["auth_user"] = "0";
    auto result = probe_cookie_import(input, 8, backend);
    CHECK(result.details.at("skipped_partition_cookies") == 3);
    CHECK(json::serialize(result.details).find("private-cookie") == std::string::npos);
    backend.candidates.clear(); backend.expand_state = true;
    try { probe_cookie_import(input, 8, backend); FAIL("expected credential size limit"); }
    catch (const Error &e) { CHECK(e.status == 413); CHECK(e.code == "cookie_import_limit"); }
    std::string header;
    for (unsigned i = 0; i < 513; ++i) header += "SID" + std::to_string(i) + "=x;";
    try { probe_cookie_import({{"cookies", header}}, 8, backend); FAIL("expected cookie count limit"); }
    catch (const Error &e) { CHECK(e.status == 413); CHECK(e.code == "cookie_import_limit"); }
    backend.expand_state = false;
    for (auto value : {json::value(-1), json::value(2.5), json::value("private-cookie")}) {
        backend.candidates.clear(); backend.replies[0]["skippedPartitionCookies"] = value;
        CHECK(probe_cookie_import(input, 8, backend).details.at("skipped_partition_cookies") == 0);
    }
}
TEST_CASE("cookie parser bounds every import format before probing") {
    std::string header, netscape;
    json::array cookies;
    json::object mapped;
    for (unsigned i = 0; i < 513; ++i) {
        auto name = "SID" + std::to_string(i);
        header += name + "=x;";
        netscape += ".google.com\tTRUE\t/\tTRUE\t0\t" + name + "\tx\n";
        cookies.push_back(json::object{{"name", name}, {"value", "x"}, {"domain", ".google.com"}});
        mapped[name] = "x";
    }
    for (const auto &input : {json::value(header), json::value(netscape), json::value(cookies),
                             json::value(json::serialize(cookies)), json::value(json::object{{"cookies", mapped}}),
                             json::value(json::object{{"cookies", header}})}) {
        ImportBackend backend;
        try { probe_cookie_import({{"cookies", input}}, 8, backend); FAIL("expected cookie count limit"); }
        catch (const Error &e) { CHECK(e.status == 413); CHECK(e.code == "cookie_import_limit"); }
        CHECK(backend.candidates.empty());
        // General storage normalization remains unlimited unless the caller opts in.
        CHECK(Store::normalize_storage_state(input).at("cookies").as_array().size() == 513);
    }
}
