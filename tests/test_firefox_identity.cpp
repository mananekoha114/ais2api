// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/firefox_identity.hpp"
#include "fpgen_fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>

using namespace ais2api;
using namespace ais2api::firefox_identity;

namespace {
using namespace fpgen_fixture;
bool has(const json::array &items, std::string_view item) {
    return std::find(items.begin(), items.end(), json::value(item)) != items.end();
}
std::size_t position(const json::array &items, std::string_view item) {
    return static_cast<std::size_t>(std::find(items.begin(), items.end(), json::value(item)) - items.begin());
}
// The properties.json entries for what generation and launch write.
Properties properties() {
    const auto list = json::parse(R"([{"property":"navigator.userAgent","type":"str"},{"property":"navigator.appVersion","type":"str"},{"property":"navigator.oscpu","type":"str"},{"property":"navigator.platform","type":"str"},{"property":"navigator.hardwareConcurrency","type":"uint"},{"property":"navigator.maxTouchPoints","type":"uint"},{"property":"screen.availLeft","type":"uint"},{"property":"screen.availTop","type":"uint"},{"property":"screen.availWidth","type":"uint"},{"property":"screen.availHeight","type":"uint"},{"property":"screen.width","type":"uint"},{"property":"screen.height","type":"uint"},{"property":"screen.colorDepth","type":"uint"},{"property":"screen.pixelDepth","type":"uint"},{"property":"window.outerWidth","type":"uint"},{"property":"window.outerHeight","type":"uint"},{"property":"window.screenX","type":"int"},{"property":"window.screenY","type":"int"},{"property":"fonts","type":"array"},{"property":"mediaDevices:enabled","type":"bool"},{"property":"mediaDevices:micros","type":"uint"},{"property":"mediaDevices:webcams","type":"uint"},{"property":"mediaDevices:speakers","type":"uint"},{"property":"mediaDevices:microphoneLabels","type":"array"},{"property":"mediaDevices:microphoneGroups","type":"array"},{"property":"mediaDevices:webcamLabels","type":"array"},{"property":"mediaDevices:webcamGroups","type":"array"},{"property":"mediaDevices:speakerLabels","type":"array"},{"property":"mediaDevices:speakerGroups","type":"array"},{"property":"webGl:vendor","type":"str"},{"property":"webGl:renderer","type":"str"},{"property":"webGl:supportedExtensions","type":"array"},{"property":"webGl:parameters","type":"dict"},{"property":"webGl:shaderPrecisionFormats","type":"dict"},{"property":"webGl2:supportedExtensions","type":"array"},{"property":"webGl2:parameters","type":"dict"},{"property":"webGl2:shaderPrecisionFormats","type":"dict"},{"property":"voices","type":"array"},{"property":"voices:blockIfNotDefined","type":"bool"},{"property":"allowMainWorld","type":"bool"},{"property":"navigator.language","type":"str"},{"property":"navigator.languages","type":"array","removed":"156.0.1-beta.32"},{"property":"headers.Accept-Language","type":"str"},{"property":"locale:language","type":"str"},{"property":"locale:region","type":"str"},{"property":"locale:script","type":"str"},{"property":"locale:all","type":"str"},{"property":"timezone","type":"str"}])").as_array();
    Properties result;
    for (const auto &item : list) result[string_or(item.as_object(), "property")] = {string_or(item.as_object(), "type"), false};
    return result;
}

const std::string nvidia = "ANGLE (NVIDIA, NVIDIA GeForce GTX 980 Direct3D11 vs_5_0 ps_5_0), or similar";
const std::string intel = "ANGLE (Intel, Intel(R) HD Graphics Direct3D11 vs_5_0 ps_5_0), or similar";
const std::string basic = "ANGLE (Microsoft, Microsoft Basic Render Driver Direct3D11 vs_5_0 ps_5_0), or similar";
const std::string linux_gpu = "NVIDIA GeForce GTX 980, or similar";
std::string webgl_record(std::string_view vendor, std::string_view renderer, bool depth_clamp) {
    json::object record{{"vendor", vendor},
                        {"renderer", renderer},
                        {"supportedExtensions", json::array{"ANGLE_instanced_arrays", "EXT_blend_minmax", "OVR_multiview2", "WEBGL_debug_renderer_info", "WEBGL_multi_draw"}},
                        {"params", {{"3379", {{"value", 16384}}}, {"33902", {{"value", json::array{1, 1}}}}}},
                        {"shaderPrecisionFormats", json::array{json::object{{"shaderType", 35633}, {"precisionType", 36338}, {"shaderPrecisionFormat", {{"rangeMin", 127}, {"rangeMax", 127}, {"precision", 23}}}}}}};
    if (depth_clamp) record["supportedExtensions"].as_array().emplace_back("EXT_depth_clamp");
    return json::serialize(record);
}
// A network shaped like fpgen's for the nodes generation reads: Firefox on
// Windows or Linux, a netbook screen among the Windows ones, a Linux user
// agent beside a Windows platform now and then, a software renderer among the
// Windows GPUs, and no WebGL2 on Linux.
struct Network : Fixture {
    ValueId firefox = add(R"("Firefox")"), chrome = add(R"("Chrome")"), windows = add(R"("Windows")"), linux = add(R"("Linux")"),
            windows_ua = add(R"("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:146.0) Gecko/20100101 Firefox/146.0")"),
            old_windows_ua = add(R"("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0")"),
            linux_ua = add(R"("Mozilla/5.0 (X11; Linux x86_64; rv:146.0) Gecko/20100101 Firefox/146.0")"),
            wide = add(R"({"availLeft":0,"availTop":0,"availWidth":1920,"availHeight":1040,"width":1920,"height":1080,"colorDepth":24,"pixelDepth":24})"),
            netbook = add(R"({"availWidth":1024,"availHeight":560,"width":1024,"height":600,"colorDepth":32,"pixelDepth":32})"),
            linux_screen = add(R"({"availWidth":2560,"availHeight":1440,"width":2560,"height":1440,"colorDepth":24,"pixelDepth":24})"),
            wide_window = add(R"({"outerWidth":1936,"outerHeight":1056,"screenX":-8,"screenY":-8})"),
            netbook_window = add(R"({"outerWidth":1024,"outerHeight":560,"screenX":0})"),
            linux_window = add(R"({"outerWidth":2000,"outerHeight":1400,"screenX":300})"),
            win32 = add(R"("Win32")"), linux_platform = add(R"("Linux x86_64")"),
            windows_cpu = add(R"("Windows NT 10.0; Win64; x64")"), linux_cpu = add(R"("Linux x86_64")"),
            no_touch = add("0"), ten_touch = add("10"),
            nvidia_gpu = add(json::serialize(json::object{{"vendor", "Google Inc. (NVIDIA)"}, {"renderer", nvidia}})),
            intel_gpu = add(json::serialize(json::object{{"vendor", "Google Inc. (Intel)"}, {"renderer", intel}})),
            basic_gpu = add(json::serialize(json::object{{"vendor", "Google Inc. (Microsoft)"}, {"renderer", basic}})),
            linux_gpu_value = add(json::serialize(json::object{{"vendor", "NVIDIA Corporation"}, {"renderer", linux_gpu}})),
            nvidia_webgl = add(webgl_record("Google Inc. (NVIDIA)", nvidia, false)),
            intel_webgl = add(webgl_record("Google Inc. (Intel)", intel, false)),
            basic_webgl = add(webgl_record("Google Inc. (Microsoft)", basic, false)),
            linux_webgl = add(webgl_record("NVIDIA Corporation", linux_gpu, true)),
            webgl2 = add(webgl_record("", "", false)), no_webgl2 = add("[]");
    fpgen::Model model = load({
        node("browser", {}, codes({firefox, chrome}), table({{firefox, .7}, {chrome, .3}})),
        node("os", {"browser"}, codes({windows, linux}), {{code(firefox), table({{windows, .6}, {linux, .4}})}, {code(chrome), table({{windows, 1}})}}),
        node("navigator.userAgent", {"os"}, codes({windows_ua, old_windows_ua, linux_ua}),
             {{code(windows), table({{windows_ua, .9}, {old_windows_ua, .1}})}, {code(linux), table({{linux_ua, 1}})}}),
        node("screen", {"os"}, codes({wide, netbook, linux_screen}),
             {{code(windows), table({{wide, .5}, {netbook, .5}})}, {code(linux), table({{linux_screen, 1}})}}),
        node("window", {"screen"}, codes({wide_window, netbook_window, linux_window}),
             {{code(wide), table({{wide_window, 1}})}, {code(netbook), table({{netbook_window, 1}})}, {code(linux_screen), table({{linux_window, 1}})}}),
        node("navigator.platform", {"navigator.userAgent"}, codes({win32, linux_platform}),
             {{code(windows_ua), table({{win32, 1}})}, {code(old_windows_ua), table({{win32, 1}})}, {code(linux_ua), table({{linux_platform, .5}, {win32, .5}})}}),
        node("navigator.oscpu", {"navigator.userAgent"}, codes({windows_cpu, linux_cpu}),
             {{code(windows_ua), table({{windows_cpu, 1}})}, {code(old_windows_ua), table({{windows_cpu, 1}})}, {code(linux_ua), table({{linux_cpu, 1}})}}),
        node("navigator.maxTouchPoints", {"navigator.userAgent"}, codes({no_touch, ten_touch}),
             {{code(windows_ua), table({{no_touch, .5}, {ten_touch, .5}})}, {code(old_windows_ua), table({{no_touch, 1}})}, {code(linux_ua), table({{no_touch, 1}})}}),
        node("gpu", {"os"}, codes({nvidia_gpu, intel_gpu, basic_gpu, linux_gpu_value}),
             {{code(windows), table({{nvidia_gpu, .5}, {intel_gpu, .4}, {basic_gpu, .1}})}, {code(linux), table({{linux_gpu_value, 1}})}}),
        node("webgl", {"gpu"}, codes({nvidia_webgl, intel_webgl, basic_webgl, linux_webgl}),
             {{code(nvidia_gpu), table({{nvidia_webgl, 1}})}, {code(intel_gpu), table({{intel_webgl, 1}})}, {code(basic_gpu), table({{basic_webgl, 1}})}, {code(linux_gpu_value), table({{linux_webgl, 1}})}}),
        node("webgl2", {"gpu", "webgl"}, codes({webgl2, no_webgl2}),
             {{code(nvidia_gpu), {{code(nvidia_webgl), table({{webgl2, 1}})}}},
              {code(intel_gpu), {{code(intel_webgl), table({{webgl2, 1}})}}},
              {code(basic_gpu), {{code(basic_webgl), table({{webgl2, 1}})}}},
              {code(linux_gpu_value), {{code(linux_webgl), table({{no_webgl2, 1}})}}}}),
    });
};
Host windows_host(std::optional<Gpu> gpu) {
    return {Os::windows, 13, 1'000'000'000'000ull, true, std::move(gpu)};
}
} // namespace

