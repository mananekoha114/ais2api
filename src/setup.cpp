// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include "ais2api/backend_router.hpp"
#include "ais2api/media.hpp"
#include "ais2api/browser.hpp"
#include "ais2api/bundle.hpp"
#include "ais2api/camoufox.hpp"
#include "ais2api/console.hpp"
#include "ais2api/firefox_identity.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <boost/asio.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace ais2api {
namespace {
struct CurlInit {
    CurlInit() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT))
            throw Error(500, "network_init", "Cannot initialize downloader");
    }
    ~CurlInit() { curl_global_cleanup(); }
};
using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
Curl curl_handle() {
    static CurlInit init;
    Curl c(curl_easy_init(), curl_easy_cleanup);
    if (!c)
        throw Error(500, "network_init", "Cannot allocate downloader");
    curl_easy_setopt(c.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c.get(), CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_USERAGENT, "ais2api/0.1");
    return c;
}
std::string platform() {
#ifdef _WIN32
    return "windows-x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (std::getenv("PREFIX") &&
        std::string(std::getenv("PREFIX")).find("com.termux") != std::string::npos)
        return "termux-arm64";
    // Ubuntu or Debian on ARM64, such as the arm64 Docker image.
    return "linux-arm64";
#elif defined(__x86_64__)
    return "linux-x64";
#else
    throw Error(400, "unsupported_platform", "Unsupported automatic setup architecture");
#endif
}
json::object manifest() {
    std::vector<fs::path> paths{executable_path().parent_path() / "runtime-manifest.json"};
#ifdef AIS2API_SOURCE_DIR
    paths.emplace_back(fs::path(AIS2API_SOURCE_DIR) / "runtime-manifest.json");
#endif
    for (auto &p : paths)
        if (fs::is_regular_file(p)) {
            auto v = json::parse(read_file(p, 65536));
            if (!v.is_object() || v.as_object().at("version") != 1)
                throw Error(400, "invalid_manifest", "Unsupported runtime manifest");
            return v.as_object();
        }
    throw Error(400, "manifest_missing", "runtime-manifest.json is missing");
}
std::string sh_quote(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\"'\"'";
        else
            out += c;
    }
    return out += "'";
}
void checked_process(const std::vector<std::string> &args) {
    if (run_process(args) != 0)
        throw Error(502, "system_prepare",
                    "System preparation failed; inspect the preceding package-manager output and "
                    "rerun setup");
}
json::object verify_browser(const Config &config, const fs::path &executable) {
    auto verification = config;
    verification.browser_executable = executable;
    verification.headless = true;
    Store store(config.data_dir);
    BrowserGateway gateway(verification, store);
    return gateway.verify_runtime();
}
const char *termux_prefix() {
    const char *prefix = std::getenv("TERMUX__PREFIX");
    return prefix ? prefix : std::getenv("PREFIX");
}
} // namespace
bool media_detail::public_address(const boost::asio::ip::address &address) {
    if (address.is_unspecified() || address.is_loopback() || address.is_multicast())
        return false;
    if (address.is_v6() && address.to_v6().scope_id() != 0)
        return false;
    if (address.is_v4()) {
        auto n = address.to_v4().to_uint();
        auto a = n >> 24, b = (n >> 16) & 255;
        return !(a == 0 || a == 10 || a == 127 || a >= 224 || (a == 169 && b == 254) ||
                 (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
                 (a == 100 && b >= 64 && b <= 127) || (a == 198 && (b == 18 || b == 19)) ||
                 (a == 192 && b == 0));
    }
    auto b = address.to_v6().to_bytes();
    if ((b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80))
        return false;
    bool mapped = true;
    for (unsigned i = 0; i < 10; ++i)
        mapped = mapped && b[i] == 0;
    if (mapped && b[10] == 255 && b[11] == 255)
        return public_address(boost::asio::ip::address_v4({b[12], b[13], b[14], b[15]}));
    // Global unicast is 2000::/3. Exclude IETF special-purpose space,
    // documentation and 6to4 (which can embed nonpublic IPv4 destinations).
    if ((b[0] & 0xe0) != 0x20) return false;
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] < 2) return false; // 2001::/23
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;
    if (b[0] == 0x20 && b[1] == 0x02) return false; // 2002::/16
    if (b[0] == 0x3f && b[1] == 0xff && (b[2] & 0xf0) == 0) return false; // 3fff::/20
    return true;
}
namespace {
curl_socket_t public_socket(void *, curlsocktype, struct curl_sockaddr *address) {
    bool allowed = false;
    if (address->family == AF_INET) {
        auto *p = reinterpret_cast<sockaddr_in *>(&address->addr);
        allowed = media_detail::public_address(boost::asio::ip::address_v4(ntohl(p->sin_addr.s_addr)));
    } else if (address->family == AF_INET6) {
        auto *p = reinterpret_cast<sockaddr_in6 *>(&address->addr);
        boost::asio::ip::address_v6::bytes_type b{};
        std::copy_n(reinterpret_cast<const unsigned char *>(&p->sin6_addr), 16, b.begin());
        allowed = media_detail::public_address(boost::asio::ip::address_v6(b));
    }
    if (!allowed)
        return CURL_SOCKET_BAD;
    return ::socket(address->family, address->socktype, address->protocol);
}
} // namespace
void extract_archive(const fs::path &input, const fs::path &destination,
                     const std::function<bool(std::string_view)> &keep) {
    if (fs::exists(destination) && (!fs::is_directory(destination) || !fs::is_empty(destination)))
        throw Error(409, "directory_not_empty",
                    "Extraction destination must be an empty owned staging directory");
    secure_directory(destination);
    auto root = fs::weakly_canonical(destination);
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(),
                                                                  archive_read_free);
    archive_read_support_filter_gzip(reader.get());
    archive_read_support_format_tar(reader.get());
    archive_read_support_format_zip(reader.get());
#ifdef _WIN32
    auto result = archive_read_open_filename_w(reader.get(), input.c_str(), 65536);
#else
    auto result = archive_read_open_filename(reader.get(), input.c_str(), 65536);
