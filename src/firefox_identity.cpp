// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Ported from the Camoufox Python launcher 0.5.9 (fingerprints.py, webgl.py,
// coherence.py, host_rendering.py and utils.launch_options; MIT License,
// Copyright (c) 2024-2026 daijro and the Camoufox contributors; see
// licenses/camoufox-launcher-LICENSE.txt). Where a step mirrors one function
// there, the comment names it.

#include "ais2api/firefox_identity.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <regex>
#include <set>
#include <thread>
#include <unordered_map>
#if defined(__linux__)
#include <sched.h>
#endif

namespace ais2api::firefox_identity {
namespace {
using fpgen::Random;
using fpgen::ValueId;

struct EmbeddedFile {
    std::string_view name;
    const unsigned char *data;
    std::size_t size;
};
#include "camoufox_catalog.inc"

const json::value &catalog(std::string_view name) {
    static const auto files = [] {
        std::map<std::string, json::value, std::less<>> result;
        for (const auto &file : embedded_camoufox_files)
            result.emplace(std::string(file.name),
                           json::parse(std::string_view(reinterpret_cast<const char *>(file.data), file.size)));
        return result;
    }();
    const auto found = files.find(name);
    if (found == files.end()) throw std::logic_error("browser/camoufox has no " + std::string(name));
    return found->second;
}
const json::object &constants() { return catalog("launcher-constants.json").as_object(); }
std::string_view key(Os os) { return os == Os::windows ? "win" : "lin"; }

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
// Python's truth value of a JSON value.
bool truthy(const json::value *value) {
    if (!value) return false;
    switch (value->kind()) {
    case json::kind::null: return false;
    case json::kind::bool_: return value->get_bool();
    case json::kind::int64: return value->get_int64() != 0;
    case json::kind::uint64: return value->get_uint64() != 0;
    case json::kind::double_: return value->get_double() != 0;
    case json::kind::string: return !value->get_string().empty();
    case json::kind::array: return !value->get_array().empty();
    case json::kind::object: return !value->get_object().empty();
    }
    return false;
}
std::optional<std::int64_t> integer(const json::value *value) {
    if (!value) return std::nullopt;
    if (value->is_int64()) return value->as_int64();
    if (value->is_uint64() && value->as_uint64() <= static_cast<std::uint64_t>(INT64_MAX))
        return static_cast<std::int64_t>(value->as_uint64());
    return std::nullopt;
}
std::optional<std::int64_t> integer(const json::object &object, std::string_view name) {
    return integer(object.if_contains(name));
}
double number_or(const json::object &object, std::string_view name, double fallback) {
    const auto *value = object.if_contains(name);
    return value && value->is_number() ? value->to_number<double>() : fallback;
}
std::vector<std::string> strings(const json::value *value) {
    std::vector<std::string> result;
    if (value && value->is_array())
        for (const auto &item : value->as_array())
            if (item.is_string()) result.emplace_back(item.as_string());
    return result;
}
json::array to_array(const std::vector<std::string> &items) {
    json::array result;
    result.reserve(items.size());
    for (const auto &item : items) result.emplace_back(item);
    return result;
}
const json::array &array_at(const json::object &object, std::string_view name) {
    static const json::array empty;
    const auto *value = object.if_contains(name);
    return value && value->is_array() ? value->as_array() : empty;
}

// Python's random.Random.sample: `count` distinct members in draw order.
std::vector<std::string> sample(std::vector<std::string> pool, std::size_t count, Random &rng) {
    count = std::min(count, pool.size());
    for (std::size_t i = 0; i < count; ++i) std::swap(pool[i], pool[i + rng.below(pool.size() - i)]);
    pool.resize(count);
    return pool;
}
// Python's random.Random.choices with weights: bisect on the running sums.
std::size_t choose_weighted(const std::vector<double> &weights, Random &rng) {
    std::vector<double> sums(weights.size());
    double total = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) sums[i] = total += weights[i];
    const auto point = rng.uniform() * total;
    const auto at = static_cast<std::size_t>(std::upper_bound(sums.begin(), sums.end(), point) - sums.begin());
    return std::min(at, weights.size() - 1);
}

// The parsed JSON of the values a generation reads, fetched from values.dat in
// as few passes as possible.
class Values {
  public:
    explicit Values(const fpgen::Model &model) : model_(model) {}
    void fetch(const std::vector<ValueId> &ids) {
        std::vector<ValueId> missing;
        for (const auto id : ids)
            if (!parsed_.contains(id)) missing.push_back(id);
        if (missing.empty()) return;
        json::parse_options precise;
        precise.numbers = json::number_precision::precise;
        for (auto &[id, text] : model_.values(std::move(missing))) {
            boost::system::error_code error;
            auto value = json::parse(text, error, {}, precise);
            if (error) throw Error(503, "browser_not_configured", "fpgen model is invalid (a value is not JSON): rerun ais2api setup");
            parsed_.emplace(id, std::move(value));
        }
    }
    const json::value &operator[](ValueId id) const { return parsed_.at(id); }