TEST_CASE("fpgen's user agents name the Firefox that launches", "[firefox-identity]") {
    using detail::rewrite_version;
    CHECK(rewrite_version("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:146.0) Gecko/20100101 Firefox/146.0", "158") ==
          "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0");
    CHECK(rewrite_version("Windows NT 10.0; 1146.0; 146.01; 146.0.1", "158") == "Windows NT 10.0; 1146.0; 146.01; 158.0.1");
    CHECK(detail::app_version("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0") == "5.0 (Windows)");
    CHECK(detail::app_version("Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:146.0) Gecko/20100101 Firefox/146.0") == "5.0 (X11; Ubuntu)");
    CHECK_FALSE(detail::app_version("Opera/9.80"));
}

TEST_CASE("generated geometry is a current screen with the window inside it", "[firefox-identity]") {
    auto windows = json::parse(R"({"screen.width":1920,"screen.height":1080,"screen.availWidth":1920,"screen.availHeight":1040,
        "window.outerWidth":1936,"window.outerHeight":1056,"window.screenX":-8,"window.screenY":-8,"navigator.maxTouchPoints":10})").as_object();
    detail::fix_geometry(windows, Os::windows);
    // Baseline fingerprinting protection: the screen less a 48px taskbar.
    CHECK(windows.at("screen.availHeight") == 1032);
    CHECK(windows.at("window.outerWidth") == 1920);
    CHECK(windows.at("window.outerHeight") == 1032);
    CHECK(windows.at("window.screenX") == 0);
    CHECK(windows.at("window.screenY") == 0);
    CHECK(windows.at("navigator.maxTouchPoints") == 5);

    auto netbook = json::parse(R"({"screen.width":1024,"screen.height":600,"screen.availWidth":1024,"screen.availHeight":560})").as_object();
    detail::fix_geometry(netbook, Os::windows);
    CHECK(netbook.at("screen.width") == 1366);
    CHECK(netbook.at("screen.height") == 768);
    CHECK(netbook.at("screen.availHeight") == 720);

    auto portrait = json::parse(R"({"screen.width":1080,"screen.height":1920,"screen.availWidth":1080,"screen.availHeight":1880})").as_object();
    detail::fix_geometry(portrait, Os::linux);
    CHECK(portrait.at("screen.width") == 1920);
    CHECK(portrait.at("screen.height") == 1080);
    CHECK(portrait.at("screen.availHeight") == 1080);

    auto linux = json::parse(R"json({"navigator.userAgent":"Mozilla/5.0 (X11; Linux x86_64; rv:146.0)","navigator.platform":"Win32","navigator.oscpu":"Windows NT 10.0"})json").as_object();
    detail::fix_geometry(linux, Os::linux);
    CHECK(string_or(linux, "navigator.platform") == "Linux x86_64");
    CHECK(string_or(linux, "navigator.oscpu") == "Linux x86_64");
}

