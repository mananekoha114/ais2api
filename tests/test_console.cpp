// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/console.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace ais2api;
namespace {
// The text after the local "MM-DD HH:MM:SS " clock.
std::string body(std::string_view record) {
    auto line = console::log_line(json::parse(record).as_object());
    REQUIRE(line.size() > 15);
    REQUIRE(line[2] == '-');
    REQUIRE(line[14] == ' ');
    REQUIRE(line.back() == '\n');
    return line.substr(15, line.size() - 16);
}
}

TEST_CASE("Console format follows the setting and the attached stream", "[console]") {
    CHECK(console::choose("", true) == console::Format::text);
    CHECK(console::choose("auto", false) == console::Format::json);
    CHECK(console::choose("text", false) == console::Format::text);
    CHECK(console::choose("json", true) == console::Format::json);
    CHECK(console::choose("unknown", false) == console::Format::json);
}

TEST_CASE("Console log lines describe service events in Chinese", "[console]") {
    CHECK(body(R"({"event":"listening","bind":"127.0.0.1","port":18451,"timestamp":1792849152})") ==
          "信息 服务已启动      http://127.0.0.1:18451");
    CHECK(body(R"({"event":"stopped","timestamp":1792849152})") == "信息 服务已停止");
    CHECK(body(R"({"event":"generation_attempt","request_id":"chatcmpl-afc7e1e44098a7ebb1ccc8e8","account_id":"acc_cfe0d942f165147f0e903e7e75a4f2bc","attempt":1})") ==
          "信息 生成开始        chatcmpl-afc7e1e4…  账号 acc_cfe0d942…");
    CHECK(body(R"({"event":"generation_attempt","request_id":"chatcmpl-afc7e1e44098","account_id":"acc_cfe0d942f165","attempt":2})") ==
          "警告 重试生成        chatcmpl-afc7e1e4…  账号 acc_cfe0d942…  第 2 次");
    CHECK(body(R"({"event":"generation","request_id":"chatcmpl-afc7e1e44098","model":"gemini-3.1-flash-lite","stream":true,"queue_ms":0,"normalize_ms":1,"generation_ms":2386,"event_bytes":24121,"attempts":1,"error":""})") ==
          "信息 生成完成        chatcmpl-afc7e1e4…  gemini-3.1-flash-lite  流式  耗时 2.4s  输出 23.6 KB");
    CHECK(body(R"({"event":"generation","request_id":"resp_64ab7e0fb10e","model":"m","queue_ms":4911,"normalize_ms":0,"generation_ms":65000,"event_bytes":512,"attempts":2,"error":""})") ==
          "信息 生成完成        resp_64ab7e0f…  m  耗时 1m05s  排队 4.9s  输出 512 B  2 次尝试");
    CHECK(body(R"({"event":"generation","request_id":"chatcmpl-1","model":"m","attempts":3,"error":"session_expired"})") ==
          "错误 生成失败        chatcmpl-1  m  session_expired  3 次尝试");
    CHECK(body(R"({"event":"generation","request_id":"chatcmpl-1","model":"m","generation_ms":12,"error":"cancelled"})") ==
          "警告 生成已取消      chatcmpl-1  m  耗时 12ms");
    CHECK(body(R"({"event":"generation_continuation","request_id":"chatcmpl-1","account_id":"acc_1","attempt":2,"round":2})") ==
          "信息 继续生成        第 2 轮  chatcmpl-1");
    CHECK(body(R"({"event":"generation_continuation_retry","request_id":"chatcmpl-1","account_id":"acc_1","attempt":3,"round":2})") ==
          "警告 续接重试        第 2 轮  chatcmpl-1");
    CHECK(body(R"({"event":"output_budget_exceeded","request_id":"chatcmpl-1","account_id":"acc_1","budget":40000,"output":40001})") ==
          "警告 输出超过总预算  40001 / 40000 token  chatcmpl-1");
    CHECK(body(R"({"event":"generation_parameters_ignored","request_id":"chatcmpl-1","parameters":["tools.functionDeclarations","web_search_options.user_location"]})") ==
          "提示 已忽略参数      chatcmpl-1  tools.functionDeclarations, web_search_options.user_location");
    CHECK(body(R"({"event":"upstream_fields_ignored","request_id":"chatcmpl-1","locations":["part[25]","part[26]","candidate[7]"]})") ==
          "提示 已跳过上游字段  chatcmpl-1  part[25], part[26], candidate[7]");
    CHECK(body(R"({"event":"account_failure","request_id":"chatcmpl-1","account_id":"acc_1","kind":"rate_limit_minute","code":"rate_limit"})") ==
          "警告 账号暂不可用    chatcmpl-1  账号 acc_1  每分钟限额  rate_limit");
    CHECK(body(R"({"event":"session_recovery","request_id":"chatcmpl-1","account_id":"acc_1","code":"session_expired","action":"restart_browser"})") ==
          "警告 重启浏览器      chatcmpl-1  账号 acc_1  session_expired");
    CHECK(body(R"({"event":"http_request","category":"http","status":401,"error":"unauthorized"})") ==
          "警告 HTTP 401        数据接口  unauthorized");
    CHECK(body(R"({"event":"http_request","category":"management","status":500,"error":"internal"})") ==
          "错误 HTTP 500        管理接口  internal");
    CHECK(body(R"({"event":"config_saved","restart_required":true})") == "信息 配置已保存      重启后生效");
    CHECK(body(R"({"event":"setup_download","received_bytes":52428800,"maximum_bytes":209715200})") ==
          "信息 下载中          50.0 MB / 200.0 MB  25%");
    CHECK(body(R"({"event":"setup_stage","stage":"extract","browser_version":"146.0","platform":"windows-x64"})") ==
          "信息 解压浏览器      146.0  windows-x64");
    CHECK(body(R"({"event":"future_event","detail":"x","count":2,"timestamp":1,"id":"abc"})") ==
          "信息 future_event    detail=x  count=2");
}

TEST_CASE("Routine successful requests stay out of the console", "[console]") {
    for (auto status : {200, 204, 304})
        for (auto category : {"http", "management"})
            CHECK(console::log_line({{"event", "http_request"}, {"category", category}, {"status", status}, {"error", ""}}).empty());
    auto colored = console::log_line({{"event", "generation"}, {"request_id", "chatcmpl-1"}, {"error", "upstream_error"}}, true);
    CHECK(colored.find("\x1b[31m错误\x1b[0m") != std::string::npos);
    CHECK(colored.find("\x1b[90m") == 0);
}

TEST_CASE("Pretty command results keep every field", "[console]") {
    auto value = json::parse(R"({"running":true,"accounts":[{"id":"a","tags":[]},{"id":"b"}],"empty":{},"name":"中文"})");
    auto text = console::pretty(value);
    CHECK(text == "{\n  \"running\": true,\n  \"accounts\": [\n    {\n      \"id\": \"a\",\n      \"tags\": []\n    },\n    {\n      \"id\": \"b\"\n    }\n  ],\n  \"empty\": {},\n  \"name\": \"中文\"\n}");
    CHECK(json::parse(text) == value);
}