  private:
    const fpgen::Model &model_;
    std::unordered_map<ValueId, json::value> parsed_;
};
// fpgen's trace as the launchers see it: likeliest first, ties in order.
std::vector<fpgen::Weighted> ranked(std::vector<fpgen::Weighted> distribution) {
    std::stable_sort(distribution.begin(), distribution.end(),
                     [](const auto &a, const auto &b) { return a.probability > b.probability; });
    return distribution;
}
ValueId draw(const std::vector<fpgen::Weighted> &distribution, Random &rng) {
    std::vector<double> weights;
    for (const auto &entry : distribution) weights.push_back(entry.probability);
    return distribution.at(choose_weighted(weights, rng)).value;
}

// --- GPUs (webgl.py, fingerprints.py, coherence.py) ---

bool gpu_fits_os(std::string_view renderer, Os os) {
    if (renderer.empty()) return true;
    if (os == Os::windows) return renderer.starts_with("ANGLE");
    return renderer.find("ANGLE") == std::string_view::npos && renderer.find("Apple M") == std::string_view::npos;
}
// _renderer_bucket: Gecko's sanitized device bucket in any of its spellings.
std::string renderer_bucket(std::string_view renderer) {
    static const std::regex angle_d3d(R"(^ANGLE \([^,]*, (.*?) Direct3D.*\)$)");
    static const std::regex angle_vulkan(R"(^ANGLE \((.*)\) on Vulkan$)");
    static const std::regex pcie_sse2(R"(^(.*)/PCIe?/SSE2$)");
    std::string core(renderer);
    if (core.ends_with(", or similar")) core.resize(core.size() - 12);
    std::smatch match;
    if (std::regex_match(core, match, angle_d3d) || std::regex_match(core, match, angle_vulkan)) core = match[1].str();
    if (std::regex_match(core, match, pcie_sse2)) core = match[1].str();
    if (core.starts_with("NVIDIA ")) core.erase(0, 7);
    return core;
}
// gpu_screen_is_plausible: no discrete GPU behind a netbook panel.
bool gpu_fits_screen(std::string_view renderer, std::optional<std::int64_t> width, std::optional<std::int64_t> height) {
    static const std::set<std::string, std::less<>> discrete{"GeForce 8800 GTX", "GeForce GTX 480", "GeForce GTX 980",
                                                              "Radeon R9 200 Series"};
    if (renderer.empty() || !width.value_or(0) || !height.value_or(0) || software_renderer(renderer)) return true;
    if (!discrete.contains(renderer_bucket(renderer))) return true;
    return *width * *height > 1024 * 600;
}

// --- Geometry (fingerprints.py, coherence.py) ---

// A config field read as Python's config.get() sees it: absent is None.
struct Fields {
    json::object &config;
    std::optional<std::int64_t> get(std::string_view name) const { return integer(config, name); }
    bool has(std::string_view name) const { return config.contains(name); }
    void set(std::string_view name, std::int64_t value) { config[name] = value; }
};
bool truth(std::optional<std::int64_t> value) { return value.value_or(0) != 0; }

// coherence._check_screen_shape
std::optional<std::string> screen_shape(const json::object &config) {
    const auto width = integer(config, "screen.width"), height = integer(config, "screen.height");
    if (!truth(width) || !truth(height)) return std::nullopt;
    if (*height > *width) return "portrait screen " + std::to_string(*width) + "x" + std::to_string(*height);
    if (*width < 1024) return "screen " + std::to_string(*width) + "x" + std::to_string(*height) + " is smaller than any desktop panel";
    return std::nullopt;
}
// coherence.repair_screen_orientation
void landscape(Fields fields) {
    const auto width = fields.get("screen.width"), height = fields.get("screen.height");
    if (!truth(width) || !truth(height) || *height <= *width) return;
    fields.set("screen.width", *height);
    fields.set("screen.height", *width);
    const auto avail_width = fields.get("screen.availWidth"), avail_height = fields.get("screen.availHeight");
    if (truth(avail_width) && truth(avail_height)) {
        fields.set("screen.availWidth", *avail_height);
        fields.set("screen.availHeight", *avail_width);
    }
}
// raise_screen_to_modern_floor: nothing smaller than 1366x768.
void modern_floor(Fields fields) {
    const auto width = fields.get("screen.width"), height = fields.get("screen.height");
    if (!truth(width) || !truth(height) || (*width >= 1366 && *height >= 768)) return;
    const auto avail_width = fields.get("screen.availWidth"), avail_height = fields.get("screen.availHeight");
    const auto new_width = std::max<std::int64_t>(*width, 1366), new_height = std::max<std::int64_t>(*height, 768);
    fields.set("screen.width", new_width);
    fields.set("screen.height", new_height);
    if (truth(avail_width)) fields.set("screen.availWidth", std::max<std::int64_t>(1, new_width - std::max<std::int64_t>(0, *width - *avail_width)));
    if (truth(avail_height)) fields.set("screen.availHeight", std::max<std::int64_t>(1, new_height - std::max<std::int64_t>(0, *height - *avail_height)));
}
// clamp_window_dimensions: inner <= outer <= avail <= screen on both axes.
void clamp_window(Fields fields) {
    for (const std::string axis : {"Width", "Height"}) {
        const auto screen_key = "screen." + lower(axis), avail_key = "screen.avail" + axis,
                   outer_key = "window.outer" + axis, inner_key = "window.inner" + axis;
        const auto screen = fields.get(screen_key), avail = fields.get(avail_key), outer = fields.get(outer_key),
                   inner = fields.get(inner_key);
        if (truth(screen) && truth(avail) && *avail > *screen) fields.set(avail_key, *screen);
        const auto cap = fields.has(avail_key) ? fields.get(avail_key) : screen;
        if (truth(outer) && truth(cap) && *outer > *cap) {
            const auto chrome = truth(inner) ? std::max<std::int64_t>(0, *outer - *inner) : 0;
            fields.set(outer_key, *cap);
            if (truth(inner)) fields.set(inner_key, std::max<std::int64_t>(1, *cap - chrome));
        }
        const auto outer_now = fields.has(outer_key) ? fields.get(outer_key) : outer;
        const auto inner_now = fields.get(inner_key);
        if (truth(inner_now) && truth(outer_now) && *inner_now > *outer_now) fields.set(inner_key, *outer_now);
    }
}
// clamp_window_position: the window inside its screen.
void clamp_position(Fields fields) {
    for (const auto &[axis, position_key] : {std::pair{"width", "window.screenX"}, std::pair{"height", "window.screenY"}}) {
        const auto screen = fields.get(std::string("screen.") + axis);
        const auto outer = fields.get(std::string("window.outer") + (axis == std::string_view("width") ? "Width" : "Height"));
        const auto position = fields.get(position_key);
        if (!position || !truth(screen) || !truth(outer)) continue;
        fields.set(position_key, std::max<std::int64_t>(0, std::min(*position, *screen - *outer)));
    }
}

// --- fpgen to properties (fingerprints.from_fpgen, fpgen.yml) ---

// _cast_to_properties for one mapped field.
void cast(json::object &config, std::string_view name, const json::value *value) {
    if (!truthy(value)) return;
    json::value data = *value;
    if (data.is_array()) {
        if (data.as_array().size() != 1 || !data.as_array().front().is_string()) return;
        data = json::value(data.as_array().front());
    }
    if (name.starts_with("screen.") && data.is_int64() && data.as_int64() < 0) data = 0;
    config[name] = std::move(data);
}

// The fields of a sampled fingerprint the launcher keeps, with the
// launcher's own derivations: appVersion from the user agent, and screenY.
json::object from_fpgen(const json::value &user_agent, const json::value &platform, const json::value &oscpu,
                        const json::value &touch, const json::value &screen, const json::value &window, Random &rng) {
    json::object config;
    cast(config, "navigator.userAgent", &user_agent);
    cast(config, "navigator.oscpu", &oscpu);
    cast(config, "navigator.platform", &platform);
    cast(config, "navigator.maxTouchPoints", &touch);
    static const json::object empty;
    const auto &screen_fields = screen.is_object() ? screen.as_object() : empty;
    const auto &window_fields = window.is_object() ? window.as_object() : empty;
    for (const auto name : {"availLeft", "availTop", "availWidth", "availHeight", "height", "width", "colorDepth", "pixelDepth"})
        cast(config, std::string("screen.") + name, screen_fields.if_contains(name));
    for (const auto name : {"outerHeight", "outerWidth", "screenX", "screenY"})
        cast(config, std::string("window.") + name, window_fields.if_contains(name));
    // handle_screenXY
    if (!config.contains("window.screenY")) {
        const auto *screen_x = window_fields.if_contains("screenX");
        const auto x = integer(screen_x);
        if (!truthy(screen_x)) {
            config["window.screenX"] = 0;
            config["window.screenY"] = 0;
        } else if (x && *x >= -50 && *x <= 50) {
            config["window.screenY"] = *x;
        } else {
            const auto y = integer(screen_fields, "availHeight").value_or(0) - integer(window_fields, "outerHeight").value_or(0);
            if (y == 0) config["window.screenY"] = 0;
            else if (y > 0) config["window.screenY"] = static_cast<std::int64_t>(rng.below(static_cast<std::uint64_t>(y)));
            else config["window.screenY"] = y + static_cast<std::int64_t>(rng.below(static_cast<std::uint64_t>(-y)));
        }
    }
    if (const auto derived = detail::app_version(string_or(config, "navigator.userAgent"))) config["navigator.appVersion"] = *derived;
    return config;
}

// The OS a user agent presents, as ua-parser tells Windows from the rest.
std::optional<Os> user_agent_os(std::string_view user_agent) {
    if (user_agent.find("Windows") != std::string_view::npos) return Os::windows;
    if (user_agent.find("Macintosh") != std::string_view::npos || user_agent.find("Mac OS") != std::string_view::npos) return std::nullopt;
    return Os::linux;
}

// fix_hardware_concurrency without pinning: the host's count, snapped down
// into the counts real desktops report.
std::int64_t plausible_cores(unsigned host) {
    const auto counts = array_at(constants(), "plausibleCoreCounts");
    std::int64_t result = counts.empty() ? 4 : counts.front().to_number<std::int64_t>();
    for (const auto &count : counts)
        if (count.to_number<std::int64_t>() <= static_cast<std::int64_t>(host)) result = count.to_number<std::int64_t>();
    return result;
}

std::string major_version(std::string_view version) {
    return std::string(version.substr(0, std::min(version.find_first_not_of("0123456789"), version.size())));
}

// The seed of one per-launch draw, apart from the machine's own sequence.
std::uint64_t derive(std::uint64_t salt, std::string_view purpose) {
    const auto digest = sha256(std::to_string(salt) + ":" + std::string(purpose));
    return std::stoull(digest.substr(0, 16), nullptr, 16);
}

// --- Voices (fingerprints._generate_random_voice_subset) ---

const json::object &weighted_pick(const json::array &items, Random &rng) {
    double total = 0;
    for (const auto &item : items) total += number_or(item.as_object(), "w", 0);
    auto rest = rng.uniform() * total;
    for (const auto &item : items) {
        rest -= number_or(item.as_object(), "w", 0);
        if (rest <= 0) return item.as_object();
    }
    return items.back().as_object();
}
template <class Weight>
std::vector<std::string> weighted_sample(std::vector<std::string> pool, std::size_t count, Weight weight, Random &rng) {
    std::vector<std::string> result;
    while (!pool.empty() && result.size() < count) {
        double total = 0;
        for (const auto &item : pool) total += weight(item);
        auto rest = rng.uniform() * total;
        bool taken = false;
        for (auto at = pool.begin(); at != pool.end(); ++at) {
            rest -= weight(*at);
            if (rest <= 0) {
                result.push_back(*at);
                pool.erase(at);
                taken = true;
                break;
            }
        }
        if (!taken) {
            result.push_back(pool.back());
            pool.pop_back();
        }
    }
    return result;
}
std::string voice_uri(Os os, std::string_view name, std::string_view lang) {
    if (os == Os::windows) return "urn:moz-tts:sapi:" + std::string(name) + "?" + std::string(lang);
    // SpeechDispatcherService: spaces and non-ASCII bytes escaped.
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string escaped;
    for (const unsigned char c : name) {
        if (c == ' ') escaped += "%20";
        else if (c < 0x80) escaped += static_cast<char>(c);
        else {
            escaped += '%';
            escaped += hex[c >> 4];
            escaped += hex[c & 15];
        }
    }
    return "urn:moz-tts:speechd:" + escaped + "?" + std::string(lang);
}
} // namespace