TEST_CASE("fonts are the Windows base on Windows and a drawn machine on Linux", "[firefox-identity]") {
    fpgen::Random rng(1);
    auto host = windows_host(std::nullopt);
    const auto eleven = detail::fonts(host, rng);
    CHECK(has(eleven, "Segoe UI"));
    CHECK(has(eleven, "Segoe UI Variable"));
    host.windows_11_fonts = false;
    const auto ten = detail::fonts(host, rng);
    CHECK(has(ten, "Arial"));
    CHECK_FALSE(has(ten, "Segoe UI Variable"));
    CHECK(ten.size() < eleven.size());
    // The Windows base takes no draw.
    CHECK(rng.next() == fpgen::Random(1).next());

    const Host linux{Os::linux, 8, 0, false, std::nullopt};
    fpgen::Random first(5), second(5);
    const auto drawn = detail::fonts(linux, first);
    CHECK(drawn == detail::fonts(linux, second));
    for (const auto marker : {"Noto Sans", "DejaVu Sans Mono", "Twemoji Mozilla"}) CHECK(has(drawn, marker));
    bool differs = false;
    for (std::uint64_t seed = 6; seed < 30 && !differs; ++seed) {
        fpgen::Random other(seed);
        differs = detail::fonts(linux, other) != drawn;
    }
    CHECK(differs);
}

