// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "config.hpp"
#include <map>
#include <memory>
#include <optional>
namespace ais2api {
namespace fpgen {
class Model;
}
fs::path executable_path();
int run_process(const std::vector<std::string> &args);
void spawn_service(const Config &config);
class InstanceLock {
  public:
    explicit InstanceLock(const fs::path &directory);
    ~InstanceLock();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
json::object local_call(const Config &config, const json::object &command);
class ControlServer {
  public:
    using Handler = std::function<json::object(const json::object &)>;
    ControlServer(const Config &config, Handler handler);
    ~ControlServer();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int serve(Config config);
class GenerationBackend;
int serve_with_backend(Config config, std::unique_ptr<GenerationBackend> backend);
json::object setup_runtime(Config &config, bool approve_system_changes = false);
std::vector<std::string> linux_runtime_packages(std::string_view os_release);
json::object doctor(const Config &config);
json::object rollback_runtime(Config &config);
void download_verified(std::string_view url, const fs::path &destination, std::string_view digest,
                       std::string_view proxy = {}, std::uint64_t expected_size = 0);
// `keep`, when given, names the entries (by their '/'-separated path) that
// are written; the others are checked as strictly but skipped.
void extract_archive(const fs::path &archive, const fs::path &destination,
                     const std::function<bool(std::string_view)> &keep = {});
// The fpgen model the Camoufox backend generates identities from, pinned by
// runtime-manifest.json and installed by setup.
struct FpgenModelPin {
    std::string tag, url, sha256;
    std::uint64_t size = 0;
    std::map<std::string, std::string> file_sha256;  // the archive's files
};
FpgenModelPin fpgen_model_pin();  // from the runtime manifest
FpgenModelPin fpgen_model_pin(const json::object &manifest);
// <data_dir>/runtime/fpgen/<tag with '/' as '-'>
fs::path fpgen_model_directory(const fs::path &data_dir, const FpgenModelPin &pin);
// "installed"; "missing"; "damaged": setup's, but not the pinned files;
// "foreign": not created by setup, so setup leaves it alone.
std::string fpgen_model_state(const fs::path &directory, const FpgenModelPin &pin);
// Installs the model from its downloaded archive, replacing a damaged one.
void install_fpgen_model(const fs::path &archive, const fs::path &directory, const FpgenModelPin &pin);
// Whether a dev bundle embeds this pin's model. setup then installs none.
bool fpgen_model_embedded(const FpgenModelPin &pin);
// The pin's model: the embedded one, else setup's install under `data_dir`.
fpgen::Model load_fpgen_model(const fs::path &data_dir, const FpgenModelPin &pin);
// The Camoufox build the Camoufox backend runs, pinned per platform by
// runtime-manifest.json and installed by setup.
struct CamoufoxPin {
    std::string version, platform, url, sha256;
    std::uint64_t size = 0;
};
// Empty when the manifest pins no Camoufox for the platform.
std::optional<CamoufoxPin> camoufox_pin(const json::object &manifest, std::string_view platform);
std::optional<CamoufoxPin> camoufox_pin();  // this host's, from the runtime manifest
// Whether setup extracts an archive entry. The bundled fonts are left out but
// for the emoji font Firefox bundles: an identity always presents the host's
// own OS, and Camoufox then renders with the system's fonts. So is uBlock
// Origin, which ais2api never loads.
bool camoufox_keeps(std::string_view relative);
fs::path camoufox_root(const fs::path &data_dir);  // <data_dir>/runtime/camoufox
// <data_dir>/runtime/camoufox/<version>-<platform>
fs::path camoufox_directory(const fs::path &data_dir, const CamoufoxPin &pin);
// As fpgen_model_state: "installed", "missing", "damaged" or "foreign".
std::string camoufox_state(const fs::path &directory, const CamoufoxPin &pin);
// Installs Camoufox from its downloaded archive, replacing a damaged install.
void install_camoufox(const fs::path &archive, const fs::path &directory, const CamoufoxPin &pin);
// runtime/camoufox/current.json: the install the backend runs, the one
// before it, and whether a rollback holds the current one until setup runs
// again. Both are directory names under camoufox_root.
struct CamoufoxSelection {
    std::string current, previous;
    bool held = false;
};
CamoufoxSelection camoufox_selection(const fs::path &data_dir);
// Makes `name` current; a different former current becomes previous. Other
// installs setup made are removed.
void select_camoufox(const fs::path &data_dir, const std::string &name);
// Swaps current and previous, and holds the result.
void hold_previous_camoufox(const fs::path &data_dir);
// The executable the Camoufox backend runs: browser_executable when it names
// Camoufox ("browser_executable"), else the current install ("setup"). The
// path is empty when there is neither.
struct CamoufoxExecutable {
    fs::path path;
    std::string source;
};
CamoufoxExecutable camoufox_executable(const Config &config);
std::string fetch_public_media(std::string_view url, std::size_t limit, std::string_view proxy = {},
                               std::stop_token cancel = {});
} // namespace ais2api