std::string_view os_name(Os os) { return os == Os::windows ? "windows" : "linux"; }

bool software_renderer(std::string_view renderer) {
    for (const auto name : {"llvmpipe", "Microsoft Basic Render Driver", "SwiftShader", "Generic Renderer"})
        if (renderer.find(name) != std::string_view::npos) return true;
    return false;
}

std::string_view catalog_digest() { return embedded_camoufox_digest; }
std::string catalog_launcher() { return string_or(catalog("catalog.json").as_object(), "camoufox_python"); }

std::string detail::rewrite_version(std::string_view text, std::string_view major) {
    // (?<!\d)(1[0-9]{2})(\.0)(?!\d), widened to any three-digit major.
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string result;
    std::size_t i = 0;
    while (i < text.size()) {
        if (i + 5 <= text.size() && text[i] >= '1' && text[i] <= '9' && digit(text[i + 1]) && digit(text[i + 2]) &&
            text[i + 3] == '.' && text[i + 4] == '0' && (i == 0 || !digit(text[i - 1])) &&
            (i + 5 == text.size() || !digit(text[i + 5]))) {
            result += major;
            result += ".0";
            i += 5;
        } else {
            result += text[i++];
        }
    }
    return result;
}

std::optional<std::string> detail::app_version(std::string_view user_agent) {
    // _app_version_from_user_agent: the parenthesised OS tokens without the
    // architecture, the Gecko revision and the Windows version.
    constexpr std::string_view prefix = "Mozilla/5.0 (";
    if (!user_agent.starts_with(prefix)) return std::nullopt;
    const auto end = user_agent.find(')', prefix.size());
    if (end == std::string_view::npos) return std::nullopt;
    const auto block = user_agent.substr(prefix.size(), end - prefix.size());
    std::vector<std::string> kept;
    for (std::size_t start = 0; start <= block.size();) {
        const auto stop = std::min(block.find(';', start), block.size());
        auto token = block.substr(start, stop - start);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.remove_suffix(1);
        start = stop + 1;
        if (token.starts_with("rv:") || token == "Win64" || token == "x64" || token == "Mobile" || token == "Tablet" ||
            token.starts_with("Linux ") || token.starts_with("Intel Mac OS X"))
            continue;
        kept.emplace_back(token.starts_with("Windows") ? "Windows" : std::string(token));
    }
    if (kept.empty()) return std::nullopt;
    std::string result = "5.0 (";
    for (std::size_t i = 0; i < kept.size(); ++i) result += (i ? "; " : "") + kept[i];
    return result + ")";
}

void detail::fix_geometry(json::object &config, Os os) {
    Fields fields{config};
    if (screen_shape(config)) {
        landscape(fields);
        modern_floor(fields);
    }
    // fix_navigator_arch: a Linux platform and oscpu that name the user
    // agent's architecture.
    if (os == Os::linux) {
        const auto user_agent = string_or(config, "navigator.userAgent");
        const std::string arch = user_agent.find("Linux x86_64") != std::string::npos ? "Linux x86_64"
                                 : user_agent.find("Linux i686") != std::string::npos ? "Linux i686"
                                                                                      : "";
        if (!arch.empty()) {
            config["navigator.platform"] = arch;
            config["navigator.oscpu"] = arch;
        }
    }
    modern_floor(fields);
    // follow_baseline_protection: the available screen and touch points
    // Firefox's baseline fingerprinting protection reports.
    const auto width = fields.get("screen.width"), height = fields.get("screen.height");
    if (truth(width) && truth(height)) {
        fields.set("screen.availWidth", *width);
        fields.set("screen.availHeight", *height - integer(constants().at("baselineTaskbarHeight").as_object(), key(os)).value_or(0));
    }
    if (fields.get("navigator.maxTouchPoints").value_or(0) > 1)
        fields.set("navigator.maxTouchPoints", integer(constants(), "baselineMultiTouchPoints").value_or(5));
    clamp_window(fields);
    clamp_position(fields);
}

json::array detail::fonts(const Host &host, Random &rng) {
    const auto os = key(host.os);
    const auto full = strings(catalog("fonts.json").as_object().if_contains(os));
    const auto essential_list = strings(catalog("essential-fonts.json").as_object().if_contains(os));
    const std::set<std::string> essential(essential_list.begin(), essential_list.end());
    std::vector<std::string> result;
    if (host.os == Os::windows) {
        // A Windows identity on Windows renders with the system's own fonts,
        // so only the OS base is claimed, never an addition the host lacks.
        const std::set<std::string> listed(full.begin(), full.end());
        for (const auto &font : full)
            if (essential.contains(font)) result.push_back(font);
        for (const auto &font : essential)
            if (!listed.contains(font)) result.push_back(font);
        if (!host.windows_11_fonts) {
            const auto markers = strings(constants().if_contains("windows11MarkerFonts"));
            std::erase_if(result, [&](const std::string &font) { return std::find(markers.begin(), markers.end(), font) != markers.end(); });
        }
        return to_array(result);
    }
    // One OS-version base by weight, in full.
    const auto &bases = array_at(catalog("font-bases.json").as_object(), os);
    if (!bases.empty()) {
        const auto roll = rng.uniform();
        double cumulative = 0;
        const json::object *picked = nullptr;
        for (const auto &base : bases) {
            cumulative += number_or(base.as_object(), "weight", 0);
            if (roll < cumulative) {
                picked = &base.as_object();
                break;
            }
        }
        result = strings((picked ? *picked : bases.back().as_object()).if_contains("fonts"));
    }
    if (result.empty())
        for (const auto &font : full)
            if (essential.contains(font)) result.push_back(font);
    std::set<std::string> chosen(result.begin(), result.end());
    for (const auto &font : full)
        if (essential.contains(font) && chosen.insert(font).second) result.push_back(font);
    // _draw_units: each addition at its own probability. The launcher draws
    // without a locale, so units bound to one never apply.
    std::vector<std::string> additions;
    for (const auto &item : array_at(catalog("font-groups.json").as_object(), os)) {
        const auto &unit = item.as_object();
        if (!string_or(unit, "requiresLocale").empty()) continue;
        if (rng.uniform() >= number_or(unit, "prob", 0)) continue;
        std::vector<std::string> members;
        for (auto &font : strings(unit.if_contains("fonts")))
            if (!chosen.contains(font)) members.push_back(std::move(font));
        if (members.empty()) continue;
        if (string_or(unit, "kind") == "alacarte") {
            std::vector<std::pair<std::int64_t, double>> sizes;
            for (const auto &size : array_at(unit, "sizes"))
                sizes.emplace_back(integer(size.as_object(), "n").value_or(1), number_or(size.as_object(), "w", 0));
            if (sizes.empty()) sizes.emplace_back(static_cast<std::int64_t>(members.size()), 1.0);
            double total = 0;
            for (const auto &size : sizes) total += size.second;
            const auto roll = rng.uniform() * total;
            double cumulative = 0;
            auto count = sizes.back().first;
            for (const auto &[n, w] : sizes) {
                cumulative += w;
                if (roll < cumulative) {
                    count = n;
                    break;
                }
            }
            count = std::clamp<std::int64_t>(count, 1, static_cast<std::int64_t>(members.size()));
            members = sample(std::move(members), static_cast<std::size_t>(count), rng);
        }
        additions.insert(additions.end(), members.begin(), members.end());
    }
    for (const auto &font : additions)
        if (chosen.insert(font).second) result.push_back(font);
    // The marker fonts fingerprinting scripts detect the OS by.
    const auto markers = strings(constants().at("markerFonts").as_object().if_contains(os));
    const std::set<std::string> existing(result.begin(), result.end());
    for (const auto &font : markers)
        if (!existing.contains(font)) result.push_back(font);
    return to_array(result);
}

