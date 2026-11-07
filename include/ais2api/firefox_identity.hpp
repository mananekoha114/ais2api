// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Firefox identities for Camoufox, generated on the machine that uses them.
// A port of how the Camoufox Python launcher (camoufox 0.5.9, MIT; see
// licenses/camoufox-launcher-LICENSE.txt) assembles an identity for Windows
// and Linux: fpgen's navigator, screen and window, the host's cores, disk and
// GPU, fonts, media devices and voices from the launcher's catalogues
// (browser/camoufox), WebGL from fpgen's records of one device, and the
// launcher's coherence rules. macOS identities are not ported.

#pragma once
#include "ais2api/common.hpp"
#include "ais2api/fpgen.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace ais2api::firefox_identity {
// An identity always claims the host's own OS.
enum class Os { windows, linux };
std::string_view os_name(Os os);  // "windows", "linux"
struct Gpu {
    std::string vendor, renderer;
    bool operator==(const Gpu &) const = default;
};
// llvmpipe, the Microsoft Basic Render Driver, SwiftShader, a generic renderer.
bool software_renderer(std::string_view renderer);
// What generation reads from the machine it runs on.
struct Host {
    Os os = Os::windows;
    unsigned logical_cores = 0;
    // The disk a stock Firefox profile would live on; 0 when unknown.
    std::uint64_t disk_bytes = 0;
    // Windows: Segoe UI Variable is installed, so the system is Windows 11.
    bool windows_11_fonts = false;
    // The WebGL vendor and renderer Camoufox reports here without an
    // identity; none when it has no WebGL.
    std::optional<Gpu> gpu;
};
// This machine's OS, cores, disk and fonts. `gpu` is the caller's to probe.
Host local_host();

// The part of an identity that belongs to the machine and stays the same
// from launch to launch. Locale, timezone and voices are added per launch.
struct Identity {
    std::uint64_t salt = 0;
    Os os = Os::windows;
    std::string model;     // fpgen model tag
    std::string browser;   // the Camoufox version it was generated for
    std::string catalog;   // digest of the catalogues it was drawn from
    std::optional<Gpu> host_gpu;  // the host's GPU when it was generated
    bool claims_host_gpu = false;
    // Canvas readback is randomised because the identity names another GPU
    // than the one that renders.
    bool canvas_covered = false;
    json::object config;   // Camoufox properties
    json::object prefs;    // Firefox preferences
};
json::object to_json(const Identity &identity);
// Throws Error when the stored identity is not one this version reads.
Identity from_json(const json::object &stored);

// The identity `salt` decides for this host. The same model, host, salt and
// browser version always give the same identity.
Identity generate(const fpgen::Model &model, std::string_view model_tag, const Host &host, std::uint64_t salt,
                  std::string_view browser_version);

// The configuration properties a Camoufox build reads (properties.json).
struct Property {
    std::string type;
    bool removed = false;
};
using Properties = std::map<std::string, Property, std::less<>>;
Properties load_properties(const fs::path &browser_directory);
// The build's version from application.ini ("158.0.1-beta.41").
std::string browser_version(const fs::path &browser_directory);

struct Launch {
    json::object config, prefs;
    // Properties this build does not read, left out.
    std::vector<std::string> dropped;
};
// The configuration and preferences for launching `identity` in an account's
// locale and timezone: the user agent names `browser_version`, the speech
// voices are the locale's, and every property is checked against the build's.
// Throws Error for a property of the wrong type.
Launch launch(const Identity &identity, std::string_view locale, std::string_view timezone,
              std::string_view browser_version, const Properties &properties);

// The embedded catalogues: their digest, and the launcher they come from.
std::string_view catalog_digest();
std::string catalog_launcher();

// The steps of generation and launch, for tests.
namespace detail {
// fpgen's user agents trail the current Firefox: "rv:146.0" and
// "Firefox/146.0" become `major`.0.
std::string rewrite_version(std::string_view text, std::string_view major);
std::optional<std::string> app_version(std::string_view user_agent);
// The navigator, screen and window fix-ups, before fonts and WebGL.
void fix_geometry(json::object &config, Os os);
json::array fonts(const Host &host, fpgen::Random &rng);
json::object media_devices(Os os, fpgen::Random &rng);
json::array voices(Os os, std::string_view locale, fpgen::Random &rng);
// fpgen's webgl and webgl2 values as Camoufox properties; webgl2 is [] for a
// device without WebGL2.
json::object webgl_config(const json::object &webgl, const json::value &webgl2, Os os);
// The coherence rules: repairs what is determined, returns what is left.
std::vector<std::string> cohere(json::object &config, Os os);
void apply_locale(json::object &config, std::string_view locale, std::string_view timezone);
} // namespace detail
} // namespace ais2api::firefox_identity
