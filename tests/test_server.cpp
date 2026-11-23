// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/backend.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/store.hpp"
#include "ais2api/webui.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
using namespace ais2api;
namespace net=boost::asio;
namespace http=boost::beast::http;
using tcp=net::ip::tcp;
using namespace std::chrono_literals;
namespace {
struct LiveServer {
    fs::path directory=fs::temp_directory_path()/("ais2api-http-"+random_hex(8));
    Config config;
    std::string key;
    std::jthread thread;
    std::exception_ptr failure;
    explicit LiveServer(bool configured_key=true,json::array models={"test-explicit-model"}){
        net::io_context io;tcp::acceptor port(io,{tcp::v4(),0});auto free_port=port.local_endpoint().port();port.close();
        config.data_dir=directory/"data";config.config_file=directory/"config.json";config.port=free_port;config.configured_models=std::move(models);config.limits.request_bytes=4096;config.limits.object_bytes=8192;config.limits.relay_bytes=1024;
        key="http-test-"+random_hex(24);config.api_key=configured_key?key:"";
        fs::create_directories(directory);atomic_write(config.config_file,json::serialize(config.to_json()),true);
        thread=std::jthread([this]{try{serve(config);}catch(...){failure=std::current_exception();}});
        // Sanitizer builds on shared runners can take seconds to start; a late start would otherwise block the join forever.
        bool ready=false;for(const auto deadline=Clock::now()+30s;Clock::now()<deadline;){try{local_call(config,{{"command","status"}});ready=true;break;}catch(const Error&){}std::this_thread::sleep_for(20ms);}
        if(!ready){try{local_call(config,{{"command","stop"}});}catch(...){}if(thread.joinable())thread.join();if(failure)std::rethrow_exception(failure);throw std::runtime_error("HTTP test service did not start");}
    }
    ~LiveServer(){try{local_call(config,{{"command","stop"}});}catch(...){}if(thread.joinable())thread.join();std::error_code ec;fs::remove_all(directory,ec);}
    http::response<http::string_body> call(http::verb method,std::string target,std::string body={},std::string auth={},std::initializer_list<std::pair<std::string,std::string>> headers={}){
        net::io_context io;tcp::socket socket(io);socket.connect({net::ip::make_address("127.0.0.1"),config.port});http::request<http::string_body> request(method,target,11);request.set(http::field::host,"127.0.0.1");request.set(http::field::content_type,"application/json");if(!auth.empty())request.set(http::field::authorization,"Bearer "+auth);for(const auto& [name,value]:headers)request.set(name,value);request.body()=std::move(body);request.prepare_payload();http::write(socket,request);boost::beast::flat_buffer input;http::response_parser<http::string_body> response;response.skip(method==http::verb::head);http::read(socket,input,response);return response.release();
    }
};
}
TEST_CASE("HTTP parses media types and Bearer schemes with exact case-insensitive boundaries", "[http]") {
    LiveServer server;
    server.config.admin_key = "admin-test-" + random_hex(24);
    atomic_write(server.config.config_file, json::serialize(server.config.to_json()), true);
    local_call(server.config, {{"command", "config.reload"}});
    for (auto scheme : {"bearer ", "bEaReR ", "Bearer   "}) {
        REQUIRE(server.call(http::verb::get, "/v1/models", {}, {},
                            {{"Authorization", std::string(scheme) + server.key}}).result_int() == 200);
        REQUIRE(server.call(http::verb::get, "/api/admin/v1/auth/check", {}, {},
                            {{"Authorization", std::string(scheme) + server.config.admin_key}}).result_int() == 200);
    }
    for (auto scheme : {"BearerX ", "Basic ", "Bearer"}) {
        REQUIRE(server.call(http::verb::get, "/v1/models", {}, {},
                            {{"Authorization", std::string(scheme) + server.key}}).result_int() == 401);
        REQUIRE(server.call(http::verb::get, "/api/admin/v1/auth/check", {}, {},
                            {{"Authorization", std::string(scheme) + server.config.admin_key}}).result_int() == 401);
    }
    for (auto type : {"Application/JSON", "application/json; charset=utf-8", "APPLICATION/JSON \t; charset=UTF-8"}) {
        auto generation = server.call(http::verb::post, "/v1/chat/completions", "{", server.key,
                                      {{"Content-Type", type}});
        REQUIRE(generation.result_int() == 400);
        REQUIRE(generation.body().find("invalid_json") != std::string::npos);
        auto management = server.call(http::verb::post, "/api/admin/v1/keys", "{", server.config.admin_key,
                                      {{"Content-Type", type}});
        REQUIRE(management.result_int() == 400);
        REQUIRE(management.body().find("invalid_json") != std::string::npos);
    }
    for (auto type : {"application/jsonp", "application/json-extra", "text/json"}) {
        REQUIRE(server.call(http::verb::post, "/v1/chat/completions", "{}", server.key,
                            {{"Content-Type", type}}).result_int() == 415);
        REQUIRE(server.call(http::verb::post, "/api/admin/v1/keys", "{}", server.config.admin_key,
                            {{"Content-Type", type}}).result_int() == 415);
    }
    REQUIRE_FALSE(server.failure);
}
TEST_CASE("HTTP exposes data routes and enforces authentication without Google","[http]"){
    LiveServer server;
    REQUIRE(server.call(http::verb::get,"/health").result_int()==200);
    REQUIRE(server.call(http::verb::get,"/ready").result_int()==503);
    REQUIRE(server.call(http::verb::get,"/v1/models").result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1beta/models",{},"invalid").result_int()==401);
    REQUIRE(server.call(http::verb::post,"/v1/responses",R"({"model":"m","input":"hello"})").result_int()==401);
    auto invalid_response=server.call(http::verb::post,"/v1/responses","{",server.key);
    REQUIRE(invalid_response.result_int()==400);
    REQUIRE(json::parse(invalid_response.body()).as_object().at("error").as_object().at("code")=="invalid_json");
    auto missing_response=server.call(http::verb::post,"/v1/responses",R"({"model":"m","input":"hello"})",server.key);
    REQUIRE(missing_response.result_int()==503);
    REQUIRE(missing_response.body().find("no_account")!=std::string::npos);
    auto models=server.call(http::verb::get,"/v1/models",{},server.key);REQUIRE(models.result_int()==200);auto data=json::parse(models.body()).as_object().at("data").as_array();REQUIRE(data.size()==1);REQUIRE(string_or(data[0].as_object(),"id")=="test-explicit-model");
    REQUIRE(server.call(http::verb::get,"/v1/models/test-explicit-model",{},server.key).result_int()==200);
    REQUIRE(server.call(http::verb::get,"/v1beta/models/test-explicit-model",{},server.key).result_int()==200);
    for(auto route:{"/api/accounts","/api-keys","/v1/responses"})REQUIRE(server.call(http::verb::get,route,{},server.key).result_int()==404);
    for(auto route:{"/accounts","/config","/stats","/"})REQUIRE(server.call(http::verb::get,route,{},server.key).result_int()==(webui::enabled()?200:404));
    auto missing=server.call(http::verb::post,"/v1beta/models/test-explicit-model:generateContent",R"({"contents":[{"role":"user","parts":[{"text":"hello"}]}]})",server.key);REQUIRE(missing.result_int()==503);REQUIRE(missing.body().find("no_account")!=std::string::npos);
    auto malformed=server.call(http::verb::post,"/v1/chat/completions","{",server.key);REQUIRE(malformed.result_int()==400);
    auto large=server.call(http::verb::post,"/v1/chat/completions",std::string(5000,'x'),server.key);REQUIRE(large.result_int()==413);
    REQUIRE_FALSE(server.failure);
}

TEST_CASE("Embedded console serves history routes without exposing API or filesystem paths", "[http][webui]") {
    LiveServer server;
    if (!webui::enabled()) {
        REQUIRE(server.call(http::verb::get, "/").result_int() == 404);
        REQUIRE_FALSE(webui::find("/index.html"));
        return;
    }
    const auto root = server.call(http::verb::get, "/");
    REQUIRE(root.result_int() == 200);
    REQUIRE(root[http::field::content_type] == "text/html; charset=utf-8");
    REQUIRE(root[http::field::cache_control] == "no-cache");
    REQUIRE(root["X-Content-Type-Options"] == "nosniff");
    REQUIRE(root["Content-Security-Policy"].find("frame-ancestors 'none'") != std::string_view::npos);
    REQUIRE(root.body().find("<div id=\"app\">") != std::string::npos);
    for (auto route : {"/login", "/change-password", "/accounts", "/models", "/keys", "/stats",
                       "/logs", "/config", "/diagnostics", "/settings"}) {
        const auto response = server.call(http::verb::get, std::string(route) + "?test=1");
        REQUIRE(response.result_int() == 200);
        REQUIRE(response.body() == root.body());
    }
    auto head = server.call(http::verb::head, "/");
    REQUIRE(head.result_int() == 200);
    REQUIRE(head.body().empty());
    REQUIRE(head[http::field::content_length] == std::to_string(root.body().size()));
    const auto start = root.body().find("/assets/");
    REQUIRE(start != std::string::npos);
    const auto end = root.body().find('"', start);
    REQUIRE(end != std::string::npos);
    const auto asset = root.body().substr(start, end - start);
    const auto script = server.call(http::verb::get, asset);
    REQUIRE(script.result_int() == 200);
    REQUIRE(script[http::field::content_type] == "text/javascript; charset=utf-8");
    REQUIRE(script[http::field::cache_control] == "public, max-age=31536000, immutable");
    REQUIRE_FALSE(script.body().empty());
    head = server.call(http::verb::head, asset);
    REQUIRE(head.body().empty());
    REQUIRE(head[http::field::content_length] == std::to_string(script.body().size()));
    REQUIRE(server.call(http::verb::get, "/THIRD_PARTY_LICENSES.txt").result_int() == 200);
    for (auto path : {"/config.json", "/src/main.ts", "/assets/missing.js", "/../config.json",
                      "/%2e%2e/config.json", "/assets/../index.html", "/unknown", "/api/missing", "/v1/missing"}) {
        REQUIRE(server.call(http::verb::get, path).result_int() == 404);
    }
    REQUIRE(server.call(http::verb::post, "/login").result_int() == 404);
    REQUIRE(server.call(http::verb::get, "/api/admin/v1/status").result_int() == 403);
    REQUIRE(server.call(http::verb::get, "/v1/models").result_int() == 401);
}

TEST_CASE("HTTP model contracts provide Gemini display names and preserve metadata","[http][models]"){
    LiveServer server(true,{
        "configured-string",
        json::object{{"id","configured-object"},{"displayName","Real Display Name"},{"description","Model description"},{"inputTokenLimit",32768},{"outputTokenLimit",8192},{"version","001"}},
        json::object{{"id","null-display"},{"displayName",nullptr}},
        json::object{{"id","empty-display"},{"displayName",""}}
    });
    auto response=server.call(http::verb::get,"/v1beta/models",{},server.key);
    REQUIRE(response.result_int()==200);
    auto body=json::parse(response.body()).as_object();
    REQUIRE_FALSE(body.contains("data"));
    auto models=body.at("models").as_array();
    REQUIRE(models.size()==4);
    for(const auto& value:models){
        const auto& model=value.as_object();
        auto name=string_or(model,"name");
        auto detail=server.call(http::verb::get,"/v1beta/"+name,{},server.key);
        REQUIRE(detail.result_int()==200);
        REQUIRE(json::parse(detail.body())==value);
        if(name=="models/configured-object"){
            REQUIRE(string_or(model,"displayName")=="Real Display Name");
            REQUIRE(string_or(model,"description")=="Model description");
            REQUIRE(model.at("inputTokenLimit")==32768);
            REQUIRE(model.at("outputTokenLimit")==8192);
            REQUIRE(string_or(model,"version")=="001");
        }else{
            REQUIRE(string_or(model,"displayName")==name.substr(7));
            REQUIRE_FALSE(model.contains("inputTokenLimit"));
            REQUIRE_FALSE(model.contains("outputTokenLimit"));
        }
    }
    response=server.call(http::verb::get,"/v1/models",{},server.key);
    REQUIRE(response.result_int()==200);
    body=json::parse(response.body()).as_object();
    REQUIRE(string_or(body,"object")=="list");
    REQUIRE_FALSE(body.contains("models"));
    REQUIRE(body.at("data").as_array().size()==4);
    for(const auto& value:body.at("data").as_array()){
        const auto& model=value.as_object();
        REQUIRE(string_or(model,"object")=="model");
        REQUIRE_FALSE(string_or(model,"id").empty());
        REQUIRE_FALSE(model.contains("displayName"));
        REQUIRE_FALSE(model.contains("name"));
    }
}

TEST_CASE("HTTP model lists follow eligible models with search variants","[http][models][search-model]"){
    LiveServer server(true,{
        json::object{{"id","gemini-text"},{"displayName","Gemini Text"}},
        "gemma-test",
        "gemini-real",
        "gemini-real-search",
        "other-model"
    });
    auto response=server.call(http::verb::get,"/v1beta/models",{},server.key);
    REQUIRE(response.result_int()==200);
    std::vector<std::string> names;
    const auto listing=json::parse(response.body());
    for(const auto& value:listing.as_object().at("models").as_array())
        names.push_back(string_or(value.as_object(),"name"));
    REQUIRE(names==std::vector<std::string>{"models/gemini-text","models/gemini-text-search","models/gemma-test",
                                            "models/gemma-test-search","models/gemini-real","models/gemini-real-search","models/other-model"});
    auto detail=json::parse(server.call(http::verb::get,"/v1beta/models/gemini-text-search",{},server.key).body()).as_object();
    REQUIRE(string_or(detail,"displayName")=="Gemini Text (Search)");
    REQUIRE(string_or(detail,"source")=="search_variant");
    response=server.call(http::verb::get,"/v1/models/gemma-test-search",{},server.key);
    REQUIRE(response.result_int()==200);
    auto openai=json::parse(response.body()).as_object();
    REQUIRE(openai.at("ais2api").as_object().at("source")=="search_variant");
    REQUIRE(json::parse(server.call(http::verb::get,"/v1/models",{},server.key).body()).as_object().at("data").as_array().size()==7);
}

TEST_CASE("HTTP model lists show aliases whose targets are listed","[http][models][thinking-suffix]"){
    LiveServer server(true,{json::object{{"id","gemini-text"},{"displayName","Gemini Text"},{"inputTokenLimit",1000}},"other-model"});
    server.config.model_aliases={{"fast","gemini-text(low)"},{"web","models/gemini-text-search"},{"other-model","gemini-text"},
                                 {"ghost","gemini-missing"},{"odd(high)","gemini-text"}};
    atomic_write(server.config.config_file,json::serialize(server.config.to_json()),true);
    local_call(server.config,{{"command","config.reload"}});
    std::vector<std::string> names;
    const auto listing=json::parse(server.call(http::verb::get,"/v1beta/models",{},server.key).body());
    for(const auto& value:listing.as_object().at("models").as_array())
        names.push_back(string_or(value.as_object(),"name"));
    // An alias named like a listed model replaces it; unlisted targets and suffixed keys stay hidden.
    REQUIRE(names==std::vector<std::string>{"models/gemini-text","models/gemini-text-search","models/other-model","models/fast","models/web"});
    auto detail=json::parse(server.call(http::verb::get,"/v1beta/models/fast",{},server.key).body()).as_object();
    REQUIRE(string_or(detail,"displayName")=="fast");
    REQUIRE(string_or(detail,"source")=="alias");
    REQUIRE(detail.at("inputTokenLimit")==1000);
    auto replaced=json::parse(server.call(http::verb::get,"/v1beta/models/other-model",{},server.key).body()).as_object();
    REQUIRE(replaced.at("inputTokenLimit")==1000);
    auto openai=json::parse(server.call(http::verb::get,"/v1/models/web",{},server.key).body()).as_object();
    REQUIRE(openai.at("ais2api").as_object().at("source")=="alias");
    REQUIRE(server.call(http::verb::get,"/v1/models/ghost",{},server.key).result_int()==404);
    REQUIRE(json::parse(server.call(http::verb::get,"/v1/models",{},server.key).body()).as_object().at("data").as_array().size()==5);
    auto anthropic=json::parse(server.call(http::verb::get,"/v1/models",{},server.key,{{"anthropic-version","2023-06-01"}}).body()).as_object();
    bool found=false;
    for(const auto& value:anthropic.at("data").as_array())
        if(value.as_object().at("id")=="fast"){found=true;REQUIRE(value.as_object().at("max_input_tokens")==1000);}
    REQUIRE(found);
}

TEST_CASE("Gemini-compatible clients accept version-prefixed model routes without rewriting resources", "[http][gemini-clients]") {
    LiveServer server;
    const std::array<std::string, 4> bases{
        "/v1beta/v1beta", "/v1beta//v1beta", "/v1beta/", "/v1beta/v1beta/v1beta"
    };
    for (const auto& base : bases) {
        auto listed = server.call(http::verb::get, base + "/models", {}, server.key);
        REQUIRE(listed.result_int() == 200);
        auto body = json::parse(listed.body()).as_object();
        REQUIRE_FALSE(body.contains("data"));
        const auto& models = body.at("models").as_array();
        REQUIRE(models.size() == 1);
        REQUIRE(string_or(models[0].as_object(), "name") == "models/test-explicit-model");
        REQUIRE(string_or(models[0].as_object(), "displayName") == "test-explicit-model");

        auto detail = server.call(http::verb::get, base + "/models/test-explicit-model", {}, server.key);
        REQUIRE(detail.result_int() == 200);
        auto model = json::parse(detail.body()).as_object();
        REQUIRE(string_or(model, "name") == "models/test-explicit-model");
        REQUIRE(string_or(model, "displayName") == "test-explicit-model");
    }
    REQUIRE(server.call(http::verb::get, "/v1beta/v1beta/models?key=" + server.key).result_int() == 200);
    REQUIRE(server.call(http::verb::get, "/v1beta//models",
                        {}, {}, {{"x-goog-api-key", server.key}}).result_int() == 200);
    REQUIRE(server.call(http::verb::get, "/v1beta/v1beta/models").result_int() == 401);
    REQUIRE(server.call(http::verb::get, "/v1beta//models", {}, "invalid").result_int() == 401);

    auto unknown_model = server.call(http::verb::get, "/v1beta/v1beta/models/unknown-model", {}, server.key);
    REQUIRE(unknown_model.result_int() == 404);
    REQUIRE(json::parse(unknown_model.body()).as_object().at("error").as_object().at("reason") == "model_not_found");
    auto unnormalized_resource = server.call(http::verb::get, "/v1beta/models//test-explicit-model", {}, server.key);
    REQUIRE(unnormalized_resource.result_int() == 404);
    REQUIRE(json::parse(unnormalized_resource.body()).as_object().at("error").as_object().at("reason") == "model_not_found");

    for (const auto* path : {"/v1beta/accounts", "/v1beta/v1beta/unknown", "/v1beta/v1betax/models",
                             "/v1beta/../v1/models", "/v1beta/admin/v1/models", "/v1beta/v1/chat/models"}) {
        auto response = server.call(http::verb::get, path, {}, server.key);
        REQUIRE(response.result_int() == 404);
        REQUIRE(json::parse(response.body()).as_object().at("error").as_object().at("reason") == "route_not_found");
    }
    for (auto action : {":generateContent", ":streamGenerateContent"}) {
        auto path = std::string("/v1beta/v1beta/models/test-explicit-model") + action;
        REQUIRE(server.call(http::verb::post, path, "{}").result_int() == 401);
        REQUIRE(server.call(http::verb::post, path + "?key=invalid", "{}").result_int() == 401);
    }
    REQUIRE(server.call(http::verb::post, "/v1beta/v1beta/models", "{}", server.key).result_int() == 404);
    REQUIRE_FALSE(server.failure);
}

TEST_CASE("HTTP authorization follows the configured key across reloads","[http][config]"){
    LiveServer server;
    std::string legacy_key;
    {Store store(server.config.data_dir);legacy_key=string_or(store.create_key("legacy database key"),"secret");}
    REQUIRE(server.call(http::verb::get,"/v1/models",{},legacy_key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==200);
    auto shown=json::serialize(local_call(server.config,{{"command","config.show"}}));
    REQUIRE(shown.find(server.key)==std::string::npos);

    auto invalid=server.config.to_json();
    invalid["api_key"]="invalid key with whitespace";
    atomic_write(server.config.config_file,json::serialize(invalid),true);
    try {
        local_call(server.config,{{"command","config.reload"}});
        FAIL("Invalid key configuration was applied");
    } catch(const Error& error) {
        REQUIRE(error.code=="invalid_config");
    }
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==200);

    const auto old_key=server.key;
    server.key="rotated-http-test-"+random_hex(24);
    server.config.api_key=server.key;
    atomic_write(server.config.config_file,json::serialize(server.config.to_json()),true);
    REQUIRE(string_or(local_call(server.config,{{"command","config.reload"}}),"status")=="reloaded");
    REQUIRE(server.call(http::verb::get,"/v1/models",{},old_key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==200);
    shown=json::serialize(local_call(server.config,{{"command","config.show"}}));
    REQUIRE(shown.find(server.key)==std::string::npos);

    server.config.api_key.clear();
    atomic_write(server.config.config_file,json::serialize(server.config.to_json()),true);
    local_call(server.config,{{"command","config.reload"}});
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},legacy_key).result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models").result_int()==401);
    const auto ready=server.call(http::verb::get,"/ready");
    REQUIRE(ready.result_int()==503);
    REQUIRE_FALSE(bool_or(json::parse(ready.body()).as_object(),"ready"));
}