json::object detail::media_devices(Os os, Random &rng) {
    // draw_media_devices: one machine's sound card, cameras, headset and
    // display audio, each device's microphones and outputs in one group.
    const auto &catalogue = catalog("media-devices.json").as_object().at(key(os)).as_object();
    std::vector<std::pair<std::string, std::string>> mics, outs, cams;
    int counter = 0;
    const auto group = [&] { return "hw-" + std::to_string(++counter); };
    const auto choice = [&](const json::array &items) -> const json::object & {
        // _weighted_choice
        double total = 0;
        for (const auto &item : items) total += number_or(item.as_object(), "w", 1);
        auto rest = rng.uniform() * total;
        for (const auto &item : items) {
            rest -= number_or(item.as_object(), "w", 1);
            if (rest < 0) return item.as_object();
        }
        return items.back().as_object();
    };
    const auto add = [&](const json::object &item, const std::string &owner, bool with_mics) {
        if (with_mics)
            for (auto &mic : strings(item.if_contains("mics"))) mics.emplace_back(std::move(mic), owner);
        for (auto &out : strings(item.if_contains("outs"))) outs.emplace_back(std::move(out), owner);
        if (truthy(item.if_contains("cam"))) cams.emplace_back(string_or(item, "cam"), group());
    };
    const double no_mic_share = os == Os::windows ? 0.08 : 0.20, builtin_camera_share = os == Os::windows ? 0.78 : 0.45;
    const auto &card = choice(array_at(catalogue, "cards"));
    const bool no_mic = rng.uniform() < no_mic_share;
    const auto card_group = group();
    add(card, card_group, !no_mic);
    if (rng.uniform() < (no_mic ? builtin_camera_share * 0.3 : builtin_camera_share)) {
        json::array builtin;
        for (const auto &camera : array_at(catalogue, "cameras"))
            if (!truthy(camera.as_object().if_contains("mic"))) builtin.push_back(camera);
        if (!builtin.empty()) {
            auto label = string_or(choice(builtin), "cam");
            cams.emplace_back(std::move(label), group());
        }
    }
    if (rng.uniform() < number_or(catalogue, "p_headset", 0)) {
        const auto &headset = choice(array_at(catalogue, "headsets"));
        add(headset, group(), true);
    }
    if (rng.uniform() < number_or(catalogue, "p_display", 0)) {
        const auto &display = choice(array_at(catalogue, "displays"));
        add(display, group(), true);
    }
    if (rng.uniform() < number_or(catalogue, "p_extra_camera", 0)) {
        json::array external;
        for (const auto &camera : array_at(catalogue, "cameras"))
            if (truthy(camera.as_object().if_contains("mic"))) external.push_back(camera);
        if (external.empty()) external = array_at(catalogue, "cameras");
        const auto &camera = choice(external);
        const auto owner = group();
        cams.emplace_back(string_or(camera, "cam"), owner);
        if (truthy(camera.if_contains("mic"))) mics.emplace_back(string_or(camera, "mic"), owner);
    }
    // PulseAudio lists a monitor source per output as a microphone.
    if (bool_or(catalogue, "monitor_sources"))
        for (const auto &[label, owner] : std::vector(outs)) mics.emplace_back("Monitor of " + label, owner);
    const auto column = [](const auto &pairs, bool second) {
        json::array result;
        for (const auto &pair : pairs) result.emplace_back(second ? pair.second : pair.first);
        return result;
    };
    return {{"mediaDevices:enabled", true},
            {"mediaDevices:micros", mics.size()},
            {"mediaDevices:webcams", cams.size()},
            {"mediaDevices:speakers", outs.size()},
            {"mediaDevices:microphoneLabels", column(mics, false)},
            {"mediaDevices:microphoneGroups", column(mics, true)},
            {"mediaDevices:webcamLabels", column(cams, false)},
            {"mediaDevices:webcamGroups", column(cams, true)},
            {"mediaDevices:speakerLabels", column(outs, false)},
            {"mediaDevices:speakerGroups", column(outs, true)}};
}