TEST_CASE("media devices come as aligned labels and groups", "[firefox-identity]") {
    for (const auto os : {Os::windows, Os::linux})
        for (std::uint64_t seed = 0; seed < 50; ++seed) {
            fpgen::Random rng(seed), again(seed);
            const auto devices = detail::media_devices(os, rng);
            CHECK(devices == detail::media_devices(os, again));
            CHECK(devices.at("mediaDevices:enabled") == true);
            for (const auto &[kind, count] : {std::pair{"microphone", "micros"}, std::pair{"webcam", "webcams"}, std::pair{"speaker", "speakers"}}) {
                const auto &labels = devices.at(std::string("mediaDevices:") + kind + "Labels").as_array();
                const auto &groups = devices.at(std::string("mediaDevices:") + kind + "Groups").as_array();
                CHECK(labels.size() == groups.size());
                CHECK(devices.at(std::string("mediaDevices:") + count).to_number<std::size_t>() == labels.size());
                for (const auto &group : groups) CHECK(std::string_view(group.as_string()).starts_with("hw-"));
            }
            const auto &speakers = devices.at("mediaDevices:speakerLabels").as_array();
            CHECK(speakers.size() >= 1);
            // PulseAudio's monitor sources are Linux's alone.
            const auto &mics = devices.at("mediaDevices:microphoneLabels").as_array();
            for (const auto &speaker : speakers)
                CHECK(has(mics, "Monitor of " + std::string(speaker.as_string())) == (os == Os::linux));
        }
}

TEST_CASE("speech voices follow the display language", "[firefox-identity]") {
    fpgen::Random rng(3);
    const auto english = detail::voices(Os::windows, "en-US", rng);
    std::vector<std::string> names;
    for (const auto &voice : english) names.emplace_back(voice.at("name").as_string());
    for (const auto name : {"Microsoft David - English (United States)", "Microsoft Mark - English (United States)",
                            "Microsoft Zira - English (United States)"})
        CHECK(std::find(names.begin(), names.end(), name) != names.end());
    CHECK(english.front().at("voiceUri") == "urn:moz-tts:sapi:Microsoft David - English (United States)?en-US");
    for (const auto &voice : english) {
        CHECK(voice.at("isDefault") == false);
        CHECK(voice.at("isLocalService") == true);
    }
    const auto chinese = detail::voices(Os::windows, "zh-CN", rng);
    CHECK(chinese.front().at("name") == "Microsoft Huihui - Chinese (Simplified, PRC)");
    CHECK(chinese.front().at("lang") == "zh-CN");
    // A locale without a pack of its own falls back to its language's.
    CHECK(detail::voices(Os::windows, "zh-SG", rng).front().at("lang") == "zh-CN");

    const auto linux = detail::voices(Os::linux, "de-DE", rng);
    CHECK(linux.size() == 131);
    for (const auto &voice : linux) {
        const std::string uri(voice.at("voiceUri").as_string());
        CHECK(uri.starts_with("urn:moz-tts:speechd:"));
        CHECK(uri.find(' ') == std::string::npos);
    }
}