#endif
    if (result != ARCHIVE_OK)
        throw Error(400, "invalid_archive", "Cannot open runtime archive");
    archive_entry *entry{};
    std::set<fs::path> seen;
    std::uint64_t total = 0;
    unsigned count = 0;
    while ((result = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (++count > 100000)
            throw Error(413, "archive_limit", "Archive contains too many entries");
        auto pathname = archive_entry_pathname_utf8(entry);
        if (!pathname)
            throw Error(400, "archive_path", "Archive path must be UTF-8");
        std::string name(pathname);
        std::replace(name.begin(), name.end(), '\\', '/');
        auto relative = fs::u8path(name);
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            name.find(':') != std::string::npos || archive_entry_symlink(entry) ||
            archive_entry_hardlink(entry))
            throw Error(400, "archive_path", "Unsafe archive entry");
        std::string clean;  // the entry's path, '/'-separated, without "." pieces
        for (const auto &piece : relative) {
            if (piece == "..")
                throw Error(400, "archive_path", "Archive entry escapes staging directory");
            if (const auto text = path_utf8(piece); !text.empty() && text != ".")
                clean += (clean.empty() ? "" : "/") + text;
#ifdef _WIN32
            auto component = piece.string();
            if (component != "." && !component.empty()) {
                auto base = component.substr(0, component.find('.'));
                std::transform(base.begin(), base.end(), base.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                bool device =
                    base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                    (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
                     base[3] >= '1' && base[3] <= '9');
                if (device || component.back() == '.' || component.back() == ' ' ||
                    component.find_first_of("<>\"|?*") != std::string::npos)
                    throw Error(400, "archive_path", "Ambiguous Windows archive path");
            }
#endif
        }
        const auto type = archive_entry_filetype(entry);
        if (type != AE_IFDIR && type != AE_IFREG)
            throw Error(400, "archive_path", "Special archive entries are not supported");
        // Entries the caller leaves out pass the same checks but are not written.
        if (keep && !keep(clean))
            continue;
        auto target = (root / relative).lexically_normal();
        if (!seen.insert(target).second && type != AE_IFDIR)
            throw Error(400, "archive_path", "Duplicate archive file");
        if (type == AE_IFDIR) {
            fs::create_directories(target);
            continue;
        }
        // Filesystem aliases (Windows case folding / short names) may name an
        // already extracted entry even when the lexical paths differ.
        if (fs::exists(target))
            throw Error(400, "archive_path", "Archive file aliases an existing entry");
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        if (!out)
            throw Error(500, "extract_write", "Cannot create extracted file");
        std::array<char, 65536> block{};
        la_ssize_t n = 0;
        while ((n = archive_read_data(reader.get(), block.data(), block.size())) > 0) {
            total += static_cast<std::uint64_t>(n);
            if (total > 4ull * 1024 * 1024 * 1024)
                throw Error(413, "archive_limit", "Expanded archive exceeds four GiB");
            out.write(block.data(), n);
            if (!out)
                throw Error(500, "extract_write", "Cannot write extracted file");
        }
        if (n < 0)
            throw Error(400, "invalid_archive", "Truncated runtime archive");
        out.close();
#ifndef _WIN32
        fs::permissions(target, static_cast<fs::perms>(archive_entry_perm(entry) & 0777));
#endif
    }
    if (result != ARCHIVE_EOF)
        throw Error(400, "invalid_archive", "Truncated runtime archive");
}
namespace {
// Written last into a model directory setup installed.
constexpr std::string_view model_marker = ".ais2api-model.json";
bool model_name(std::string_view name, bool slashes) {
    return !name.empty() && name.front() != '.' && name.find("..") == std::string_view::npos &&
           std::all_of(name.begin(), name.end(), [&](unsigned char c) {
               return std::isalnum(c) || c == '.' || c == '-' || c == '_' || (slashes && c == '/');
           });
}
bool digest(std::string_view text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
} // namespace
FpgenModelPin fpgen_model_pin(const json::object &manifest) {
    FpgenModelPin pin;
    try {
        const auto &model = manifest.at("fpgen_model").as_object();
        pin.tag = json::value_to<std::string>(model.at("tag"));
        pin.url = json::value_to<std::string>(model.at("url"));
        pin.sha256 = json::value_to<std::string>(model.at("sha256"));
        pin.size = json::value_to<std::uint64_t>(model.at("size"));
        for (const auto &file : model.at("file_sha256").as_object())
            pin.file_sha256[std::string(file.key())] = json::value_to<std::string>(file.value());
    } catch (const std::exception &) {
        pin = {};
    }
    const bool valid = model_name(pin.tag, true) && pin.url.starts_with("https://") && digest(pin.sha256) &&
                       !pin.file_sha256.empty() &&
                       std::all_of(pin.file_sha256.begin(), pin.file_sha256.end(), [](const auto &file) {
                           return model_name(file.first, false) && digest(file.second);
                       });
    if (!valid) throw Error(400, "invalid_manifest", "runtime-manifest.json has no valid fpgen_model");
    return pin;
}
FpgenModelPin fpgen_model_pin() { return fpgen_model_pin(manifest()); }
fs::path fpgen_model_directory(const fs::path &data_dir, const FpgenModelPin &pin) {
    auto name = pin.tag;
    std::replace(name.begin(), name.end(), '/', '-');
    return data_dir / "runtime" / "fpgen" / fs::u8path(name);
}
std::string fpgen_model_state(const fs::path &directory, const FpgenModelPin &pin) {
    std::error_code error;
    const auto status = fs::symlink_status(directory, error);
    if (!fs::exists(status)) return "missing";
    if (!fs::is_directory(status)) return "foreign";
    json::object owner;
    try {
        owner = json::parse(read_file(directory / model_marker, 65536)).as_object();
    } catch (const std::exception &) {
        return "foreign";
    }
    if (string_or(owner, "tag") != pin.tag || string_or(owner, "sha256") != pin.sha256) return "damaged";
    for (const auto &[name, expected] : pin.file_sha256) {
        const auto path = directory / fs::u8path(name);
        if (!fs::is_regular_file(fs::symlink_status(path, error)) || sha256(read_file(path)) != expected)
            return "damaged";
    }
    return "installed";
}
void install_fpgen_model(const fs::path &archive, const fs::path &directory, const FpgenModelPin &pin) {
    const auto state = fpgen_model_state(directory, pin);
    if (state == "foreign")
        throw Error(409, "unknown_runtime",
                    path_utf8(directory) + " was not created by setup; move it away and rerun setup");
    const auto parent = directory.parent_path();
    secure_directory(parent);
    const auto staging = parent / ("staging-" + random_hex(8));
    try {
        extract_archive(archive, staging);
        // Exactly the pinned files, each checked before the model is moved
        // into place.
        std::set<std::string> found, pinned;
        for (const auto &entry : fs::directory_iterator(staging)) {
            if (!entry.is_regular_file())
                throw Error(502, "archive_layout", "fpgen model archive holds a directory");
            found.insert(path_utf8(entry.path().filename()));
        }
        for (const auto &[name, expected] : pin.file_sha256) pinned.insert(name);
        if (found != pinned)
            throw Error(502, "archive_layout", "fpgen model archive does not hold the pinned files");
        for (const auto &[name, expected] : pin.file_sha256)
            if (sha256(read_file(staging / fs::u8path(name))) != expected)
                throw Error(502, "digest_mismatch", "fpgen model file " + name + " does not match its pinned sha256");
        atomic_write(staging / model_marker,
                     json::serialize(json::object{{"version", 1}, {"tag", pin.tag}, {"sha256", pin.sha256}}));
        if (state != "missing") fs::remove_all(directory);
        fs::rename(staging, directory);
    } catch (...) {
        std::error_code error;
        fs::remove_all(staging, error);
        throw;
    }
}
namespace {
#ifdef AIS2API_DEV_BUNDLE
struct EmbeddedModelFile {
    std::string_view name;
    const unsigned char *data;
    std::size_t size;
    std::string_view sha256;
};
#include "fpgen_model.inc"
#endif
// The embedded model's files when they are the pin's.
std::optional<std::map<std::string, std::string_view>> embedded_model(const FpgenModelPin &pin) {
#ifdef AIS2API_DEV_BUNDLE
    if (embedded_fpgen_tag != pin.tag || std::size(embedded_fpgen_files) != pin.file_sha256.size()) return std::nullopt;
    std::map<std::string, std::string_view> files;
    for (const auto &file : embedded_fpgen_files) {
        const auto pinned = pin.file_sha256.find(std::string(file.name));
        if (pinned == pin.file_sha256.end() || pinned->second != file.sha256) return std::nullopt;
        files.emplace(file.name, std::string_view(reinterpret_cast<const char *>(file.data), file.size));
    }
    return files;
#else
    (void)pin;
    return std::nullopt;
#endif
}
} // namespace
bool fpgen_model_embedded(const FpgenModelPin &pin) { return embedded_model(pin).has_value(); }
fpgen::Model load_fpgen_model(const fs::path &data_dir, const FpgenModelPin &pin) {
    if (const auto files = embedded_model(pin)) return fpgen::Model::load(*files, pin.file_sha256);
    return fpgen::Model::load(fpgen_model_directory(data_dir, pin), pin.file_sha256);
}
namespace {
// Written last into a Camoufox directory setup installed.
constexpr std::string_view camoufox_marker = ".ais2api-runtime.json";
// The font groups a Linux identity reads: fonts/groups.json names them for
// "lin", and the launch's fontconfig scans their directories.
constexpr std::array<std::string_view, 4> linux_font_groups{"L", "LM", "LMW", "LW"};
// What camoufox_keeps leaves out of an install for a platform, and keeps from
// what it leaves out, as recorded in the marker.
struct CamoufoxRules {
    std::vector<std::string> skipped, kept;
};
CamoufoxRules camoufox_rules(std::string_view platform) {
    // Windows renders with the system's fonts. Camoufox still activates the
    // emoji font Firefox bundles on Windows, and so a page finds it.
    if (platform == "windows-x64") return {{"fonts", "addons/UBO"}, {"fonts/TwemojiMozilla.ttf"}};
    // Linux renders with the bundled fonts a Linux identity reads, through
    // Camoufox's Linux fontconfig.
    CamoufoxRules rules{{"fonts", "fontconfig", "addons/UBO"},
                        {"fonts/groups.json", "fonts/TwemojiMozilla.ttf", "fontconfig/linux"}};
    for (const auto group : linux_font_groups) rules.kept.push_back("fonts/" + std::string(group));
    return rules;
}
// The marker's record of the rules. An install made under other rules is
// installed again.
json::object camoufox_trim(std::string_view platform) {
    const auto rules = camoufox_rules(platform);
    json::array skipped, kept;
    for (const auto &entry : rules.skipped) skipped.emplace_back(entry);
    for (const auto &entry : rules.kept) kept.emplace_back(entry);
    return {{"skipped", std::move(skipped)}, {"kept", std::move(kept)}};
}
// `relative` is `entry` or below it.
bool within(std::string_view relative, std::string_view entry) {
    return relative.starts_with(entry) && (relative.size() == entry.size() || relative[entry.size()] == '/');
}
// Camoufox's Linux fontconfig scans the font groups setup keeps: an archive
// that groups its fonts otherwise would render with a part of them, or with
// another OS's.
void check_font_groups(const fs::path &directory) {
    std::vector<std::string> read;
    try {
        const auto groups = json::parse(read_file(directory / "fonts" / "groups.json", 1 << 20)).as_object();
        for (const auto &group : groups.at("readBy").as_object().at("lin").as_array())
            read.emplace_back(group.as_string());
    } catch (const std::exception &) {
        throw Error(502, "camoufox_layout_changed", "Camoufox archive holds no readable fonts/groups.json");
    }
    std::sort(read.begin(), read.end());
    if (!std::equal(read.begin(), read.end(), linux_font_groups.begin(), linux_font_groups.end()))
        throw Error(502, "camoufox_layout_changed",
                    "Camoufox groups its Linux fonts differently from what setup extracts; update setup's rules");
    for (const auto group : linux_font_groups)
        if (!fs::is_directory(fs::symlink_status(directory / "fonts" / fs::u8path(std::string(group)))))
            throw Error(502, "camoufox_layout_changed", "Camoufox archive holds no fonts/" + std::string(group));
    const auto conf = directory / "fontconfig" / "linux" / "fonts.conf";
    if (!fs::is_regular_file(fs::symlink_status(conf)))
        throw Error(502, "camoufox_layout_changed", "Camoufox archive holds no fontconfig/linux/fonts.conf");
    // Throws when the file no longer reads as the launch rewrites it.
    browser_detail::camoufox_fontconfig(read_file(conf, 16 << 20), {directory / "fonts" / "L"}, directory / "cache");
}
std::string host_platform() {
    try {
        return platform();
    } catch (const std::exception &) {
        return {};
    }
}
// "installed" when `directory` holds a complete install setup made; its
// marker is then in `owner`.
std::string camoufox_install_state(const fs::path &directory, json::object &owner) {
    std::error_code error;
    const auto status = fs::symlink_status(directory, error);
    if (!fs::exists(status)) return "missing";
    if (!fs::is_directory(status)) return "foreign";
    try {
        owner = json::parse(read_file(directory / camoufox_marker, 65536)).as_object();
    } catch (const std::exception &) {
        return "foreign";
    }
    if (string_or(owner, "browser") != "camoufox") return "foreign";
    const auto file = [&](std::string_view name) {
        return fs::is_regular_file(fs::symlink_status(directory / name, error));
    };
    if (!file(camoufox_program(string_or(owner, "platform"))) || !file("properties.json") || !file("application.ini"))
        return "damaged";
    try {
        if (firefox_identity::browser_version(directory) != string_or(owner, "browser_version")) return "damaged";
    } catch (const std::exception &) {
        return "damaged";
    }
    return "installed";
}
fs::path selection_file(const fs::path &data_dir) { return camoufox_root(data_dir) / "current.json"; }
void write_selection(const fs::path &data_dir, const CamoufoxSelection &selection) {
    secure_directory(camoufox_root(data_dir));
    atomic_write(selection_file(data_dir),
                 json::serialize(json::object{{"version", 1},
                                              {"current", selection.current},
                                              {"previous", selection.previous},
                                              {"held", selection.held}}));
}
} // namespace
std::string runtime_platform() { return host_platform(); }
fs::path termux_container_rootfs() {
    const auto prefix = termux_prefix();
    if (!prefix) return {};
    const auto containers = fs::path(prefix) / "var" / "lib" / "proot-distro";
    const std::string name(termux_container);
    // proot-distro moves a container of the older layout at its next login.
    for (const auto &rootfs : {containers / "containers" / name / "rootfs", containers / "installed-rootfs" / name}) {
        std::error_code error;
        if (fs::is_directory(rootfs, error)) return rootfs;
    }
    return {};
}
std::vector<std::string> missing_packages(std::string_view dpkg_status, const std::vector<std::string> &packages) {
    std::set<std::string> installed;
    std::string package;
    bool present = false;
    const auto finish = [&] {
        if (present && !package.empty()) installed.insert(package);
        package.clear();
        present = false;
    };
    std::istringstream input{std::string(dpkg_status)};
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            finish();
        } else if (line.starts_with("Package:")) {
            package = line.substr(8);
            package.erase(0, package.find_first_not_of(" \t"));
            package.erase(package.find_last_not_of(" \t") + 1);
        } else if (line.starts_with("Status:")) {
            // "install ok installed": its last word is the package's state.
            const auto end = line.find_last_not_of(" \t");
            present = end != std::string::npos && line.substr(0, end + 1).ends_with(" installed");
        }
    }
    finish();
    std::vector<std::string> missing;
    for (const auto &name : packages)
        if (!installed.contains(name)) missing.push_back(name);
    return missing;
}
std::size_t remove_profile_leftovers(const fs::path &data_dir) {
    std::size_t removed = 0;
    std::error_code error;
    for (fs::directory_iterator it(data_dir / "runtime", error), end; !error && it != end; it.increment(error)) {
        // The gateways name each launch's profile "<browser>-" + random_hex(12).
        const auto name = path_utf8(it->path().filename());
        const auto dash = name.find('-');
        if (dash == std::string::npos) continue;
        const auto prefix = name.substr(0, dash), suffix = name.substr(dash + 1);
        std::error_code ignored;
        if ((prefix != "browser" && prefix != "camoufox") || suffix.size() != 24 ||
            suffix.find_first_not_of("0123456789abcdef") != std::string::npos ||
            it->is_symlink(ignored) || !it->is_directory(ignored))
            continue;
        if (fs::remove_all(it->path(), ignored) != static_cast<std::uintmax_t>(-1) && !ignored) ++removed;
    }
    return removed;
}
std::optional<CamoufoxPin> camoufox_pin(const json::object &manifest, std::string_view platform) {
    const auto *camoufox = manifest.if_contains("camoufox");
    if (!camoufox) return std::nullopt;
    CamoufoxPin pin;
    try {
        const auto &object = camoufox->as_object();
        const auto *artifact = object.at("artifacts").as_object().if_contains(platform);
        if (!artifact) return std::nullopt;
        pin.version = json::value_to<std::string>(object.at("version"));
        pin.platform = std::string(platform);
        pin.url = json::value_to<std::string>(artifact->at("url"));
        pin.sha256 = json::value_to<std::string>(artifact->at("sha256"));
        pin.size = json::value_to<std::uint64_t>(artifact->at("size"));
    } catch (const std::exception &) {
        pin = {};
    }
    if (!model_name(pin.version, false) || !model_name(pin.platform, false) || !pin.url.starts_with("https://") ||
        !digest(pin.sha256) || !pin.size)
        throw Error(400, "invalid_manifest",
                    "runtime-manifest.json has no valid Camoufox pin for " + std::string(platform));
    return pin;
}
std::optional<CamoufoxPin> camoufox_pin() {
    const auto host = host_platform();
    if (host.empty()) return std::nullopt;
    return camoufox_pin(manifest(), host);
}
std::string_view camoufox_program(std::string_view platform) {
    return platform == "windows-x64" ? "camoufox.exe" : "camoufox-bin";
}
bool camoufox_keeps(std::string_view platform, std::string_view relative) {
    const auto rules = camoufox_rules(platform);
    const auto under = [&](const std::string &entry) { return within(relative, entry); };
    return std::any_of(rules.kept.begin(), rules.kept.end(), under) ||
           std::none_of(rules.skipped.begin(), rules.skipped.end(), under);
}
fs::path camoufox_root(const fs::path &data_dir) { return data_dir / "runtime" / "camoufox"; }
fs::path camoufox_directory(const fs::path &data_dir, const CamoufoxPin &pin) {
    return camoufox_root(data_dir) / fs::u8path(pin.version + "-" + pin.platform);
}
std::string camoufox_state(const fs::path &directory, const CamoufoxPin &pin) {
    json::object owner;
    const auto state = camoufox_install_state(directory, owner);
    const auto trim = camoufox_trim(pin.platform);
    if (state == "installed" &&
        (string_or(owner, "browser_version") != pin.version || string_or(owner, "platform") != pin.platform ||
         string_or(owner, "sha256") != pin.sha256 || owner.if_contains("skipped") == nullptr ||
         owner.at("skipped") != trim.at("skipped") || owner.if_contains("kept") == nullptr ||
         owner.at("kept") != trim.at("kept")))
        return "damaged";
    return state;
}
void install_camoufox(const fs::path &archive, const fs::path &directory, const CamoufoxPin &pin) {
    const auto state = camoufox_state(directory, pin);
    if (state == "foreign")
        throw Error(409, "unknown_runtime",
                    path_utf8(directory) + " was not created by setup; move it away and rerun setup");
    const auto parent = directory.parent_path();
    secure_directory(parent);
    const auto staging = parent / ("staging-" + random_hex(8));
    try {
        extract_archive(archive, staging,
                        [&](std::string_view relative) { return camoufox_keeps(pin.platform, relative); });
        for (const auto name : {camoufox_program(pin.platform), std::string_view("properties.json"),
                                std::string_view("application.ini")})
            if (!fs::is_regular_file(fs::symlink_status(staging / name)))
                throw Error(502, "archive_layout", "Camoufox archive holds no " + std::string(name) + " at its root");
        if (firefox_identity::browser_version(staging) != pin.version)
            throw Error(502, "archive_layout", "Camoufox archive is not version " + pin.version);
        if (pin.platform != "windows-x64") check_font_groups(staging);
        auto marker = camoufox_trim(pin.platform);
        marker.insert({{"version", 1},
                       {"browser", "camoufox"},
                       {"browser_version", pin.version},
                       {"platform", pin.platform},
                       {"sha256", pin.sha256}});
        atomic_write(staging / camoufox_marker, json::serialize(marker));
        if (state != "missing") fs::remove_all(directory);
        fs::rename(staging, directory);
    } catch (...) {
        std::error_code error;
        fs::remove_all(staging, error);
        throw;
    }
}
CamoufoxSelection camoufox_selection(const fs::path &data_dir) {
    CamoufoxSelection selection;
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(selection_file(data_dir), error))) return selection;
    try {
        const auto object = json::parse(read_file(selection_file(data_dir), 65536)).as_object();
        selection.current = string_or(object, "current");
        selection.previous = string_or(object, "previous");
        selection.held = bool_or(object, "held");
    } catch (const std::exception &) {
        return {};
    }
    // Directory names under camoufox_root, nothing else.
    if (!model_name(selection.current, false)) selection.current.clear();
    if (!model_name(selection.previous, false)) selection.previous.clear();
    return selection;
}
void select_camoufox(const fs::path &data_dir, const std::string &name) {
    if (!model_name(name, false)) throw Error(400, "invalid_runtime", "Invalid Camoufox install name");
    auto selection = camoufox_selection(data_dir);
    if (selection.current != name) {
        if (!selection.current.empty()) selection.previous = selection.current;
        selection.current = name;
    }
    if (selection.previous == name) selection.previous.clear();
    selection.held = false;
    write_selection(data_dir, selection);
    // Two installs are kept. An install still in use elsewhere may resist
    // removal; the next setup tries again.
    for (const auto &entry : fs::directory_iterator(camoufox_root(data_dir))) {
        const auto file = path_utf8(entry.path().filename());
        if (file == selection.current || file == selection.previous) continue;
        json::object owner;
        if (const auto state = camoufox_install_state(entry.path(), owner); state == "installed" || state == "damaged") {
            std::error_code error;
            fs::remove_all(entry.path(), error);
        }
    }
}
void hold_previous_camoufox(const fs::path &data_dir) {
    auto selection = camoufox_selection(data_dir);
    if (selection.previous.empty())
        throw Error(409, "rollback_missing", "There is no earlier Camoufox install to roll back to");
    std::swap(selection.current, selection.previous);
    selection.held = true;
    write_selection(data_dir, selection);
}
fs::path camoufox_bundle_directory(const fs::path &program_directory, const CamoufoxPin &pin) {
    return program_directory / "camoufox-runtime" / fs::u8path(pin.version + "-" + pin.platform);
}
CamoufoxExecutable camoufox_executable(const Config &config, const fs::path &program_directory) {
    auto stem = path_utf8(config.browser_executable.stem());
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (stem == "camoufox" || stem == "camoufox-bin") return {config.browser_executable, "browser_executable"};
    // A bundle ships with the program, so it is the pinned build; one that is
    // not, or is damaged, is passed over.
    std::optional<CamoufoxPin> pin;
    try {
        pin = camoufox_pin();
    } catch (const std::exception &) {
    }
    if (pin) {
        const auto bundle = camoufox_bundle_directory(program_directory, *pin);
        if (camoufox_state(bundle, *pin) == "installed") return {bundle / camoufox_program(pin->platform), "bundle"};
    }
    const auto selection = camoufox_selection(config.data_dir);
    if (selection.current.empty()) return {};
    const auto directory = camoufox_root(config.data_dir) / fs::u8path(selection.current);
    json::object owner;
    if (camoufox_install_state(directory, owner) != "installed") return {};
    return {directory / camoufox_program(string_or(owner, "platform")), "setup"};
}
namespace {
json::object verify_camoufox(const Config &config, const fs::path &executable) {
    auto verification = config;
    verification.browser_executable = executable;
    verification.headless = true;
    Store store(config.data_dir);
    CamoufoxGateway gateway(verification, store);
    return gateway.verify_runtime();
}
// The image setup creates the Termux container from, and its os-release.
constexpr std::string_view container_image = "ubuntu:24.04";
constexpr std::string_view container_os_release = "ID=ubuntu\nVERSION_ID=\"24.04\"\n";
// Creates the Termux container unless it exists, recording in
// runtime/proot-owned.json that setup did; one setup did not create is left
// alone. Returns its rootfs.
fs::path prepare_termux_container(const fs::path &runtime) {
    checked_process({"pkg", "install", "-y", "proot-distro"});
    const auto created = runtime / "proot-owned.json";
    if (termux_container_rootfs().empty()) {
        atomic_write(created, "{\"version\":1,\"phase\":\"creating\"}", true);
        checked_process({"proot-distro", "install", std::string(container_image), "--name", std::string(termux_container)});
    } else if (!fs::exists(created))
        throw Error(409, "unknown_container",
                    "Existing " + std::string(termux_container) + " container has no ownership marker");
    auto rootfs = termux_container_rootfs();
    if (rootfs.empty())
        throw Error(502, "system_prepare", "proot-distro created no " + std::string(termux_container) + " container");
    return rootfs;
}
void install_container_packages(const std::vector<std::string> &packages) {
    const std::vector<std::string> login{"proot-distro", "login", std::string(termux_container), "--"};
    auto command = login;
    command.insert(command.end(), {"apt-get", "update"});
    checked_process(command);
    command = login;
    command.insert(command.end(), {"apt-get", "install", "-y"});
    command.insert(command.end(), packages.begin(), packages.end());
    checked_process(command);
}
void mark_termux_container_ready(const fs::path &runtime) {
    atomic_write(runtime / "proot-owned.json", "{\"version\":1,\"phase\":\"ready\"}", true);
}
// Those of `packages` the container's dpkg does not record as installed.
std::vector<std::string> missing_container_packages(const fs::path &rootfs, const std::vector<std::string> &packages) {
    const auto status = rootfs / "var" / "lib" / "dpkg" / "status";
    std::error_code error;
    return missing_packages(fs::is_regular_file(status, error) ? read_file(status, 64ull << 20) : std::string(), packages);
}
// On Termux Camoufox runs in the container: setup creates it and installs
// the packages Camoufox needs there. Nothing is asked or run when it holds
// them already.
json::object prepare_camoufox_container(const fs::path &runtime, bool approved) {
    const auto packages = camoufox_linux_packages(container_os_release);
    auto rootfs = termux_container_rootfs();
    if (!rootfs.empty() && !fs::exists(runtime / "proot-owned.json"))
        throw Error(409, "unknown_container",
                    "Existing " + std::string(termux_container) + " container has no ownership marker");
    auto missing = rootfs.empty() ? packages : missing_container_packages(rootfs, packages);
    std::error_code error;
    const auto prefix = termux_prefix();
    const bool proot = prefix && fs::is_regular_file(fs::path(prefix) / "bin" / "proot-distro", error);
    if (proot && !rootfs.empty() && missing.empty()) return {{"status", "ready"}, {"container", termux_container}};
    std::string names;
    for (const auto &package : missing) names += (names.empty() ? "" : " ") + package;
    if (!approved)
        throw Error(409, "system_confirmation_required",
                    (rootfs.empty() ? "Camoufox runs in the " + std::string(termux_container) +
                                          " container, which setup creates with proot-distro from " +
                                          std::string(container_image) + ", with these packages: "
                                    : "Camoufox needs these packages in the " + std::string(termux_container) +
                                          " container: ") +
                        (names.empty() ? "none (proot-distro is missing)" : names) +
                        ". Review the setup documentation and run setup --yes to approve package-manager changes");
    console::emit_record({{"event", "setup_stage"}, {"stage", "system-packages"}, {"container", termux_container}, {"packages", names}});
    rootfs = prepare_termux_container(runtime);
    missing = missing_container_packages(rootfs, packages);
    if (!missing.empty()) install_container_packages(missing);
    mark_termux_container_ready(runtime);
    json::array installed;
    for (const auto &package : missing) installed.emplace_back(package);
    return {{"status", "installed"}, {"container", termux_container}, {"installed", std::move(installed)}};
}
// Whether dpkg records `package` as installed; it also knows removed ones.
bool package_installed(const std::string &package) {
    return run_process({"sh", "-c", "dpkg-query -W -f='${db:Status-Abbrev}' \"$1\" 2>/dev/null | grep -q '^ii'",
                        "sh", package}) == 0;
}
// Installs the packages Camoufox needs that this Ubuntu or Debian lacks.
// Nothing is asked or run when none is missing, as in the Docker image.
json::object prepare_camoufox_system(const fs::path &runtime, bool approved) {
    const auto system = platform();
    if (system == "termux-arm64") return prepare_camoufox_container(runtime, approved);
    if (system != "linux-x64" && system != "linux-arm64") return {};
    std::vector<std::string> packages;
    try {
        packages = camoufox_linux_packages(read_file("/etc/os-release", 65536));
    } catch (const std::exception &) {
        // Elsewhere the packages are the user's to install; verification
        // tells what is missing.
        return {{"status", "not_checked"}, {"reason", "unsupported_distribution"}};
    }
    std::vector<std::string> missing;
    for (const auto &package : packages)
        if (!package_installed(package)) missing.push_back(package);
    if (missing.empty()) return {{"status", "ready"}};
    std::string names;
    for (const auto &package : missing) names += (names.empty() ? "" : " ") + package;
    if (!approved)
        throw Error(409, "system_confirmation_required",
                    "Camoufox needs these system packages: " + names +
                        ". Review the setup documentation and run setup --yes to approve package-manager changes");
    console::emit_record({{"event", "setup_stage"}, {"stage", "system-packages"}, {"packages", names}});
    std::vector<std::string> prefix;
#ifndef _WIN32
    if (geteuid() != 0) prefix = {"sudo", "--"};
#endif
    auto command = prefix;
    command.insert(command.end(), {"apt-get", "update"});
    checked_process(command);
    command = prefix;
    command.insert(command.end(), {"apt-get", "install", "-y"});
    command.insert(command.end(), missing.begin(), missing.end());
    checked_process(command);
    json::array installed;
    for (const auto &package : missing) installed.emplace_back(package);
    return {{"status", "installed"}, {"installed", std::move(installed)}};
}
// The Camoufox backend needs the fpgen model and Camoufox: the pinned build
// for this platform, unless browser_executable names one, and on Linux the
// system packages it runs with. setup changes neither browser_executable nor
// anything CloakBrowser uses.
json::object setup_camoufox(const Config &config, bool approved) {
    const auto model = fpgen_model_pin();
    const auto camoufox = camoufox_pin();
    const auto root = config.data_dir / "runtime";
    secure_directory(config.data_dir);
    secure_directory(root);
    const auto directory = fpgen_model_directory(config.data_dir, model);
    const auto stage = [&](std::string name, bool browser) {
        json::object record{{"event", "setup_stage"}, {"stage", name}, {"browser_backend", config.browser_backend}};
        if (!browser)
            record["fpgen_model"] = model.tag;
        else if (camoufox) {
            record["browser"] = "camoufox";
            record["browser_version"] = camoufox->version;
            record["platform"] = camoufox->platform;
        }
        console::emit_record(record);
    };
    json::object model_record{{"tag", model.tag}, {"source", "embedded"}};
    if (!fpgen_model_embedded(model)) {
        const auto state = fpgen_model_state(directory, model);
        if (state == "foreign")
            throw Error(409, "unknown_runtime",
                        path_utf8(directory) + " was not created by setup; move it away and rerun setup");
        if (state != "installed") {
            stage("model-download", false);
            const auto archive = root / "downloads" / "fpgen-model.zip";
            download_verified(model.url, archive, model.sha256, config.proxy, model.size);
            stage("model-install", false);
            install_fpgen_model(archive, directory, model);
        }
        model_record = {{"tag", model.tag},
                        {"source", "setup"},
                        {"directory", path_utf8(directory)},
                        {"installed_now", state != "installed"}};
    }
    stage("model-complete", false);
    json::object result{{"status", "prepared"},
                        {"browser_backend", config.browser_backend},
                        {"fpgen_model", std::move(model_record)},
                        {"note", "Run accounts probe and an actual generation to verify Google access"}};
    if (auto system = prepare_camoufox_system(root, approved); !system.empty())
        result["system_packages"] = std::move(system);

    // Neither browser_executable nor a bundle is installed; both are verified.
    if (const auto configured = camoufox_executable(config);
        configured.source == "browser_executable" || configured.source == "bundle") {
        stage("verify", true);
        auto verification = verify_camoufox(config, configured.path);
        stage("complete", true);
        result["camoufox"] = json::object{{"source", configured.source},
                                          {"executable", path_utf8(configured.path)},
                                          {"browser_verification", std::move(verification)}};
        return result;
    }
    if (!camoufox) {
        result["note"] = "No Camoufox is pinned for this platform yet: set browser_executable to a Camoufox build";
        return result;
    }
    const auto install = camoufox_directory(config.data_dir, *camoufox);
    const auto installed = camoufox_state(install, *camoufox);
    if (installed == "foreign")
        throw Error(409, "unknown_runtime",
                    path_utf8(install) + " was not created by setup; move it away and rerun setup");
    if (installed != "installed") {
        const auto archive =
            root / "downloads" / fs::u8path("camoufox-" + camoufox->version + "-" + camoufox->platform + ".zip");
        std::error_code error;
        const bool downloaded = fs::is_regular_file(archive, error) && fs::file_size(archive, error) == camoufox->size;
        // The four download segments are joined into a second copy.
        if (fs::space(root).available < (downloaded ? 0 : 2 * camoufox->size) + (512ull << 20))
            throw Error(507, "disk_space",
                        "Installing Camoufox needs about " + std::to_string(((downloaded ? 0 : 2 * camoufox->size) >> 20) + 512) +
                            " MiB of free space in the data directory");
        stage("download", true);
        download_verified(camoufox->url, archive, camoufox->sha256, config.proxy, camoufox->size);
        stage("extract", true);
        install_camoufox(archive, install, *camoufox);
        fs::remove(archive, error);
    }
    stage("verify", true);
    const auto executable = install / camoufox_program(camoufox->platform);
    auto verification = verify_camoufox(config, executable);
    select_camoufox(config.data_dir, path_utf8(install.filename()));
    stage("complete", true);
    result["camoufox"] = json::object{{"source", "setup"},
                                      {"version", camoufox->version},
                                      {"directory", path_utf8(install)},
                                      {"executable", path_utf8(executable)},
                                      {"installed_now", installed != "installed"},
                                      {"browser_verification", std::move(verification)}};
    return result;
}
} // namespace
namespace {
// Ubuntu 24.04 or Debian 13, whose package names setup knows.
void check_distribution(std::string_view os_release) {
    // Read os-release as data: never source distribution metadata in a shell.
    std::istringstream input{std::string(os_release)};
    std::string id, version, line;
    while (std::getline(input, line)) {
        auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos || line[begin] == '#')
            continue;
        auto equals = line.find('=', begin);
        if (equals == std::string::npos)
            continue;
        auto key = line.substr(begin, equals - begin);
        if (key != "ID" && key != "VERSION_ID")
            continue;
        auto value = line.substr(equals + 1);
        auto end = value.find_last_not_of(" \t\r");
        value.resize(end == std::string::npos ? 0 : end + 1);
        if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') &&
            value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        (key == "ID" ? id : version) = value;
    }
    if (!((id == "ubuntu" && version == "24.04") || (id == "debian" && version == "13")))
        throw Error(400, "unsupported_distribution",
                    "Automatic Linux setup supports Ubuntu 24.04 and Debian 13; "
                    "manually configure browser_executable on other distributions");
}
} // namespace
std::vector<std::string> linux_runtime_packages(std::string_view os_release) {
    check_distribution(os_release);
    // Ubuntu 24.04 and Debian 13 both use the t64 library package names.
    return {"libnss3", "libnspr4", "libatk1.0-0t64", "libatk-bridge2.0-0t64",
            "libcups2t64", "libdrm2", "libxkbcommon0", "libxcomposite1", "libxdamage1",
            "libxfixes3", "libxrandr2", "libgbm1", "libasound2t64", "libgtk-3-0t64",
            "fonts-noto-cjk", "fonts-noto-color-emoji", "ca-certificates"};
}
std::vector<std::string> camoufox_linux_packages(std::string_view os_release) {
    check_distribution(os_release);
    // Xvfb is the display a headless launch runs on, with Mesa's software
    // OpenGL for WebGL; Camoufox brings its fonts.
    return {"xvfb",           "libgtk-3-0t64", "libx11-xcb1", "libasound2t64", "libdbus-1-3",
            "libgl1-mesa-dri", "libegl1",       "fontconfig",  "ca-certificates"};
}
namespace {
json::object setup_cloakbrowser(Config &config, bool approved) {
    auto m = manifest();
    auto system = platform();
    if (!m.at("artifacts").as_object().contains(system))
        throw Error(400, "unsupported_platform",
                    "No CloakBrowser is pinned for " + system + "; use the camoufox backend");
    auto version = string_or(m, "browser_version");
    auto artifact = m.at("artifacts").as_object().at(system).as_object();
    auto root = config.data_dir / "runtime";
    secure_directory(config.data_dir);
    secure_directory(root);
    auto journal = root / "setup.json";
    auto stage = [&](std::string name) {
        console::emit_record({{"event", "setup_stage"},
                              {"stage", name},
                              {"browser_version", version},
                              {"platform", system}});
        atomic_write(journal,
                     json::serialize(json::object{{"version", 1},
                                                  {"stage", name},
                                                  {"browser_version", version},
                                                  {"platform", system}}),
                     true);
    };
    auto prepared = [&](const fs::path &launcher, bool bundled) -> json::object {
        stage("verify");
        auto browser_verification = verify_browser(config, launcher);
        if (!config.browser_executable.empty() && config.browser_executable != launcher)
            atomic_write(root / "previous.json",
                         json::serialize(json::object{{"browser_executable",
                             config.to_json(true).at("browser_executable")}}), true);
        auto previous = config.browser_executable;
        config.browser_executable = launcher;
        try {
            atomic_write(config.config_file, json::serialize(config.to_json(true)), true);
        } catch (...) {
            config.browser_executable = previous;
            throw;
        }
        stage("complete");
        return {{"status", "prepared"}, {"browser_version", version},
                {"browser_executable", path_utf8(launcher)}, {"bundled", bundled},
                {"verified", "local_cdp_and_isolated_context"},
                {"browser_verification", browser_verification},
                {"note", "Run accounts probe and an actual generation to verify Google access"}};
    };
    stage("system-check");
    if (system != "windows-x64" && !approved)
        throw Error(409, "system_confirmation_required",
                    "System packages may be installed. Review the setup documentation and run "
                    "setup --yes to approve package-manager changes");
    if (system == "linux-x64") {
        auto packages = linux_runtime_packages(read_file("/etc/os-release", 65536));
        std::vector<std::string> prefix;
#ifndef _WIN32
        if (geteuid() != 0)
            prefix = {"sudo", "--"};
#endif
        auto command = prefix;
        command.insert(command.end(), {"apt-get", "update"});
        checked_process(command);
        command = prefix;
        command.insert(command.end(), {"apt-get", "install", "-y"});
        command.insert(command.end(), packages.begin(), packages.end());
        checked_process(command);
    } else if (system == "termux-arm64") {
        prepare_termux_container(root);
        install_container_packages(linux_runtime_packages(container_os_release));
        mark_termux_container_ready(root);
    }
    if (auto bundle = bundled_browser(Config::default_directory()); !bundle.empty()) {
        stage("bundle-check");
        verify_browser_bundle(bundle);
        return prepared(bundle, true);
    }
    auto target = root / ("chromium-" + version + "-" + system);
    auto executable = target / (system == "windows-x64" ? "chrome.exe" : "chrome");
    if (!fs::is_regular_file(executable)) {
        if (fs::exists(target))
            throw Error(409, "incomplete_runtime",
                        "An incomplete final runtime directory exists; preserve it and rerun with "
                        "a clean version directory");
        if (fs::space(root).available < 2ull * 1024 * 1024 * 1024)
            throw Error(507, "disk_space",
                        "At least two GiB of free space is required for browser staging");
        stage("download");
        auto cache =
            root / "downloads" / (system == "windows-x64" ? "browser.zip" : "browser.tar.gz");
        download_verified(string_or(artifact, "url"), cache, string_or(artifact, "sha256"),
                          config.proxy, json::value_to<std::uint64_t>(artifact.at("size")));
        stage("extract");
        auto staging = root / ("staging-" + random_hex(8));
        try {
            extract_archive(cache, staging);
            auto binary = staging / executable.filename();
            if (!fs::is_regular_file(binary))
                throw Error(502, "archive_layout",
                            "Runtime archive does not contain the expected executable");
            atomic_write(staging / ".ais2api-runtime.json",
                         json::serialize(json::object{{"browser_version", version},
                                                      {"sha256", string_or(artifact, "sha256")}}),
                         true);
            fs::rename(staging, target);
        } catch (...) {
            std::error_code ec;
            fs::remove_all(staging, ec);
            throw;
        }
    }
    auto ownership = target / ".ais2api-runtime.json";
    if (!fs::is_regular_file(ownership))
        throw Error(409, "unknown_runtime",
                    "Existing browser directory has no project ownership marker");
    auto owner = json::parse(read_file(ownership, 65536)).as_object();
    if (string_or(owner, "browser_version") != version ||
        string_or(owner, "sha256") != string_or(artifact, "sha256"))
        throw Error(409, "unknown_runtime",
                    "Existing browser directory does not match the locked artifact");
    fs::path launcher = executable;
    if (system == "termux-arm64") {
        auto prefix = std::getenv("PREFIX");
        if (!prefix)
            throw Error(400, "environment", "Missing Termux PREFIX");
        launcher = target / "run-browser.sh";
        auto target_string = fs::absolute(target).string(),
             data = fs::absolute(config.data_dir).string();
        auto text = "#!" + std::string(prefix) + "/bin/sh\nexec proot-distro login --bind " +
                    sh_quote(target_string + ":" + target_string) + " --bind " +
                    sh_quote(data + ":" + data) + " " + std::string(termux_container) + " -- " +
                    sh_quote(fs::absolute(executable).string()) + " \"$@\"\n";
        atomic_write(launcher, text, true);
#ifndef _WIN32
        fs::permissions(launcher, fs::perms::owner_all);
#endif
    }
    return prepared(launcher, false);
}
} // namespace
json::object setup_runtime(Config &config, bool approved, std::string_view backend) {
    const auto accounts = stored_backends(config.data_dir);
    const auto backends = backend.empty() ? backends_needed(config.browser_backend, accounts)
                                          : std::vector<std::string>{std::string(backend)};
    // The first backend's result is the answer; the others nest under their names.
    json::object result;
    json::array prepared;
    for (const auto &name : backends) {
        auto next = name == "camoufox" ? setup_camoufox(config, approved) : setup_cloakbrowser(config, approved);
        if (result.empty()) result = std::move(next);
        else result[name] = std::move(next);
        prepared.emplace_back(name);
    }
    result["backends"] = std::move(prepared);
    // CloakBrowser prepared on request alone is deprecated all the same.
    auto deprecations = backend_deprecations(config.browser_backend, accounts);
    if (deprecations.empty() && backend == "cloakbrowser")
        deprecations.push_back(json::object{{"code", "cloakbrowser_backend"}, {"default_backend", false}, {"accounts", false}});
    result["deprecations"] = std::move(deprecations);
    return result;
}
namespace {
json::object rollback_camoufox(const Config &config, const fs::path &program_directory) {
    if (camoufox_executable(config, program_directory).source == "bundle")
        throw Error(409, "bundled_runtime",
                    "This build runs the Camoufox bundled with it: use the previous package, or set "
                    "browser_executable to another Camoufox");
    const auto selection = camoufox_selection(config.data_dir);
    if (selection.previous.empty())
        throw Error(409, "rollback_missing", "There is no earlier Camoufox install to roll back to");
    const auto directory = camoufox_root(config.data_dir) / fs::u8path(selection.previous);
    json::object owner;
    if (camoufox_install_state(directory, owner) != "installed")
        throw Error(409, "rollback_missing", "The earlier Camoufox install no longer exists");
    const auto executable = directory / camoufox_program(string_or(owner, "platform"));
    auto verification = verify_camoufox(config, executable);
    hold_previous_camoufox(config.data_dir);
    json::object result{{"status", "rolled_back"},
                        {"browser_backend", config.browser_backend},
                        {"browser_version", string_or(owner, "browser_version")},
                        {"browser_executable", path_utf8(executable)},
                        {"browser_verification", std::move(verification)},
                        {"note", "Kept until browser update installs the pinned Camoufox again"}};
    if (camoufox_executable(config, program_directory).source == "browser_executable")
        result["note"] = "browser_executable names Camoufox and stays in use";
    return result;
}
} // namespace
json::object rollback_runtime(Config &config, const fs::path &program_directory, std::string_view backend) {
    if ((backend.empty() ? effective_backend({}, config.browser_backend) : backend) == "camoufox")
        return rollback_camoufox(config, program_directory);
    auto file = config.data_dir / "runtime/previous.json";
    auto previous = json::parse(read_file(file, 65536)).as_object();
    auto path = fs::u8path(string_or(previous, "browser_executable"));
    if (path.is_relative())
        path = (config.config_file.parent_path() / path).lexically_normal();
    if (!fs::is_regular_file(path))
        throw Error(409, "rollback_missing", "Previous browser executable no longer exists");
    auto browser_verification = verify_browser(config, path);
    auto current = config.browser_executable;
    auto stored_current = config.to_json(true).at("browser_executable");
    config.browser_executable = path;
    try {
        atomic_write(config.config_file, json::serialize(config.to_json(true)), true);
    } catch (...) {
        config.browser_executable = current;
        throw;
    }
    atomic_write(file, json::serialize(json::object{{"browser_executable", stored_current}}),
                 true);
    return {{"status", "rolled_back"},
            {"browser_executable", path_utf8(path)},
            {"browser_verification", browser_verification}};
}
json::object doctor(const Config &config, const fs::path &program_directory,
                    std::optional<std::vector<std::string>> stored) {
    const auto accounts = stored ? *stored : stored_backends(config.data_dir);
    const auto backends = backends_needed(config.browser_backend, accounts);
    const auto needs = [&](std::string_view backend) {
        return std::find(backends.begin(), backends.end(), backend) != backends.end();
    };
    json::array needed;
    for (const auto &backend : backends) needed.emplace_back(backend);
    json::object result{{"config_version", config.version},
            {"build_mode", build_mode()},
            {"program_directory", path_utf8(program_directory)},
            {"config_file", path_utf8(config.config_file)},
            {"download_directory", path_utf8(config.data_dir / "runtime/downloads")},
            {"service_log", path_utf8(config.data_dir / "service.log")},
            {"data_directory", path_utf8(config.data_dir)},
            {"manifest_available", !manifest().empty()},
            {"browser_backend", config.browser_backend},
            {"backends_needed", std::move(needed)},
            {"deprecations", backend_deprecations(config.browser_backend, accounts)},
            {"google_access", "not_checked"}};
    // Ready only when every backend the accounts need is.
    bool configured = true, exists = true;
    if (needs("cloakbrowser")) {
        configured = !config.browser_executable.empty();
        exists = fs::is_regular_file(config.browser_executable);
    }
    if (needs("camoufox")) {
        const auto model = fpgen_model_pin();
        const bool embedded = fpgen_model_embedded(model);
        const auto directory = fpgen_model_directory(config.data_dir, model);
        const auto model_state = embedded ? std::string("installed") : fpgen_model_state(directory, model);
        json::object fpgen{{"tag", model.tag}, {"source", embedded ? "embedded" : "setup"}, {"state", model_state}};
        if (!embedded) fpgen["directory"] = path_utf8(directory);
        result["fpgen_model"] = std::move(fpgen);
        const auto executable = camoufox_executable(config, program_directory);
        const auto selection = camoufox_selection(config.data_dir);
        const auto pin = camoufox_pin();
        json::object camoufox{{"source", executable.source},
                              {"executable", path_utf8(executable.path)},
                              {"current", selection.current},
                              {"previous", selection.previous},
                              {"held", selection.held},
                              {"pinned", pin ? json::value(pin->version) : json::value(nullptr)}};
        // A program upgrade that pins another Camoufox, or extracts it
        // differently, makes the launcher run setup again, unless a rollback
        // holds the current install.
        bool outdated = false;
        std::error_code error;
        if (pin) {
            const auto pinned = camoufox_directory(config.data_dir, *pin);
            const auto state = camoufox_state(pinned, *pin);
            camoufox["state"] = state;
            outdated = executable.source == "setup" &&
                       (selection.current != path_utf8(pinned.filename()) || state != "installed");
            if (const auto bundle = camoufox_bundle_directory(program_directory, *pin);
                fs::exists(fs::symlink_status(bundle, error)))
                camoufox["bundle"] = json::object{{"directory", path_utf8(bundle)},
                                                  {"state", camoufox_state(bundle, *pin)}};
        }
        camoufox["setup_needed"] = outdated;
        configured = configured && !executable.path.empty();
        exists = exists && !executable.path.empty() && fs::is_regular_file(executable.path, error) &&
                 model_state == "installed" && (!outdated || selection.held);
        result["camoufox"] = std::move(camoufox);
    }
    result["browser_configured"] = configured;
    result["browser_exists"] = exists;
    return result;
}
std::string media_detail::fetch_with_resolver(std::string_view input, std::size_t limit, std::string_view proxy,
                               std::stop_token cancel, const Resolver& resolve) {
    std::string url(input), proxy_text(proxy);
    for (unsigned redirect = 0; redirect < 4; ++redirect) {
        check_cancel(cancel);
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
        if (curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0))
            throw Error(400, "media_url", "Invalid image URL");
        auto part = [&](CURLUPart p) {
            char *text = nullptr;
            auto result = curl_url_get(parsed.get(), p, &text, 0);
            std::string value = result == CURLUE_OK ? text : "";
            curl_free(text);
            return value;
        };
        if (part(CURLUPART_SCHEME) != "https" || !part(CURLUPART_USER).empty() ||
            !part(CURLUPART_PASSWORD).empty() ||
            !part(CURLUPART_ZONEID).empty() ||
            (!part(CURLUPART_PORT).empty() && part(CURLUPART_PORT) != "443"))
            throw Error(400, "media_url",
                        "Only public HTTPS images without URL credentials are supported");
        const auto host = part(CURLUPART_HOST);
        auto resolution_host = host;
        if (resolution_host.starts_with('[') && resolution_host.ends_with(']'))
            resolution_host = resolution_host.substr(1, resolution_host.size() - 2);
        std::vector<boost::asio::ip::address> addresses;
        boost::system::error_code literal_error;
        auto literal = boost::asio::ip::make_address(resolution_host, literal_error);
        if (!literal_error) addresses.push_back(literal);
        else if (resolve) addresses = resolve(resolution_host);
        else {
            boost::asio::io_context io;
            boost::asio::ip::tcp::resolver resolver(io);
            boost::system::error_code ec;
            const auto results = resolver.resolve(resolution_host, "443", ec);
            if (ec) throw Error(400, "media_url", "Cannot resolve image host");
            for (const auto& result : results) addresses.push_back(result.endpoint().address());
        }
        check_cancel(cancel);
        if (addresses.empty()) throw Error(400, "media_url", "Cannot resolve image host");
        for (const auto& address : addresses)
            if (!public_address(address))
                throw Error(400, "media_url", "Private image destinations are not allowed");
        // Bound fallback work after validating every result, including those
        // outside the fallback budget. Never silently ignore a private result.
        std::vector<boost::asio::ip::address> destinations;
        for (const auto& address : addresses)
            if (std::find(destinations.begin(), destinations.end(), address) == destinations.end() && destinations.size() < 8)
                destinations.push_back(address);
        struct Buffer {
            std::string bytes;
            std::size_t max;
            bool exceeded = false;
        } data{{}, limit};
        auto c = curl_handle();
        curl_easy_setopt(c.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(
            c.get(), CURLOPT_WRITEFUNCTION, +[](char *b, size_t s, size_t n, void *p) -> size_t {
                auto &d = *static_cast<Buffer *>(p);
                auto length = s * n;
                if (length > d.max - d.bytes.size()) {
                    d.exceeded = true;
                    return 0;
                }
                d.bytes.append(b, length);
                return length;
            });
        curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(c.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c.get(), CURLOPT_XFERINFODATA, &cancel);
        curl_easy_setopt(
            c.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<std::stop_token *>(p)->stop_requested() ? 1 : 0;
            });
        // An empty explicit proxy disables environment proxies. With a
        // configured proxy, do not let NO_PROXY bypass the requested route.
        curl_easy_setopt(c.get(), CURLOPT_PROXY, proxy_text.c_str());
        curl_easy_setopt(c.get(), CURLOPT_NOPROXY, "");
        if (proxy_text.empty())
            curl_easy_setopt(c.get(), CURLOPT_OPENSOCKETFUNCTION, public_socket);
        CURLcode result = CURLE_COULDNT_CONNECT;
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        for (const auto& address : destinations) {
            check_cancel(cancel);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) break;
            curl_easy_setopt(c.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
            // CONNECT_TO changes only the connection target. HTTPS SNI,
            // certificate verification and HTTP Host keep the original name.
            auto numeric = address.to_string();
            if (address.is_v6()) numeric = "[" + numeric + "]";
            const auto mapping = host + ":443:" + numeric + ":443";
            std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> target(
                curl_slist_append(nullptr, mapping.c_str()), curl_slist_free_all);
            if (!target) throw Error(500, "media_fetch", "Cannot allocate image destination");
            if (curl_easy_setopt(c.get(), CURLOPT_CONNECT_TO, target.get()) != CURLE_OK)
                throw Error(500, "media_fetch", "Cannot pin image destination");
            data.bytes.clear();
            result = curl_easy_perform(c.get());
            curl_easy_setopt(c.get(), CURLOPT_CONNECT_TO, nullptr);
            if (data.exceeded || !data.bytes.empty() ||
                (result != CURLE_COULDNT_CONNECT && result != CURLE_RECV_ERROR &&
                 result != CURLE_SSL_CONNECT_ERROR)) break;
        }
        check_cancel(cancel);
        if (data.exceeded)
            throw Error(413, "media_limit", "Remote image exceeds size limit");
        if (result != CURLE_OK)
            throw Error(502, "media_fetch", "Unable to retrieve remote image");
        long status = 0;
        curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &status);
        if (status >= 300 && status < 400) {
            char *target = nullptr;
            curl_easy_getinfo(c.get(), CURLINFO_REDIRECT_URL, &target);
            if (!target)
                throw Error(502, "media_fetch", "Image redirect has no destination");
            url = target;
            continue;
        }
        if (status != 200)
            throw Error(502, "media_fetch", "Image server returned an error");
        char *type = nullptr;
        curl_easy_getinfo(c.get(), CURLINFO_CONTENT_TYPE, &type);
        std::string mime = type ? type : "";
        mime = mime.substr(0, mime.find(';'));
        if (!mime.starts_with("image/") || mime.size() > 64)
            throw Error(400, "media_type", "Remote resource is not an image");
        std::string b64(4 * ((data.bytes.size() + 2) / 3), '\0');
        auto n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(b64.data()),
                                 reinterpret_cast<const unsigned char *>(data.bytes.data()),
                                 static_cast<int>(data.bytes.size()));
        b64.resize(static_cast<std::size_t>(n));
        return "data:" + mime + ";base64," + b64;
    }
    throw Error(400, "media_redirect", "Too many image redirects");
}
std::string fetch_public_media(std::string_view url, std::size_t limit, std::string_view proxy,
                               std::stop_token cancel) {
    return media_detail::fetch_with_resolver(url, limit, proxy, cancel);
}
} // namespace ais2api