json::array detail::voices(Os os, std::string_view locale, Random &rng) {
    const auto &manifest = catalog("voice-manifests.json").as_object().at(key(os)).as_object();
    std::vector<std::string> out, legacy;
    std::set<std::string> seen;
    const auto add = [&](const json::value *entries) {
        for (auto &entry : strings(entries))
            if (seen.insert(entry).second) out.push_back(std::move(entry));
    };
    static const json::object no_packs;
    const auto *packs_value = manifest.if_contains("langPacks");
    const auto &packs = packs_value && packs_value->is_object() ? packs_value->as_object() : no_packs;
    const auto take_pack = [&](const json::object &pack) {
        add(pack.if_contains("oneCore"));
        if (truthy(pack.if_contains("desktop")) && rng.uniform() < number_or(pack, "desktopProb", 0))
            for (auto &entry : strings(pack.if_contains("desktop")))
                if (std::find(legacy.begin(), legacy.end(), entry) == legacy.end()) legacy.push_back(std::move(entry));
    };
    add(manifest.if_contains("base"));
    std::set<std::string> chosen;
    if (!packs.empty()) {
        // _resolve_display_pack: the display language's pack.
        const auto fallback = string_or(manifest, "fallbackLocale", "en-US");
        std::string pack = packs.contains(fallback) ? fallback : std::string(packs.begin()->key());
        if (!locale.empty()) {
            const auto language = lower(locale.substr(0, locale.find('-')));
            if (packs.contains(locale)) pack = std::string(locale);
            else
                for (const auto &item : packs)
                    if (lower(std::string_view(item.key()).substr(0, std::string_view(item.key()).find('-'))) == language) {
                        pack = std::string(item.key());
                        break;
                    }
        }
        chosen.insert(pack);
        take_pack(packs.at(pack).as_object());
    }
    for (const auto &item : array_at(manifest, "additions")) {
        const auto &addition = item.as_object();
        if (truthy(addition.if_contains("deferred"))) continue;
        const auto required = string_or(addition, "requiresLocale");
        if (!required.empty() && !lower(locale).starts_with(lower(required))) continue;
        if (rng.uniform() >= number_or(addition, "prob", 0)) continue;
        const auto kind = string_or(addition, "kind");
        if (kind == "bundle") {
            add(addition.if_contains("voices"));
        } else if (kind == "alacarte") {
            static const json::array one{json::object{{"n", 1}, {"w", 1}}};
            const auto &sizes = array_at(addition, "sizes").empty() ? one : array_at(addition, "sizes");
            const auto count = static_cast<std::size_t>(std::max<std::int64_t>(0, integer(weighted_pick(sizes, rng), "n").value_or(1)));
            for (auto &entry : weighted_sample(strings(addition.if_contains("voices")), count, [](const std::string &) { return 1.0; }, rng)) {
                if (!seen.insert(entry).second) continue;
                // A downloaded voice sits in its alphabetical place.
                const auto at = std::find_if(out.begin(), out.end(), [&](const std::string &voice) { return lower(voice) > lower(entry); });
                out.insert(at, std::move(entry));
            }
        } else if (kind == "groups" && !packs.empty()) {
            std::vector<std::string> eligible;
            for (auto &name : strings(addition.if_contains("groups")))
                if (packs.contains(name) && !chosen.contains(name)) eligible.push_back(std::move(name));
            if (eligible.empty()) continue;
            const auto count = array_at(addition, "sizes").empty()
                                   ? eligible.size()
                                   : static_cast<std::size_t>(std::max<std::int64_t>(0, integer(weighted_pick(array_at(addition, "sizes"), rng), "n").value_or(1)));
            const auto weight = [&](const std::string &name) {
                const auto value = number_or(packs.at(name).as_object(), "weight", 0);
                return value ? value : 0.01;
            };
            for (const auto &name : weighted_sample(std::move(eligible), count, weight, rng)) {
                chosen.insert(name);
                take_pack(packs.at(name).as_object());
            }
        }
    }
    out.insert(out.end(), legacy.begin(), legacy.end());
    json::array result;
    for (const auto &entry : out) {
        // "name:lang:type", split from the right.
        const auto type_at = entry.rfind(':');
        if (type_at == std::string::npos || type_at == 0) continue;
        const auto lang_at = entry.rfind(':', type_at - 1);
        if (lang_at == std::string::npos) continue;
        const auto name = entry.substr(0, lang_at), lang = entry.substr(lang_at + 1, type_at - lang_at - 1);
        // No voice is the default: stock Firefox marks none.
        result.push_back(json::object{{"name", name},
                                      {"lang", lang},
                                      {"voiceUri", voice_uri(os, name, lang)},
                                      {"isDefault", false},
                                      {"isLocalService", entry.substr(type_at + 1) == "local"}});
    }
    return result;
}

json::object detail::webgl_config(const json::object &webgl, const json::value &webgl2, Os os) {
    // webgl.to_config
    const bool has_webgl2 = truthy(&webgl2);
    const auto extensions1 = strings(webgl.if_contains("supportedExtensions"));
    std::vector<std::string> backend;
    if (os == Os::windows) backend.emplace_back("EXT_depth_clamp");
    if ((os == Os::windows && has_webgl2) ||
        (os == Os::linux && std::find(extensions1.begin(), extensions1.end(), "EXT_depth_clamp") != extensions1.end()))
        backend.emplace_back("EXT_polygon_offset_clamp");
    const auto context = [&](json::object &config, const std::string &prefix, const json::object &source) {
        // Never in a release Firefox; OVR_multiview2 depends on the host's GL
        // driver off Windows.
        std::set<std::string_view> blocked{"WEBGL_multi_draw", "WEBGL_clip_cull_distance", "EXT_texture_norm16",
                                           "WEBGL_compressed_texture_etc1"};
        if (os != Os::windows) blocked.insert("OVR_multiview2");
        std::vector<std::string> extensions;
        for (auto &extension : strings(source.if_contains("supportedExtensions")))
            if (!blocked.contains(extension)) extensions.push_back(std::move(extension));
        for (const auto &extension : backend) {
            if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end()) continue;
            // WebGLExtensionID order: alphabetical, ignoring case.
            const auto at = std::find_if(extensions.begin(), extensions.end(), [&](const std::string &other) { return lower(other) > lower(extension); });
            extensions.insert(at, extension);
        }
        json::object parameters;
        if (const auto *params = source.if_contains("params"); params && params->is_object())
            for (const auto &item : params->as_object())
                if (const auto *value = item.value().is_object() ? item.value().as_object().if_contains("value") : nullptr)
                    parameters[item.key()] = *value;
        json::object formats;
        for (const auto &entry : array_at(source, "shaderPrecisionFormats")) {
            if (!entry.is_object()) continue;
            const auto text = [](const json::value *value) {
                if (!value) return std::string("None");
                return value->is_string() ? std::string(value->as_string()) : json::serialize(*value);
            };
            const auto &fields = entry.as_object();
            const auto *format = fields.if_contains("shaderPrecisionFormat");
            formats[text(fields.if_contains("shaderType")) + "," + text(fields.if_contains("precisionType"))] = format ? *format : json::value();
        }
        config[prefix + ":supportedExtensions"] = to_array(extensions);
        config[prefix + ":parameters"] = std::move(parameters);
        config[prefix + ":shaderPrecisionFormats"] = std::move(formats);
    };
    json::object config{{"webGl:vendor", string_or(webgl, "vendor")}, {"webGl:renderer", string_or(webgl, "renderer")}};
    context(config, "webGl", webgl);
    config["webGl2Enabled"] = has_webgl2;
    if (has_webgl2 && webgl2.is_object()) {
        context(config, "webGl2", webgl2.as_object());
        // Firefox creates WebGL1 on WebGL2's profile, so both report one line
        // width range.
        if (const auto *range = config.at("webGl2:parameters").as_object().if_contains("33902"))
            config.at("webGl:parameters").as_object()["33902"] = *range;
    }
    return config;
}