TEST_CASE("WebGL records become the properties Firefox reports", "[firefox-identity]") {
    const auto webgl = json::parse(webgl_record("Google Inc. (NVIDIA)", nvidia, false)).as_object();
    const auto webgl2 = json::parse(webgl_record("", "", false));
    const auto windows = detail::webgl_config(webgl, webgl2, Os::windows);
    CHECK(windows.at("webGl:vendor") == "Google Inc. (NVIDIA)");
    CHECK(windows.at("webGl2Enabled") == true);
    const auto &extensions = windows.at("webGl:supportedExtensions").as_array();
    CHECK_FALSE(has(extensions, "WEBGL_multi_draw"));
    CHECK(has(extensions, "OVR_multiview2"));
    // Added by the shipped Firefox, in case-insensitive order.
    CHECK(position(extensions, "EXT_blend_minmax") < position(extensions, "EXT_depth_clamp"));
    CHECK(position(extensions, "EXT_depth_clamp") < position(extensions, "EXT_polygon_offset_clamp"));
    CHECK(position(extensions, "EXT_polygon_offset_clamp") < position(extensions, "OVR_multiview2"));
    CHECK(windows.at("webGl:parameters").at("3379") == 16384);
    CHECK(windows.at("webGl:shaderPrecisionFormats").at("35633,36338").at("precision") == 23);
    CHECK(windows.contains("webGl2:parameters"));

    const auto linux = detail::webgl_config(json::parse(webgl_record("NVIDIA Corporation", linux_gpu, true)).as_object(), json::array{}, Os::linux);
    CHECK(linux.at("webGl2Enabled") == false);
    CHECK_FALSE(linux.contains("webGl2:parameters"));
    CHECK_FALSE(has(linux.at("webGl:supportedExtensions").as_array(), "OVR_multiview2"));
    CHECK(has(linux.at("webGl:supportedExtensions").as_array(), "EXT_polygon_offset_clamp"));
    const auto plain = detail::webgl_config(json::parse(webgl_record("Mesa", "llvmpipe", false)).as_object(), json::array{}, Os::linux);
    CHECK_FALSE(has(plain.at("webGl:supportedExtensions").as_array(), "EXT_polygon_offset_clamp"));
}

TEST_CASE("coherence rules repair what they can and report the rest", "[firefox-identity]") {
    auto repairable = json::parse(R"json({"screen.width":1920,"screen.height":1080,"screen.availWidth":2000,"screen.availHeight":1040,
        "screen.colorDepth":32,"navigator.maxTouchPoints":256,"window.devicePixelRatio":1.818,"webGl:renderer":"ANGLE (Intel)"})json").as_object();
    CHECK(detail::cohere(repairable, Os::windows).empty());
    CHECK(repairable.at("screen.availWidth") == 1920);
    CHECK(repairable.at("screen.colorDepth") == 24);
    CHECK(repairable.at("screen.pixelDepth") == 24);
    CHECK(repairable.at("navigator.maxTouchPoints") == 0);
    CHECK(repairable.at("window.devicePixelRatio") == 1.75);

    auto broken = json::parse(R"json({"screen.width":800,"screen.height":1200,"webGl:renderer":"Mesa Intel",
        "navigator.userAgent":"Mozilla/5.0 (X11; Linux x86_64)","navigator.platform":"Linux armv81"})json").as_object();
    const auto violations = detail::cohere(broken, Os::windows);
    REQUIRE(violations.size() == 3);
    CHECK(violations[0].starts_with("gpu-matches-os"));
    CHECK(violations[1].starts_with("screen-shape"));
    CHECK(violations[2].starts_with("arch-agreement"));
}

