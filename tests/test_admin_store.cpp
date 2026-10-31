// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/store.hpp"
#include <catch2/catch_test_macros.hpp>
using namespace ais2api;
namespace {
struct AdminDirectory {
    fs::path path = fs::temp_directory_path() / ("ais2api-admin-store-" + random_hex(8));
    AdminDirectory() { fs::create_directories(path); }
    ~AdminDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};
std::string add(Store& store, std::string name) {
    auto rows = store.import_accounts(json::object{{"cookies", json::array{json::object{
        {"name", "SID"}, {"value", "fixture"}, {"domain", ".google.com"},
        {"path", "/"}, {"secure", true}}}}, {"origins", json::array{}}}, name);
    return string_or(rows.front().as_object(), "id");
}
}
TEST_CASE("admin rotation advances in insertion order and preserves selection on failure") {
    AdminDirectory d;
    std::string a, b, c;
    {
        Store store(d.path);
        CHECK(store.rotation_status().at("preferred_account_id").is_null());
        CHECK_THROWS_AS(store.rotate_account(), Error);
        a = add(store, "a"); b = add(store, "b"); c = add(store, "c");
        CHECK(store.rotate_account() == a);
        store.success(c, "m"); // Explicit rotation must clear model stickiness.
        CHECK(store.rotate_account() == b);
        CHECK(store.choose("m")->id == b);
        store.failure(c, "m", "login_expired");
        CHECK(store.rotate_account() == a);
        store.failure(b, "m", "rate_limit");
        CHECK(store.rotate_account() == a);
        auto disabled = store.account(a); disabled.disabled = true; store.put_account(disabled);
        try { (void)store.rotate_account(); FAIL("rotation should fail"); }
        catch (const Error& e) { CHECK(e.status == 503); }
        CHECK(string_or(store.rotation_status(), "preferred_account_id") == a);
        CHECK(store.rotation_status().at("next_account_id").is_null());
    }
    Store reopened(d.path);
    CHECK(string_or(reopened.rotation_status(), "preferred_account_id") == a);
    reopened.clear_cooldown(b, "*");
    CHECK(reopened.rotate_account() == b);
}
TEST_CASE("admin cooldown clearing is exact and never repairs expired or disabled accounts") {
    AdminDirectory d;
    Store store(d.path);
    auto a = add(store, "a");
    store.failure(a, "m", "permission");
    store.failure(a, "m2", "permission");
    store.failure(a, "m", "rate_limit");
    store.clear_cooldown(a, "m");
    CHECK_FALSE(store.choose("m")); // Global cooldown remains.
    store.clear_cooldown(a, "*");
    CHECK(store.choose("m"));
    CHECK_FALSE(store.choose("m2"));
    try { store.clear_cooldown("missing"); FAIL("unknown account should fail"); }
    catch (const Error& e) { CHECK(e.status == 404); }
    CHECK_FALSE(store.choose("m2"));
    store.failure(a, "m", "login_expired");
    store.clear_cooldown(a);
    CHECK_FALSE(store.choose("m"));
    CHECK(store.account_status().front().as_object().at("expired").as_bool());
    auto disabled = store.account(a); disabled.disabled = true; store.put_account(disabled);
    store.clear_cooldown(a);
    CHECK(store.account(a).disabled);
    store.reset_stats();
    CHECK(store.stats().at("records").as_array().empty());
    CHECK_FALSE(store.choose("m"));
}
TEST_CASE("managed API keys expose secrets once and isolate legacy keys") {
    AdminDirectory d;
    std::string secret, id;
    {
        Store store(d.path);
        auto legacy = store.create_key("legacy");
        CHECK_FALSE(store.authenticate_managed(string_or(legacy, "secret")));
        auto created = store.create_managed_key("client");
        secret = string_or(created, "secret"); id = string_or(created, "id");
        REQUIRE(store.authenticate_managed(secret));
        CHECK(*store.authenticate_managed(secret) == id);
        CHECK_FALSE(store.authenticate(secret));
        CHECK_FALSE(store.authenticate_managed("invalid"));
        CHECK_FALSE(store.authenticate_managed(std::string(4097, 'x')));
        auto metadata = store.managed_keys();
        REQUIRE(metadata.size() == 1);
        CHECK_FALSE(metadata.front().as_object().contains("secret"));
        CHECK_FALSE(metadata.front().as_object().contains("hash"));
        store.rename_managed_key(id, "renamed");
        CHECK(string_or(store.managed_keys().front().as_object(), "name") == "renamed");
        CHECK_THROWS_AS(store.rename_managed_key("missing", "name"), Error);
        CHECK_THROWS_AS(store.revoke_managed_key("missing"), Error);
        CHECK_THROWS_AS(store.create_managed_key(std::string(201, 'x')), Error);
        CHECK_THROWS_AS(store.rename_managed_key(id, ""), Error);
    }
    Store reopened(d.path);
    CHECK(reopened.authenticate_managed(secret) == std::optional<std::string>(id));
    reopened.revoke_managed_key(id);
    CHECK_FALSE(reopened.authenticate_managed(secret));
    CHECK(reopened.managed_keys().empty());
}
TEST_CASE("account metadata updates preserve credential and authentication fields") {
    AdminDirectory d;
    Store store(d.path);
    auto id = add(store, "original");
    auto before = store.account(id);
    store.failure(id, "m", "login_expired");
    store.update_email(id, "fixture@example.invalid");
    store.update_account_metadata(id, {{"name", "renamed"}, {"disabled", true}});
    auto after = store.account(id);
    CHECK(after.storage_state == before.storage_state);
    CHECK(after.auth_user == before.auth_user);
    CHECK(after.name == "renamed");
    CHECK(after.email == "fixture@example.invalid");
    CHECK(after.disabled);
    CHECK(store.account_status().front().as_object().at("expired").as_bool());
    CHECK_THROWS_AS(store.update_email("missing", ""), Error);
    CHECK_THROWS_AS(store.update_account_metadata("missing", {{"name", "x"}}), Error);
    CHECK_THROWS_AS(store.update_account_metadata(id, {{"email", "unexpected"}}), Error);
    CHECK_THROWS_AS(store.update_account_metadata(id, {{"disabled", "false"}}), Error);
    CHECK(store.account(id).name == "renamed");
}