std::vector<std::string> detail::cohere(json::object &config, Os os) {
    struct Rule {
        std::string_view name;
        std::function<std::optional<std::string>(const json::object &)> check;
        std::function<void(json::object &)> repair;
    };
    const auto renderer = [](const json::object &c) { return string_or(c, "webGl:renderer"); };
    const auto apple_cores = [] {
        std::vector<std::int64_t> cores;
        for (const auto &count : array_at(constants(), "appleSiliconCores")) cores.push_back(count.to_number<std::int64_t>());
        std::sort(cores.begin(), cores.end());
        return cores;
    }();
    const auto plausible_dpr = [&] {
        std::vector<double> steps;
        for (const auto &step : array_at(constants().at("plausibleDpr").as_object(), key(os))) steps.push_back(step.to_number<double>());
        return steps;
    }();
    constexpr std::int64_t chrome_height = 86;
    const std::vector<Rule> rules{
        {"apple-silicon-cores",
         [&](const json::object &c) -> std::optional<std::string> {
             const auto cores = integer(c, "navigator.hardwareConcurrency");
             if (renderer(c).find("Apple M") == std::string::npos || !cores ||
                 std::binary_search(apple_cores.begin(), apple_cores.end(), *cores))
                 return std::nullopt;
             return "Apple Silicon with hardwareConcurrency " + std::to_string(*cores);
         },
         [&](json::object &c) {
             const auto cores = integer(c, "navigator.hardwareConcurrency");
             if (!cores || apple_cores.empty()) return;
             const auto at = std::lower_bound(apple_cores.begin(), apple_cores.end(), *cores);
             c["navigator.hardwareConcurrency"] = at == apple_cores.end() ? apple_cores.back() : *at;
         }},
        {"gpu-matches-os",
         [&](const json::object &c) -> std::optional<std::string> {
             const auto value = renderer(c);
             if (gpu_fits_os(value, os)) return std::nullopt;
             return std::string(os_name(os)) + " identity with " + value;
         },
         nullptr},
        {"color-depth",
         [](const json::object &c) -> std::optional<std::string> {
             const auto *depth = c.if_contains("screen.colorDepth");
             if (!depth || depth->is_null()) return std::nullopt;
             if (depth->is_number() && (depth->to_number<double>() == 24 || depth->to_number<double>() == 30)) return std::nullopt;
             return "screen.colorDepth " + json::serialize(*depth);
         },
         [](json::object &c) {
             const auto *depth = c.if_contains("screen.colorDepth");
             if (depth && !depth->is_null() && !(depth->is_number() && (depth->to_number<double>() == 24 || depth->to_number<double>() == 30)))
                 c["screen.colorDepth"] = 24;
             if (c.contains("screen.pixelDepth") || c.contains("screen.colorDepth"))
                 c["screen.pixelDepth"] = c.contains("screen.colorDepth") ? c.at("screen.colorDepth") : json::value(24);
         }},
        {"touch-points",
         [](const json::object &c) -> std::optional<std::string> {
             const auto *touch = c.if_contains("navigator.maxTouchPoints");
             if (!touch || touch->is_null()) return std::nullopt;
             const auto value = integer(touch);
             if (!value || *value < 0 || *value > 10) return "navigator.maxTouchPoints " + json::serialize(*touch);
             return std::nullopt;
         },
         [](json::object &c) {
             const auto *touch = c.if_contains("navigator.maxTouchPoints");
             if (!touch || touch->is_null()) return;
             const auto value = integer(touch);
             if (!value || *value > 10) c["navigator.maxTouchPoints"] = 0;
         }},
        {"device-pixel-ratio",
         [&](const json::object &c) -> std::optional<std::string> {
             const auto *dpr = c.if_contains("window.devicePixelRatio");
             if (!dpr || !dpr->is_number() || plausible_dpr.empty()) return std::nullopt;
             if (std::find(plausible_dpr.begin(), plausible_dpr.end(), dpr->to_number<double>()) != plausible_dpr.end()) return std::nullopt;
             return "window.devicePixelRatio " + json::serialize(*dpr);
         },
         [&](json::object &c) {
             const auto *dpr = c.if_contains("window.devicePixelRatio");
             if (!dpr || !dpr->is_number() || plausible_dpr.empty()) return;
             const auto value = dpr->to_number<double>();
             c["window.devicePixelRatio"] = *std::min_element(plausible_dpr.begin(), plausible_dpr.end(), [&](double a, double b) {
                 return std::abs(a - value) < std::abs(b - value);
             });
         }},
        {"window-chrome",
         [](const json::object &c) -> std::optional<std::string> {
             const auto inner = integer(c, "window.innerHeight"), outer = integer(c, "window.outerHeight");
             if (!truth(inner) || !truth(outer) || *outer - *inner >= chrome_height) return std::nullopt;
             return "window chrome " + std::to_string(*outer - *inner) + "px, less than the browser's";
         },
         [](json::object &c) {
             const auto inner = integer(c, "window.innerHeight"), outer = integer(c, "window.outerHeight");
             if (!truth(inner) || !truth(outer)) return;
             auto avail = integer(c, "screen.availHeight");
             if (!truth(avail)) avail = integer(c, "screen.height");
             if (!truth(avail) || *inner + chrome_height <= *avail) c["window.outerHeight"] = *inner + chrome_height;
             else c["window.innerHeight"] = std::max<std::int64_t>(*outer - chrome_height, 1);
         }},
        {"screen-shape", [](const json::object &c) { return screen_shape(c); }, nullptr},
        {"avail-bounds",
         [](const json::object &c) -> std::optional<std::string> {
             const auto width = integer(c, "screen.width"), height = integer(c, "screen.height");
             const auto avail_width = integer(c, "screen.availWidth"), avail_height = integer(c, "screen.availHeight");
             if (truth(width) && truth(avail_width) && *avail_width > *width) return "screen.availWidth exceeds screen.width";
             if (truth(height) && truth(avail_height) && *avail_height > *height) return "screen.availHeight exceeds screen.height";
             return std::nullopt;
         },
         [](json::object &c) {
             const auto width = integer(c, "screen.width"), height = integer(c, "screen.height");
             if (truth(width) && integer(c, "screen.availWidth").value_or(0) > *width) c["screen.availWidth"] = *width;
             if (truth(height) && integer(c, "screen.availHeight").value_or(0) > *height) c["screen.availHeight"] = *height;
         }},
        {"arch-agreement",
         [](const json::object &c) -> std::optional<std::string> {
             const auto user_agent = string_or(c, "navigator.userAgent"), platform = string_or(c, "navigator.platform"),
                        oscpu = string_or(c, "navigator.oscpu");
             if (user_agent.find("x86_64") != std::string::npos &&
                 (platform.find("armv") != std::string::npos || oscpu.find("armv") != std::string::npos))
                 return "user agent claims x86_64 while platform/oscpu say " + platform + "/" + oscpu;
             return std::nullopt;
         },
         nullptr},
    };
    for (const auto &rule : rules)
        if (rule.repair && rule.check(config)) rule.repair(config);
    std::vector<std::string> violations;
    for (const auto &rule : rules)
        if (const auto detail = rule.check(config)) violations.push_back(std::string(rule.name) + ": " + *detail);
    return violations;
}

void detail::apply_locale(json::object &config, std::string_view locale, std::string_view timezone) {
    if (!locale.empty()) {
        // BCP 47 subtags: language first, then an optional 4-letter script and
        // a 2-letter or 3-digit region.
        std::vector<std::string> parts;
        for (std::size_t start = 0; start <= locale.size();) {
            const auto end = std::min(locale.find('-', start), locale.size());
            parts.emplace_back(locale.substr(start, end - start));
            start = end + 1;
        }
        const auto language = lower(parts.front());
        std::string region, script;
        for (std::size_t i = 1; i < parts.size(); ++i)
            if (parts[i].size() == 2 || (parts[i].size() == 3 && std::all_of(parts[i].begin(), parts[i].end(), [](unsigned char c) { return std::isdigit(c); }))) {
                region = parts[i];
                std::transform(region.begin(), region.end(), region.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            } else if (i == 1 && parts[i].size() == 4) {
                script = lower(parts[i]);
                script[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(script[0])));
            }
        const std::string tag(locale);
        // Camoufox derives navigator.languages from locale:all.
        config["navigator.language"] = tag;
        config["headers.Accept-Language"] = language != tag ? tag + "," + language + ";q=0.9" : tag;
        config["locale:language"] = language;
        if (!region.empty()) config["locale:region"] = region;
        else config.erase("locale:region");
        if (!script.empty()) config["locale:script"] = script;
        else config.erase("locale:script");
        config["locale:all"] = language != tag ? tag + ", " + language : tag;
    }
    if (!timezone.empty()) config["timezone"] = std::string(timezone);
}