TEST_CASE("launch configuration applies the account locale and timezone", "[firefox-identity]") {
    auto identity = json::parse(R"({"navigator.userAgent":"UA","locale:region":"US","timezone":"America/New_York"})").as_object();
    auto chinese = identity;
    detail::apply_locale(chinese, "zh-CN", "Asia/Shanghai");
    CHECK(string_or(chinese, "navigator.userAgent") == "UA");
    CHECK(string_or(chinese, "navigator.language") == "zh-CN");
    CHECK(string_or(chinese, "headers.Accept-Language") == "zh-CN,zh;q=0.9");
    CHECK(string_or(chinese, "locale:language") == "zh");
    CHECK(string_or(chinese, "locale:region") == "CN");
    CHECK(string_or(chinese, "locale:all") == "zh-CN, zh");
    CHECK(string_or(chinese, "timezone") == "Asia/Shanghai");
    auto scripted = identity;
    detail::apply_locale(scripted, "zh-Hant-TW", "Asia/Taipei");
    CHECK(string_or(scripted, "locale:language") == "zh");
    CHECK(string_or(scripted, "locale:region") == "TW");
    CHECK(string_or(scripted, "locale:script") == "Hant");
    CHECK_FALSE(chinese.contains("locale:script"));
    auto bare = identity;
    detail::apply_locale(bare, "de", "Europe/Berlin");
    CHECK(string_or(bare, "headers.Accept-Language") == "de");
    CHECK_FALSE(bare.contains("locale:region"));
    auto unchanged = identity;
    detail::apply_locale(unchanged, "", "");
    CHECK(string_or(unchanged, "timezone") == "America/New_York");
    CHECK_FALSE(unchanged.contains("navigator.language"));
}

TEST_CASE("an identity claims the host's GPU when fpgen has recorded it", "[firefox-identity]") {
    Network network;
    const auto identity = generate(network.model, "model-test", windows_host(Gpu{"Google Inc. (NVIDIA)", nvidia}), 42, "158.0.1-beta.41");
    const auto &config = identity.config;
    CHECK(identity.os == Os::windows);
    CHECK(identity.model == "model-test");
    CHECK(identity.browser == "158.0.1-beta.41");
    CHECK(identity.catalog == catalog_digest());
    CHECK(identity.claims_host_gpu);
    CHECK_FALSE(identity.canvas_covered);
    CHECK(config.at("webGl:renderer").as_string() == std::string_view(nvidia));
    CHECK(config.at("webGl:vendor") == "Google Inc. (NVIDIA)");
    const std::string user_agent(config.at("navigator.userAgent").as_string());
    CHECK(user_agent.find("rv:158.0") != std::string::npos);
    CHECK(user_agent.ends_with("Firefox/158.0"));
    CHECK(config.at("navigator.appVersion") == "5.0 (Windows)");
    CHECK(config.at("navigator.platform") == "Win32");
    CHECK(config.at("navigator.hardwareConcurrency") == 12);  // 13 cores, snapped down
    CHECK(config.at("screen.width").to_number<int>() >= 1366);
    CHECK(config.at("screen.availHeight").to_number<int>() == config.at("screen.height").to_number<int>() - 48);
    CHECK(config.at("screen.colorDepth") == 24);
    CHECK(has(config.at("fonts").as_array(), "Segoe UI Variable"));
    CHECK(has(config.at("webGl:supportedExtensions").as_array(), "EXT_polygon_offset_clamp"));
    CHECK_FALSE(config.contains("voices"));
    const auto &prefs = identity.prefs;
    CHECK(prefs.at("gfx.blacklist.webgpu") == 1);
    CHECK(prefs.at("dom.webgpu.enabled") == true);
    CHECK(prefs.at("webgl.enable-webgl2") == true);
    CHECK(prefs.at("ui.useOverlayScrollbars") == 1);
    CHECK(prefs.at("dom.quotaManager.temporaryStorage.fixedLimit") == 488281250);
    CHECK_FALSE(prefs.contains("privacy.baselineFingerprintingProtection.overrides"));

    // The salt decides the identity, so a probe and the next launch agree.
    const auto again = generate(network.model, "model-test", windows_host(Gpu{"Google Inc. (NVIDIA)", nvidia}), 42, "158.0.1-beta.41");
    CHECK(to_json(again) == to_json(identity));
    bool differs = false;
    for (std::uint64_t salt = 43; salt < 60 && !differs; ++salt)
        differs = generate(network.model, "model-test", windows_host(Gpu{"Google Inc. (NVIDIA)", nvidia}), salt, "158.0.1-beta.41").config != config;
    CHECK(differs);
}

