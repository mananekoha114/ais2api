// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "ais2api/backend.hpp"
#include "ais2api/waa/runtime.hpp"

namespace ais2api {
class Store;
// What the SpiderMonkey backend reads from, and sends as, the AI Studio page.
// After AIStudio2API's internal/aistudio/runtime_go.go (MIT).
namespace spidermonkey_detail {
// The page's public API key (the "WIu0Nc" entry of its inline data); empty
// when the document has none.
std::string page_api_key(std::string_view html);
// The build globals the page's inline scripts declare: the toggles array and
// the _F_cssRowKey and _F_combinedSignature strings.
json::object page_globals(std::string_view html);
// Whether a Content-Security-Policy requires Trusted Types at script sinks.
bool requires_trusted_types(const std::vector<std::string> &policies);
// A visit id as the page makes one: "v1_" and the Base64 of a random UUID.
std::string new_visit_id();
// The top-level numbers, booleans and strings of a JSON+protobuf array as
// protobuf binary, field numbers by position: how the page turns the
// GetLoggingContext response into its X-Goog-Ext-519733851-Bin header (before
// Base64). 502 browser_incompatible for anything else.
std::string encode_jspb_fields(std::string_view text);
// document.cookie for https://aistudio.google.com/: the unexpired cookies of
// the host that are not HttpOnly.
std::string document_cookie(const json::array &stored, std::int64_t now = unix_seconds());
// The tier a GetAiStudioBenefitTier response names: "free", "pro", "ultra" or
// "plus"; "unknown" for a response that is not one.
std::string benefit_tier(std::string_view body);
// A ListModels response as the model entries discovery returns. 502
// browser_incompatible for an unknown shape.
json::array discovered_models(std::string_view body);
// Sec-Fetch-Site for a request the AI Studio page makes to `url`.
std::string_view fetch_site(std::string_view url);
// The text of the request's last user turn: what the page's prompt box holds
// when Run is clicked. Empty when that turn has no text.
std::string page_prompt(const Request &request);
// document.lastModified for a document received at `time` in `timezone`
// (IANA; UTC when unknown here): "MM/DD/YYYY hh:mm:ss".
std::string last_modified(std::chrono::system_clock::time_point time, std::string_view timezone);

// The signed-in account's address in the page's inline data (its "oPEP7c"
// entry); empty when the document names none.
std::string page_email(std::string_view html);
// The URL a redirect's Location names, resolved against the request's `base`.
std::string resolve_location(std::string_view base, std::string_view location);
// Why a page RPC's response is not a result: AI Studio refusing the region or
// the account (`[7,"Region not supported."]`, 403 region_blocked or
// account_unsupported), a Google RPC error, or an HTTP failure. Nothing for a
// 200 response that is a result.
std::optional<Error> page_rpc_failure(std::string_view method, long status, std::string_view body);
// The CountTokens body the page sends for its prompt box after Run.
std::string count_tokens_body(std::string_view model, std::string_view prompt);
// X-AIStudio-G1-Tier for a benefit tier: TIER0 for plus, TIER1 for pro, TIER2
// for ultra; empty for the others.
std::string_view tier_header(std::string_view tier);
// What browser_detail::run_generation reads on each poll, shaped like the
// browser bridge's parsed JSON. Every number is int64: run_generation checks
// is_int64(), and Boost.JSON stores a value built from an unsigned type as uint64.
struct RelayState {
    bool done = false;
    long status = 0;
    unsigned retry_after = 0;
    std::string_view error, error_body, benefit;
};
json::object relay_state(const RelayState &state);
// The "pending" entry of a relay state: one chunk of the response body.
json::object relay_chunk(std::uint64_t sequence, std::string_view bytes);

// Request headers as the page's Firefox sends them, in Firefox's order. A
// Cookie header with an empty value stands for the session's cookies.
using Headers = std::vector<std::pair<std::string, std::string>>;
struct PageHeaders {
    std::string user_agent, accept_language, accept_encoding;
};
// The AI Studio page itself, typed into the address bar.
Headers document_headers(const PageHeaders &page);
// An iframe the page inserts.
Headers frame_headers(const PageHeaders &page, std::string_view url);
// An <img> the page loads.
Headers image_headers(const PageHeaders &page, std::string_view url);
// A <script> the page loads.
Headers script_headers(const PageHeaders &page, std::string_view url);
// What the page's grpc-web client adds to a MakerSuite or WAA RPC.
struct RpcContext {
    std::string api_key, auth_user = "0", authorization;
    // Empty ones are left out.
    std::string visit_id, extension, tier;
    std::string priority = "u=4";
};
Headers rpc_headers(const PageHeaders &page, const RpcContext &rpc);

struct HostIdentity {
    waa::Profile profile; // without the page's URL, globals and document
    std::string user_agent, accept_language;
};
// The host values of a Camoufox configuration (an identity's properties with
// the account's locale and timezone applied), as a Camoufox page shows them.
HostIdentity host_identity(const json::object &config);
} // namespace spidermonkey_detail

#ifdef AIS2API_HAS_SPIDERMONKEY
// The experimental SpiderMonkey backend (browser_backend "spidermonkey"):
// Google's WAA virtual machine runs in this process and requests go out over
// HTTP, with no browser. An account uses the Firefox identity it was given
// under the Camoufox backend. The prototype claims Firefox 152; a build with
// curl-impersonate sends with Firefox's TLS and HTTP/2 fingerprints.
class SpiderMonkeyGateway final : public GenerationBackend {
  public:
    SpiderMonkeyGateway(const Config &config, Store &store);
    ~SpiderMonkeyGateway() override;
    void generate(const Request &request, const Account &account, const EventSink &sink, std::stop_token cancel) override;
    json::array models(const Account &account, std::stop_token cancel = {}) override;
    json::object probe(const Account &account, std::stop_token cancel = {}) override;
    json::object diagnostics() override;
    std::string_view backend_for(const Account &) override { return "spidermonkey"; }
    void reconfigure(const Config &config) override;
    void close() override;
    void reset_session() override;
    // There is no captured template to discard.
    void clear_template() override {}
    bool release_idle() override;
    // For tools/waa_experiment only: generate() in its two halves. A proof
    // from the account's WAA program for the request's content, and the
    // request sent over HTTP from the loaded page with a proof from anywhere.
    std::string experiment_proof(const Request &request, const Account &account, std::stop_token cancel = {});
    void experiment_send(const Request &request, const Account &account, std::string_view proof, const EventSink &sink,
                         std::stop_token cancel = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
} // namespace ais2api