Identity generate(const fpgen::Model &model, std::string_view model_tag, const Host &host, std::uint64_t salt,
                  std::string_view browser_version) {
    const auto node = [&](std::string_view name) {
        const auto found = model.node(name);
        if (!found) throw Error(503, "browser_not_configured", "fpgen model has no " + std::string(name) + " node: rerun ais2api setup");
        return *found;
    };
    const auto browser_node = node("browser"), os_node = node("os"), user_agent_node = node("navigator.userAgent"),
               screen_node = node("screen"), window_node = node("window"), platform_node = node("navigator.platform"),
               oscpu_node = node("navigator.oscpu"), touch_node = node("navigator.maxTouchPoints"), gpu_node = node("gpu"),
               webgl_node = node("webgl"), webgl2_node = node("webgl2");
    Values values(model);
    // fpgen.generate(browser='Firefox', os=...): the values whose JSON is the
    // condition's, compared case-insensitively.
    {
        auto ids = model.possible_values(browser_node);
        ids.insert(ids.end(), model.possible_values(os_node).begin(), model.possible_values(os_node).end());
        values.fetch(ids);
    }
    const auto matching = [&](std::size_t of, std::string_view text) {
        std::vector<ValueId> ids;
        for (const auto id : model.possible_values(of))
            if (values[id].is_string() && lower(values[id].as_string()) == text) ids.push_back(id);
        if (ids.empty()) throw Error(503, "browser_not_configured", "fpgen model has no " + std::string(text) + " value: rerun ais2api setup");
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    fpgen::Evidence evidence{{browser_node, matching(browser_node, "firefox")},
                             {os_node, matching(os_node, host.os == Os::windows ? "windows" : "linux")}};
    // firefox_gpus: every GPU fpgen saw Firefox report on this OS.
    const auto gpus = ranked(model.trace(gpu_node, evidence));
    if (gpus.empty()) throw Error(503, "browser_not_configured", "fpgen model has no Firefox GPU for this OS: rerun ais2api setup");
    const auto major = major_version(browser_version);
    // plan_rendering: claim the GPU the canvas really renders on when fpgen
    // has recorded it; a hardware GPU it has not recorded gets a recorded one
    // and randomised canvas readback; a software renderer matches any GPU.
    const bool hardware = host.gpu && !software_renderer(host.gpu->renderer);
    fpgen::Random rng(salt);
    std::vector<std::string> last_violations;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto drawn = model.sample({user_agent_node, screen_node, window_node, platform_node, oscpu_node, touch_node}, evidence, rng);
        std::vector<ValueId> wanted;
        for (const auto of : {user_agent_node, screen_node, window_node, platform_node, oscpu_node, touch_node}) wanted.push_back(drawn.at(of));
        for (const auto &gpu : gpus) wanted.push_back(gpu.value);
        values.fetch(wanted);
        auto config = from_fpgen(values[drawn.at(user_agent_node)], values[drawn.at(platform_node)], values[drawn.at(oscpu_node)],
                                 values[drawn.at(touch_node)], values[drawn.at(screen_node)], values[drawn.at(window_node)], rng);
        if (!major.empty())
            for (auto &item : config)
                if (item.value().is_string()) item.value() = detail::rewrite_version(item.value().as_string(), major);
        if (user_agent_os(string_or(config, "navigator.userAgent")) != host.os) {
            last_violations = {"the user agent names another OS"};
            continue;
        }
        config["navigator.hardwareConcurrency"] = plausible_cores(host.logical_cores);
        detail::fix_geometry(config, host.os);
        config["fonts"] = detail::fonts(host, rng);
        for (auto &item : detail::media_devices(host.os, rng)) config[item.key()] = std::move(item.value());

        json::object prefs;
        // Scrollbars follow the Windows version the fonts present.
        bool overlay = true;
        if (host.os == Os::windows) {
            const auto &fonts = config.at("fonts").as_array();
            const auto markers = strings(constants().if_contains("windows11MarkerFonts"));
            overlay = fonts.empty() || std::any_of(markers.begin(), markers.end(), [&](const std::string &marker) {
                          return std::find(fonts.begin(), fonts.end(), json::value(marker)) != fonts.end();
                      });
        }
        prefs["ui.useOverlayScrollbars"] = overlay ? 1 : 0;
        if (host.os == Os::linux) prefs["gfx.font_rendering.fallback.async"] = false;
        // The storage quota a stock profile on this disk reports.
        if (host.disk_bytes)
            prefs["dom.quotaManager.temporaryStorage.fixedLimit"] =
                static_cast<std::int64_t>(std::min<std::uint64_t>(host.disk_bytes / 2 / 1024, std::numeric_limits<std::int32_t>::max()));
        prefs["privacy.donottrackheader.enabled"] = false;
        prefs["privacy.globalprivacycontrol.enabled"] = false;
        prefs["permissions.default.image"] = 1;
        prefs["media.peerconnection.enabled"] = true;
        prefs["browser.tabs.remote.useCrossOriginOpenerPolicy"] = true;

        std::optional<ValueId> gpu_id;
        if (hardware)
            for (const auto &gpu : gpus) {
                const auto &value = values[gpu.value];
                if (value.is_object() && string_or(value.as_object(), "vendor") == host.gpu->vendor &&
                    string_or(value.as_object(), "renderer") == host.gpu->renderer) {
                    gpu_id = gpu.value;
                    break;
                }
            }
        const bool covered = hardware && !gpu_id;
        if (covered)
            prefs["privacy.baselineFingerprintingProtection.overrides"] =
                "+CanvasImageExtractionPrompt,+CanvasExtractionBeforeUserInputIsBlocked,+CanvasExtractionFromThirdPartiesIsBlocked";
        if (!gpu_id) {
            // sample_webgl_for_screen: a GPU the rest of the identity can stand beside.
            std::vector<fpgen::Weighted> candidates;
            const auto width = integer(config, "screen.width"), height = integer(config, "screen.height");
            for (const auto &gpu : gpus) {
                const auto &value = values[gpu.value];
                if (!value.is_object()) continue;
                const auto renderer = string_or(value.as_object(), "renderer");
                if (!software_renderer(renderer) && renderer != "Mozilla" && gpu_fits_os(renderer, host.os) &&
                    gpu_fits_screen(renderer, width, height))
                    candidates.push_back(gpu);
            }
            if (candidates.empty()) {
                last_violations = {"no recorded GPU fits the screen"};
                continue;
            }
            gpu_id = draw(candidates, rng);
        }
        // webgl_for_gpu: one recorded device with this GPU, its WebGL2 drawn
        // given its WebGL1.
        auto pinned = evidence;
        pinned[gpu_node] = {*gpu_id};
        const auto webgl_options = ranked(model.trace(webgl_node, pinned));
        if (webgl_options.empty()) throw Error(503, "browser_not_configured", "fpgen model has no WebGL record for a GPU it lists: rerun ais2api setup");
        const auto webgl_id = draw(webgl_options, rng);
        pinned[webgl_node] = {webgl_id};
        const auto webgl2_options = ranked(model.trace(webgl2_node, pinned));
        if (webgl2_options.empty()) throw Error(503, "browser_not_configured", "fpgen model has no WebGL2 record for a device it lists: rerun ais2api setup");
        const auto webgl2_id = draw(webgl2_options, rng);
        values.fetch({webgl_id, webgl2_id});
        if (!values[webgl_id].is_object()) throw Error(503, "browser_not_configured", "fpgen model has a malformed WebGL record: rerun ais2api setup");
        auto webgl = detail::webgl_config(values[webgl_id].as_object(), values[webgl2_id], host.os);
        prefs["webgl.enable-webgl2"] = webgl.at("webGl2Enabled");
        prefs["webgl.force-enabled"] = true;
        webgl.erase("webGl2Enabled");
        for (auto &item : webgl) config[item.key()] = std::move(item.value());

        if (auto violations = detail::cohere(config, host.os); !violations.empty()) {
            last_violations = std::move(violations);
            continue;
        }
        // set_webgpu: Firefox has WebGPU on Windows, with the host's adapter
        // only when the identity claims the host's GPU.
        prefs["dom.webgpu.enabled"] = host.os == Os::windows;
        if (host.os == Os::windows) prefs["gfx.blacklist.webgpu"] = hardware && !covered ? 1 : 4;

        Identity identity;
        identity.salt = salt;
        identity.os = host.os;
        identity.model = std::string(model_tag);
        identity.browser = std::string(browser_version);
        identity.catalog = std::string(catalog_digest());
        identity.host_gpu = host.gpu;
        identity.claims_host_gpu = hardware && !covered;
        identity.canvas_covered = covered;
        identity.config = std::move(config);
        identity.prefs = std::move(prefs);
        return identity;
    }
    std::string detail;
    for (const auto &violation : last_violations) detail += (detail.empty() ? "" : "; ") + violation;
    throw Error(500, "identity_generation", "Cannot generate a coherent Firefox identity (" + detail + ")");
}

