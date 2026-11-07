// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "config.hpp"
#include <map>
#include <memory>
namespace ais2api {
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
void extract_archive(const fs::path &archive, const fs::path &destination);
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
std::string fetch_public_media(std::string_view url, std::size_t limit, std::string_view proxy = {},
                               std::stop_token cancel = {});
} // namespace ais2api
