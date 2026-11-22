// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/log_sources.hpp"
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <openssl/evp.h>
namespace ais2api {
namespace {
std::map<std::string, fs::path> sources(const fs::path& directory) {
    std::map<std::string, fs::path> result;
    auto add = [&](std::string id, const fs::path& path) {
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (!ec && !fs::is_symlink(status) && fs::is_regular_file(status))
            result.emplace(std::move(id), path);
    };
    add("service", directory / "service.log");
    add("management", directory / "management.log");
    const auto browsers = directory / "logs";
    std::error_code ec;
    const auto bstatus = fs::symlink_status(browsers, ec);
    if (!ec && !fs::is_symlink(bstatus) && fs::is_directory(bstatus))
        for (fs::directory_iterator it(browsers, ec), end; !ec && it != end; it.increment(ec)) {
            auto name = path_utf8(it->path().filename());
            if ((name.starts_with("browser-") || name.starts_with("camoufox-")) && name.ends_with(".log"))
                add(name, it->path());
        }
    return result;
}
}
json::object log_sources(const fs::path& directory) {
    json::array result;
    for (const auto& [id, path] : sources(directory)) {
        std::error_code ec;
        const auto bytes = fs::file_size(path, ec);
        if (!ec)
            result.push_back(json::object{{"id", id}, {"bytes", bytes}});
    }
    return {{"sources", std::move(result)}};
}
json::object read_log_source(const fs::path& directory, std::string_view source,
                             std::string_view cursor, std::size_t bytes) {
    if (!bytes || bytes > 49152)
        throw Error(400, "invalid_limit", "Log byte limit must be 1..49152");
    std::uint64_t offset = 0;
    const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
    if (cursor.empty() || parsed.ec != std::errc{} || parsed.ptr != cursor.data() + cursor.size() ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        throw Error(400, "invalid_cursor", "Log cursor must be a decimal byte offset");
    auto available = sources(directory);
    auto it = available.find(std::string(source));
    if (it == available.end()) throw Error(404, "log_missing", "Unknown log source");
    std::error_code ec;
    const auto size = fs::file_size(it->second, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory)
            throw Error(404, "log_missing", "Unknown log source");
        throw Error(500, "log_read", "Cannot inspect log source");
    }
    if (offset > size) throw Error(409, "log_truncated", "Log was truncated; resume from cursor 0");
    std::ifstream input(it->second, std::ios::binary);
    if (!input) throw Error(500, "log_read", "Cannot open log source");
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) throw Error(500, "log_read", "Cannot seek log source");
    std::string data(static_cast<std::size_t>(std::min<std::uint64_t>(bytes, size - offset)), '\0');
    if (!data.empty()) {
        input.read(data.data(), static_cast<std::streamsize>(data.size()));
        const auto got = static_cast<std::size_t>(input.gcount());
        if (input.bad()) throw Error(500, "log_read", "Cannot read log source");
        if (got < data.size() || input.eof()) {
            std::error_code size_ec;
            const auto current_size = fs::file_size(it->second, size_ec);
            if (size_ec == std::errc::no_such_file_or_directory)
                throw Error(404, "log_missing", "Unknown log source");
            if ((!size_ec && (current_size < size || offset > current_size)) || input.eof())
                throw Error(409, "log_truncated", "Log was truncated; resume from cursor 0");
            throw Error(500, "log_read", "Cannot read log source");
        }
    }
    std::string encoded(4*((data.size()+2)/3), '\0');
    if (!data.empty()) EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
    auto next = offset + data.size();
    return {{"source", source}, {"cursor", std::to_string(offset)},
            {"next_cursor", std::to_string(next)}, {"has_more", next < size},
            {"encoding", "base64"}, {"data", encoded}};
}
}