json::object to_json(const Identity &identity) {
    char salt[17];
    std::snprintf(salt, sizeof salt, "%016llx", static_cast<unsigned long long>(identity.salt));
    json::value gpu;
    if (identity.host_gpu) gpu = json::object{{"vendor", identity.host_gpu->vendor}, {"renderer", identity.host_gpu->renderer}};
    return {{"format", 1},
            {"salt", salt},
            {"os", os_name(identity.os)},
            {"model", identity.model},
            {"browser", identity.browser},
            {"catalog", identity.catalog},
            {"hostGpu", std::move(gpu)},
            {"claimsHostGpu", identity.claims_host_gpu},
            {"canvasCovered", identity.canvas_covered},
            {"config", identity.config},
            {"prefs", identity.prefs}};
}

Identity from_json(const json::object &stored) {
    const auto invalid = [] { return Error(500, "identity_invalid", "The stored Firefox identity is not readable: regenerate the account's fingerprint"); };
    const auto format = integer(stored, "format");
    const auto os = string_or(stored, "os");
    const auto salt = string_or(stored, "salt");
    const auto *config = stored.if_contains("config");
    const auto *prefs = stored.if_contains("prefs");
    if (format != 1 || (os != "windows" && os != "linux") || salt.size() != 16 ||
        salt.find_first_not_of("0123456789abcdef") != std::string::npos || !config || !config->is_object() || !prefs || !prefs->is_object())
        throw invalid();
    Identity identity;
    identity.salt = std::stoull(salt, nullptr, 16);
    identity.os = os == "windows" ? Os::windows : Os::linux;
    identity.model = string_or(stored, "model");
    identity.browser = string_or(stored, "browser");
    identity.catalog = string_or(stored, "catalog");
    if (const auto *gpu = stored.if_contains("hostGpu"); gpu && gpu->is_object())
        identity.host_gpu = Gpu{string_or(gpu->as_object(), "vendor"), string_or(gpu->as_object(), "renderer")};
    identity.claims_host_gpu = bool_or(stored, "claimsHostGpu");
    identity.canvas_covered = bool_or(stored, "canvasCovered");
    identity.config = config->as_object();
    identity.prefs = prefs->as_object();
    return identity;
}

Properties load_properties(const fs::path &browser_directory) {
    const auto file = browser_directory / "properties.json";
    std::error_code error;
    if (!fs::is_regular_file(file, error))
        throw Error(503, "browser_not_configured", "Camoufox properties.json is missing beside the executable: reinstall Camoufox");
    boost::system::error_code parse_error;
    const auto value = json::parse(read_file(file, 4 * 1024 * 1024), parse_error);
    if (parse_error || !value.is_array()) throw Error(503, "browser_not_configured", "Camoufox properties.json is not readable: reinstall Camoufox");
    Properties result;
    for (const auto &item : value.as_array()) {
        if (!item.is_object()) continue;
        const auto name = string_or(item.as_object(), "property");
        if (name.empty()) continue;
        result[name] = {string_or(item.as_object(), "type"), item.as_object().contains("removed")};
    }
    return result;
}

std::string browser_version(const fs::path &browser_directory) {
    const auto file = browser_directory / "application.ini";
    std::error_code error;
    if (!fs::is_regular_file(file, error))
        throw Error(503, "browser_not_configured", "Camoufox application.ini is missing beside the executable: reinstall Camoufox");
    const auto text = read_file(file, 1024 * 1024);
    bool app = false;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("[")) app = line == "[App]";
        else if (app && line.starts_with("Version=")) return std::string(line.substr(8));
    }
    throw Error(503, "browser_not_configured", "Camoufox application.ini names no version: reinstall Camoufox");
}

Launch launch(const Identity &identity, std::string_view locale, std::string_view timezone,
              std::string_view browser_version, const Properties &properties) {
    Launch result{identity.config, identity.prefs, {}};
    auto &config = result.config;
    if (const auto major = major_version(browser_version); !major.empty())
        if (auto *user_agent = config.if_contains("navigator.userAgent"); user_agent && user_agent->is_string())
            *user_agent = detail::rewrite_version(user_agent->as_string(), major);
    detail::apply_locale(config, locale, timezone);
    // The browser's own language follows the locale, as in a localized
    // Firefox (Camoufox packages the language packs), so validation messages
    // agree with Intl formatting.
    const auto language = string_or(config, "locale:language"), region = string_or(config, "locale:region");
    const auto requested = language.empty() ? std::string("en-US") : region.empty() ? language : language + "-" + region;
    result.prefs["intl.locale.requested"] = requested;
    // The display language's voices, the same for every launch in it. An
    // empty list still keeps the host's own voices out.
    fpgen::Random rng(derive(identity.salt, "voices"));
    config["voices"] = detail::voices(identity.os, language.empty() ? string_or(config, "navigator.language") : requested, rng);
    config["voices:blockIfNotDefined"] = true;
    // The bridge runs in the page's own world, as AI Studio's code does.
    config["allowMainWorld"] = true;
    // validate_config: a property this build does not read is left out; one
    // of the wrong type is a bug.
    std::vector<std::string> names;
    for (const auto &item : config) names.emplace_back(item.key());
    for (const auto &name : names) {
        const auto found = properties.find(name);
        if (found == properties.end() || found->second.removed) {
            config.erase(name);
            result.dropped.push_back(name);
            continue;
        }
        const auto &value = config.at(name);
        const auto &type = found->second.type;
        const auto whole = [&] {
            return value.is_int64() || value.is_uint64() || (value.is_double() && value.as_double() == std::floor(value.as_double()));
        };
        const bool fits = type == "str"      ? value.is_string()
                          : type == "int"    ? whole()
                          : type == "uint"   ? whole() && value.to_number<double>() >= 0
                          : type == "double" ? value.is_number()
                          : type == "bool"   ? value.is_bool()
                          : type == "array"  ? value.is_array()
                          : type == "dict"   ? value.is_object()
                                             : false;
        if (!fits) throw Error(500, "browser_launch", "Camoufox property " + name + " is not of type " + type);
    }
    return result;
}

Host local_host() {
    Host host;
#ifdef _WIN32
    host.os = Os::windows;
    const auto variable = [](const wchar_t *name) -> std::optional<fs::path> {
        wchar_t *value = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&value, &size, name) != 0 || !value) return std::nullopt;
        fs::path result(value);
        std::free(value);
        if (result.empty()) return std::nullopt;
        return result;
    };
    const auto windows = variable(L"WINDIR").value_or(fs::path(L"C:\\Windows"));
    std::error_code font_error;
    host.windows_11_fonts = fs::exists(windows / "Fonts" / "SegUIVar.ttf", font_error);
    const auto appdata = variable(L"APPDATA");
    const auto home = variable(L"USERPROFILE");
    std::vector<fs::path> candidates;
    if (appdata) candidates.push_back(*appdata / "Mozilla");
    if (home) candidates.push_back(*home);
#else
    host.os = Os::linux;
    std::vector<fs::path> candidates;
    if (const char *home = std::getenv("HOME"); home && *home) {
        candidates.push_back(fs::path(home) / ".mozilla");
        candidates.push_back(fs::path(home));
    }
#endif
    host.logical_cores = std::thread::hardware_concurrency();
#if defined(__linux__)
    // The CPUs this process may run on, as a page timing workers measures.
    if (cpu_set_t set; sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0) host.logical_cores = static_cast<unsigned>(CPU_COUNT(&set));
#endif
    // _stock_profile_disk_capacity_kb: the disk Firefox's profile directory
    // is on, found by walking up to the first directory that exists.
    for (auto probe : candidates) {
        std::error_code error;
        while (!fs::exists(probe, error) && probe != probe.parent_path()) probe = probe.parent_path();
        const auto space = fs::space(probe, error);
        if (!error && space.capacity > 0 && space.capacity != static_cast<std::uintmax_t>(-1)) {
            host.disk_bytes = space.capacity;
            break;
        }
    }
    return host;
}
} // namespace ais2api::firefox_identity