TEST_CASE("An empty configured key never authorizes HTTP requests","[http][config]"){
    LiveServer server(false);
    REQUIRE(server.call(http::verb::get,"/health").result_int()==200);
    REQUIRE(server.call(http::verb::get,"/v1/models").result_int()==401);
    REQUIRE(server.call(http::verb::get,"/v1/models",{},server.key).result_int()==401);
    REQUIRE_FALSE(bool_or(local_call(server.config,{{"command","status"}}),"ready"));
}

TEST_CASE("HTTP listener reports an occupied port with actionable diagnostics","[http]"){
    net::io_context io;
    tcp::acceptor occupied(io);
    occupied.open(tcp::v4());
#ifdef _WIN32
    // Prevent the service's SO_REUSEADDR from sharing this Windows listener.
    const BOOL exclusive=TRUE;
    REQUIRE(::setsockopt(occupied.native_handle(),SOL_SOCKET,SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0);
#endif
    occupied.bind({net::ip::make_address("127.0.0.1"),0});
    occupied.listen();
    struct TemporaryDirectory {
        fs::path path=fs::temp_directory_path()/("ais2api-occupied-http-"+random_hex(8));
        ~TemporaryDirectory(){std::error_code ec;fs::remove_all(path,ec);}
    } directory;
    Config config;
    config.data_dir=directory.path/"data";
    config.config_file=directory.path/"config.json";
    config.bind="127.0.0.1";
    config.port=occupied.local_endpoint().port();
    fs::create_directories(directory.path);
    try {
        serve(config);
        FAIL("Service unexpectedly started on an occupied HTTP port");
    } catch(const Error& error) {
        REQUIRE(error.status==503);
#ifdef _WIN32
        // An exclusive listener can produce WSAEACCES rather than WSAEADDRINUSE.
        REQUIRE((error.code=="http_permission_denied"||error.code=="http_address_in_use"));
#else
        REQUIRE(error.code=="http_address_in_use");
#endif
        const std::string message=error.what();
        REQUIRE(message.find("HTTP listener bind failed")!=std::string::npos);
        REQUIRE(message.find("bind=127.0.0.1")!=std::string::npos);
        REQUIRE(message.find("port="+std::to_string(config.port))!=std::string::npos);
        REQUIRE(message.find("reason="+error.code.substr(5))!=std::string::npos);
        REQUIRE(message.find("os_error=")!=std::string::npos);
        REQUIRE(message.find("different available port")!=std::string::npos);
    }
}

namespace {
class SlowTailBackend final : public GenerationBackend {
  public:
    explicit SlowTailBackend(std::chrono::milliseconds pause) : pause_(pause) {}
    void generate(const Request &, const Account &, const EventSink &emit,
                  std::stop_token token) override {
        emit(json::object{{"candidates", json::array{json::object{
            {"index", 0},
            {"content", json::object{{"role", "model"},
                                     {"parts", json::array{json::object{{"text", "hello"}}}}}}}}}});
        std::mutex mu;
        std::condition_variable_any cv;
        std::unique_lock lock(mu);
        cv.wait_for(lock, token, pause_, [] { return false; });
        check_cancel(token);
        emit(json::object{{"candidates", json::array{json::object{
            {"index", 0},
            {"content", json::object{{"role", "model"},
                                     {"parts", json::array{json::object{{"text", " world"}}}}}},
            {"finishReason", "STOP"}}}}});
    }
    json::array models(const Account &, std::stop_token = {}) override { return {}; }
    json::object probe(const Account &, std::stop_token = {}) override { return {{"ok", true}}; }
    json::object diagnostics() override { return {}; }
    void close() override {}
  private:
    std::chrono::milliseconds pause_;
};
} // namespace

TEST_CASE("SSE streams survive gaps longer than the socket write timeout without false 504",
          "[http][sse][timeout]") {
    fs::path directory = fs::temp_directory_path() / ("ais2api-slow-sse-" + random_hex(8));
    fs::create_directories(directory);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { std::error_code ec; fs::remove_all(dir, ec); }
    } cleanup{directory};

    Config config;
    {
        net::io_context io;
        tcp::acceptor port(io, {tcp::v4(), 0});
        config.port = port.local_endpoint().port();
        port.close();
    }
    config.data_dir = directory / "data";
    config.config_file = directory / "config.json";
    config.configured_models = {"slow-model"};
    config.api_key = "slow-sse-" + random_hex(16);
    config.limits.generation_seconds = 60;
    config.limits.stream_idle_seconds = 60;
    atomic_write(config.config_file, json::serialize(config.to_json()), true);
    {
        Store store(config.data_dir);
        store.import_accounts(json::value("SAPISID=synthetic-slow-sse"), "Slow SSE");
    }

    std::exception_ptr failure;
    std::jthread thread([&] {
        try { serve_with_backend(config, std::make_unique<SlowTailBackend>(31s)); }
        catch (...) { failure = std::current_exception(); }
    });
    struct StopGuard {
        const Config& cfg;
        std::jthread& th;
        ~StopGuard() {
            try { local_call(cfg, {{"command", "stop"}}); } catch (...) {}
            if (th.joinable()) th.join();
        }
    } guard{config, thread};

    bool ready = false;
    for (const auto deadline = Clock::now() + 30s; Clock::now() < deadline;) {
        try { ready = bool_or(local_call(config, {{"command", "status"}}), "running"); }
        catch (const Error&) {}
        if (ready) break;
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(ready);

    net::io_context io;
    tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), config.port});
    http::request<http::string_body> request(http::verb::post, "/v1/chat/completions", 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::authorization, "Bearer " + config.api_key);
    request.body() = json::serialize(json::object{
        {"model", "slow-model"},
        {"stream", true},
        {"messages", json::array{json::object{{"role", "user"}, {"content", "hi"}}}}});
    request.prepare_payload();
    http::write(socket, request);

    boost::beast::flat_buffer input;
    http::response<http::string_body> response;
    http::read(socket, input, response);
    CHECK(response.result_int() == 200);
    CHECK(response.body().find("hello") != std::string::npos);
    CHECK(response.body().find(" world") != std::string::npos);
    CHECK(response.body().find("data: [DONE]") != std::string::npos);
    CHECK(response.body().find("network_timeout") == std::string::npos);
    REQUIRE_FALSE(failure);
}

TEST_CASE("Service shutdown waits for active count_tokens work before io_context destruction",
          "[http][shutdown]") {
    LiveServer server;
    std::string words;
    for (int i = 0; i < 400; ++i) words += "token ";
    const std::string body = json::serialize(json::object{
        {"model", "claude-sonnet-4-5"},
        {"messages", json::array{json::object{{"role", "user"}, {"content", words}}}}});

    std::atomic<bool> stop_flag{false};
    std::vector<std::jthread> clients;
    for (int i = 0; i < 4; ++i) {
        clients.emplace_back([&] {
            while (!stop_flag.load()) {
                try { (void)server.call(http::verb::post, "/v1/messages/count_tokens", body, server.key); }
                catch (...) { break; }
            }
        });
    }
    std::this_thread::sleep_for(50ms);
    local_call(server.config, {{"command", "stop"}});
    stop_flag.store(true);
    clients.clear();
    if (server.thread.joinable()) server.thread.join();
    REQUIRE_FALSE(server.failure);
}
