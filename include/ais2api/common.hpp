// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ais2api {
namespace json = boost::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
inline std::string path_utf8(const fs::path &path) {
    auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
struct Error : std::runtime_error {
    int status;
    std::string code;
    unsigned retry_after = 0;
    std::string quota_scope = "unknown";
    Error(int s, std::string c, std::string message)
        : std::runtime_error(std::move(message)), status(s), code(std::move(c)) {}
};
inline void check_cancel(std::stop_token token) {
    if (token.stop_requested())
        throw Error(499, "cancelled", "Request cancelled");
}
inline const json::value *find(const json::object &o, std::string_view k) {
    return o.if_contains(k);
}
inline std::string string_or(const json::object &o, std::string_view k, std::string fallback = {}) {
    auto p = o.if_contains(k);
    return p && p->is_string() ? std::string(p->as_string()) : std::move(fallback);
}
inline bool bool_or(const json::object &o, std::string_view k, bool fallback = false) {
    auto p = o.if_contains(k);
    return p && p->is_bool() ? p->as_bool() : fallback;
}
inline std::int64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string random_hex(std::size_t bytes);
std::string sha256(std::string_view input);
// Incremental SHA-256 for inputs that are not contiguous in memory.
class Sha256 {
  public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;
    void update(std::string_view input);
    std::string finish();
  private:
    void *context_;
};
// Exact byte length of json::serialize() output, without producing it.
std::size_t json_size(const json::value &value);
std::size_t json_size(const json::object &object);
std::size_t json_size(const json::array &array);
std::size_t json_size(json::string_view text);
void atomic_write(const fs::path &path, std::string_view contents, bool secret = false);
std::string read_file(const fs::path &path, std::size_t limit = 32 * 1024 * 1024);
void secure_directory(const fs::path &path);
json::object error_json(const Error &error, bool openai = false);
// "dev" in a build with AIS2API_DEV_BUNDLE (scripts/build-dev.ps1), else
// "release".
std::string_view build_mode();
} // namespace ais2api