TEST_CASE("an identity on an unrecorded or software GPU draws a recorded one", "[firefox-identity]") {
    Network network;
    for (std::uint64_t salt = 0; salt < 30; ++salt) {
        const auto unrecorded = generate(network.model, "m", windows_host(Gpu{"Google Inc. (AMD)", "ANGLE (AMD, Radeon RX 7900)"}), salt, "158.0");
        CHECK(unrecorded.canvas_covered);
        CHECK_FALSE(unrecorded.claims_host_gpu);
        CHECK(unrecorded.prefs.at("gfx.blacklist.webgpu") == 4);
        CHECK(std::string_view(unrecorded.prefs.at("privacy.baselineFingerprintingProtection.overrides").as_string()).starts_with("+CanvasImageExtractionPrompt"));
        const std::string renderer(unrecorded.config.at("webGl:renderer").as_string());
        CHECK((renderer == nvidia || renderer == intel));

        for (const auto &host : {windows_host(Gpu{"Google Inc. (Microsoft)", basic}), windows_host(std::nullopt)}) {
            const auto software = generate(network.model, "m", host, salt, "158.0");
            CHECK_FALSE(software.canvas_covered);
            CHECK_FALSE(software.claims_host_gpu);
            CHECK(software.prefs.at("gfx.blacklist.webgpu") == 4);
            CHECK(software.config.at("webGl:renderer").as_string() != std::string_view(basic));
        }
    }
}

TEST_CASE("a Linux identity is a Linux machine", "[firefox-identity]") {
    Network network;
    for (std::uint64_t salt = 0; salt < 20; ++salt) {
        const auto identity = generate(network.model, "m", {Os::linux, 64, 0, false, Gpu{"NVIDIA Corporation", linux_gpu}}, salt, "158.0");
        const auto &config = identity.config;
        CHECK(identity.claims_host_gpu);
        CHECK(std::string_view(config.at("navigator.userAgent").as_string()).starts_with("Mozilla/5.0 (X11; Linux x86_64; rv:158.0)"));
        CHECK(config.at("navigator.platform") == "Linux x86_64");
        CHECK(config.at("navigator.oscpu") == "Linux x86_64");
        CHECK(config.at("navigator.appVersion") == "5.0 (X11)");
        CHECK(config.at("navigator.hardwareConcurrency") == 32);
        CHECK(config.at("screen.availHeight") == config.at("screen.height"));
        const auto x = config.at("window.screenX").to_number<int>(), y = config.at("window.screenY").to_number<int>();
        CHECK((x >= 0 && x <= 560 && y >= 0 && y < 40));
        CHECK(has(config.at("fonts").as_array(), "Noto Sans"));
        CHECK_FALSE(config.contains("webGl2:parameters"));
        CHECK(identity.prefs.at("webgl.enable-webgl2") == false);
        CHECK(identity.prefs.at("dom.webgpu.enabled") == false);
        CHECK_FALSE(identity.prefs.contains("gfx.blacklist.webgpu"));
        CHECK(identity.prefs.at("gfx.font_rendering.fallback.async") == false);
        CHECK_FALSE(identity.prefs.contains("dom.quotaManager.temporaryStorage.fixedLimit"));
    }
}

TEST_CASE("stored identities read back and launch in the account's locale", "[firefox-identity]") {
    Network network;
    const auto identity = generate(network.model, "m", windows_host(Gpu{"Google Inc. (Intel)", intel}), 7, "158.0.1-beta.41");
    const auto stored = to_json(identity);
    const auto loaded = from_json(json::parse(json::serialize(stored)).as_object());
    CHECK(to_json(loaded) == stored);
    CHECK(loaded.salt == 7);
    CHECK(loaded.host_gpu == Gpu{"Google Inc. (Intel)", intel});
    auto corrupt = stored;
    corrupt["format"] = 2;
    CHECK_THROWS_AS(from_json(corrupt), Error);

    auto build = properties();
    build["webGl:contextAttributes"] = {"dict", true};
    const auto launched = launch(loaded, "zh-CN", "Asia/Shanghai", "159.0.1-beta.1", build);
    const auto &config = launched.config;
    CHECK(std::string_view(config.at("navigator.userAgent").as_string()).ends_with("Firefox/159.0"));
    CHECK(config.at("navigator.language") == "zh-CN");
    CHECK(config.at("timezone") == "Asia/Shanghai");
    CHECK(config.at("voices").as_array().front().at("lang") == "zh-CN");
    CHECK(config.at("voices:blockIfNotDefined") == true);
    CHECK(config.at("allowMainWorld") == true);
    CHECK(launched.prefs.at("intl.locale.requested") == "zh-CN");
    CHECK(launched.dropped.empty());
    // The same locale draws the same voices every launch.
    CHECK(launch(loaded, "zh-CN", "Asia/Shanghai", "159.0", build).config.at("voices") == config.at("voices"));
    CHECK(launch(loaded, "", "", "158.0", build).prefs.at("intl.locale.requested") == "en-US");

    auto older = properties();
    older.erase("mediaDevices:webcamGroups");
    older["voices:blockIfNotDefined"].removed = true;
    const auto trimmed = launch(loaded, "en-US", "UTC", "158.0", older);
    CHECK_FALSE(trimmed.config.contains("mediaDevices:webcamGroups"));
    CHECK_FALSE(trimmed.config.contains("voices:blockIfNotDefined"));
    CHECK(trimmed.dropped.size() == 2);

    auto wrong = properties();
    wrong["fonts"].type = "str";
    CHECK_THROWS_AS(launch(loaded, "en-US", "UTC", "158.0", wrong), Error);
}

TEST_CASE("the browser's version and properties are read beside it", "[firefox-identity]") {
    Directory directory;
    CHECK_THROWS_AS(browser_version(directory.path), Error);
    CHECK_THROWS_AS(load_properties(directory.path), Error);
    std::ofstream(directory.path / "application.ini", std::ios::binary)
        << "[App]\r\nVendor=Mozilla\r\nVersion=158.0.1-beta.41\r\n\r\n[Gecko]\r\nMinVersion=158.0.1\r\n";
    std::ofstream(directory.path / "properties.json", std::ios::binary)
        << R"([{"property":"fonts","type":"array"},{"property":"webgl2:parameters","type":"dict","removed":"130.0.1-beta.12"}])";
    CHECK(browser_version(directory.path) == "158.0.1-beta.41");
    const auto loaded = load_properties(directory.path);
    REQUIRE(loaded.size() == 2);
    CHECK(loaded.at("fonts").type == "array");
    CHECK(loaded.at("webgl2:parameters").removed);
}

TEST_CASE("the catalogues are embedded with their digest", "[firefox-identity]") {
    CHECK(catalog_digest().size() == 64);
    CHECK(catalog_launcher() == "0.5.9");
    const auto host = local_host();
#ifdef _WIN32
    CHECK(host.os == Os::windows);
#else
    CHECK(host.os == Os::linux);
#endif
    CHECK(host.logical_cores > 0);
    CHECK_FALSE(host.gpu);
}

// AIS2API_TEST_FPGEN_MODEL=<directory with the pinned model files>: generate
// identities for this host from the real model. AIS2API_TEST_HOST_GPU=
// "<vendor>|<renderer>" is the GPU Camoufox reports here (none: every identity
// draws one); AIS2API_TEST_FPGEN_OUTPUT=<file> writes the identities as JSON
// lines for scripts/camoufox-dev.py check-identities.
TEST_CASE("the real fpgen model generates coherent identities for this host", "[.fpgen-model]") {
    const char *directory = std::getenv("AIS2API_TEST_FPGEN_MODEL");
    if (!directory) SKIP("AIS2API_TEST_FPGEN_MODEL is not set");
    std::map<std::string, std::string> pins;
    for (const auto name : {"fingerprint-network.json.zst", "values.json.zst", "values.dat.zst"})
        pins[name] = sha256(read_file(fs::path(directory) / name));
    const auto model = fpgen::Model::load(directory, pins);
    auto host = local_host();
    if (const char *gpu = std::getenv("AIS2API_TEST_HOST_GPU")) {
        const std::string_view text(gpu);
        REQUIRE(text.find('|') != std::string_view::npos);
        host.gpu = Gpu{std::string(text.substr(0, text.find('|'))), std::string(text.substr(text.find('|') + 1))};
    }
    const char *output_path = std::getenv("AIS2API_TEST_FPGEN_OUTPUT");
    std::ofstream output;
    if (output_path) output.open(output_path, std::ios::binary);
    const auto count = std::getenv("AIS2API_TEST_FPGEN_COUNT") ? std::stoi(std::getenv("AIS2API_TEST_FPGEN_COUNT")) : 500;
    std::map<std::string, int> renderers;
    for (int i = 0; i < count; ++i) {
        const auto identity = generate(model, "model-2/2026", host, static_cast<std::uint64_t>(i) * 7919 + 1, "158.0.1-beta.41");
        auto config = identity.config;
        CHECK(detail::cohere(config, host.os).empty());
        CHECK(std::string_view(config.at("navigator.userAgent").as_string()).find(host.os == Os::windows ? "Windows NT" : "Linux") !=
              std::string_view::npos);
        ++renderers[std::string(config.at("webGl:renderer").as_string())];
        if (output) output << json::serialize(to_json(identity)) << '\n';
    }
    // Without a host GPU to claim, identities draw from fpgen's records.
    if (!host.gpu) CHECK(renderers.size() >= 5);
    for (const auto &[renderer, _] : renderers) CHECK_FALSE(software_renderer(renderer));
}
