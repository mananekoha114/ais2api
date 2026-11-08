// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/common.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <charconv>
#include <fstream>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace ais2api {
namespace {
std::string hex(const unsigned char *data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    return out;
}
void restrict_access(const fs::path &path, bool directory) {
#ifdef _WIN32
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw Error(500, "permissions", "Cannot inspect current user");
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> buffer(length);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), length, &length)) {
        CloseHandle(token);
        throw Error(500, "permissions", "Cannot inspect current user");
    }
    CloseHandle(token);
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName =
        reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid);
    PACL acl{};
    auto error = SetEntriesInAclW(1, &access, nullptr, &acl);
    if (error == ERROR_SUCCESS)
        error =
            SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, acl, nullptr);
    if (acl)
        LocalFree(acl);
    if (error != ERROR_SUCCESS)
        throw Error(500, "permissions", "Cannot protect data path");
#else
    if (::chmod(path.c_str(), directory ? 0700 : 0600) != 0)
        throw Error(500, "permissions", "Cannot protect data path");
#endif
}
// scripts/check-package.py looks for the whole tag in the executable.
#ifdef AIS2API_DEV_BUNDLE
constexpr char build_tag[] = "ais2api-build-mode=dev";
#else
constexpr char build_tag[] = "ais2api-build-mode=release";
#endif
} // namespace
std::string_view build_mode() {
    const std::string_view tag(build_tag);
    return tag.substr(tag.find('=') + 1);
}
std::string random_hex(std::size_t bytes) {
    if (bytes > 1024)
        throw Error(500, "random", "Invalid random request");
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(bytes)) != 1)
        throw Error(500, "random", "Secure random generator failed");
    return hex(buffer.data(), bytes);
}
std::string sha256(std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned size = 0;
    if (EVP_Digest(input.data(), input.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1)
        throw Error(500, "digest", "SHA-256 failed");
    return hex(digest.data(), size);
}
Sha256::Sha256() : context_(EVP_MD_CTX_new()) {
    if (!context_ || EVP_DigestInit_ex(static_cast<EVP_MD_CTX *>(context_), EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(context_));
        throw Error(500, "digest", "SHA-256 failed");
    }
}
Sha256::~Sha256() { EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(context_)); }
void Sha256::update(std::string_view input) {
    if (!input.empty() && EVP_DigestUpdate(static_cast<EVP_MD_CTX *>(context_), input.data(), input.size()) != 1)
        throw Error(500, "digest", "SHA-256 failed");
}
std::string Sha256::finish() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned size = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX *>(context_), digest.data(), &size) != 1)
        throw Error(500, "digest", "SHA-256 failed");
    return hex(digest.data(), size);
}
// Mirrors Boost.JSON's escaping: \b \t \n \f \r \" \\ take two bytes, other
// control characters \u00XX six; everything else, including UTF-8, is copied.
std::size_t json_size(json::string_view text) {
    static constexpr auto extra = [] {
        std::array<unsigned char, 256> table{};
        for (int c = 0; c < 0x20; ++c) table[c] = 5;
        for (unsigned char c : {'\b', '\t', '\n', '\f', '\r', '"', '\\'}) table[c] = 1;
        return table;
    }();
    std::size_t size = text.size() + 2;
    for (unsigned char c : text) size += extra[c];
    return size;
}
std::size_t json_size(const json::array &array) {
    std::size_t size = array.empty() ? 2 : array.size() + 1;
    for (const auto &item : array) size += json_size(item);
    return size;
}
std::size_t json_size(const json::object &object) {
    std::size_t size = object.empty() ? 2 : 2 * object.size() + 1;
    for (const auto &item : object) size += json_size(item.key()) + json_size(item.value());
    return size;
}
std::size_t json_size(const json::value &value) {
    char digits[24];
    switch (value.kind()) {
    case json::kind::null: return 4;
    case json::kind::bool_: return value.get_bool() ? 4 : 5;
    case json::kind::int64: return std::to_chars(digits, digits + sizeof digits, value.get_int64()).ptr - digits;
    case json::kind::uint64: return std::to_chars(digits, digits + sizeof digits, value.get_uint64()).ptr - digits;
    case json::kind::double_: return json::serialize(value).size();
    case json::kind::string: return json_size(json::string_view(value.get_string()));
    case json::kind::array: return json_size(value.get_array());
    case json::kind::object: return json_size(value.get_object());
    }
    return json::serialize(value).size();
}
void secure_directory(const fs::path &path) {
    if (fs::is_symlink(fs::symlink_status(path)))
        throw Error(400, "unsafe_path", "Data directory cannot be a symbolic link");
    fs::create_directories(path);
    restrict_access(path, true);
}
void atomic_write(const fs::path &path, std::string_view contents, bool secret) {
    fs::create_directories(path.parent_path());
    if (fs::is_symlink(fs::symlink_status(path)))
        throw Error(400, "unsafe_path", "Refusing to replace symbolic link");
    auto temporary = path;
    temporary += ".tmp-" + random_hex(8);
    try {
#ifdef _WIN32
        HANDLE h = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            throw Error(500, "write_failed", "Cannot create temporary file");
        std::size_t offset = 0;
        while (offset < contents.size()) {
            DWORD wrote = 0;
            auto amount =
                static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset, 1024 * 1024));
            if (!WriteFile(h, contents.data() + offset, amount, &wrote, nullptr) || !wrote) {
                CloseHandle(h);
                throw Error(500, "write_failed", "Cannot write temporary file");
            }
            offset += wrote;
        }
        const bool flushed = FlushFileBuffers(h) != 0;
        CloseHandle(h);
        if (!flushed)
            throw Error(500, "write_failed", "Cannot flush temporary file");
        if (secret)
            restrict_access(temporary, false);
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw Error(500, "write_failed", "Cannot replace file");
#else
        int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0)
            throw Error(500, "write_failed", "Cannot create temporary file");
        std::size_t offset = 0;
        while (offset < contents.size()) {
            auto n = ::write(fd, contents.data() + offset, contents.size() - offset);
            if (n < 0) {
                ::close(fd);
                throw Error(500, "write_failed", "Cannot write temporary file");
            }
            offset += static_cast<std::size_t>(n);
        }
        const bool flushed = ::fsync(fd) == 0;
        ::close(fd);
        if (!flushed)
            throw Error(500, "write_failed", "Cannot flush temporary file");
        fs::rename(temporary, path);
        int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (parent >= 0) {
            ::fsync(parent);
            ::close(parent);
        }
        (void)secret;
#endif
    } catch (...) {
        std::error_code ec;
        fs::remove(temporary, ec);
        throw;
    }
}
std::string read_file(const fs::path &path, std::size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error(400, "file_read", "Cannot read requested file");
    std::string output;
    std::array<char, 16384> block{};
    while (in) {
        in.read(block.data(), block.size());
        auto count = static_cast<std::size_t>(in.gcount());
        if (count > limit - output.size())
            throw Error(413, "file_limit", "File exceeds size limit");
        output.append(block.data(), count);
    }
    if (!in.eof())
        throw Error(500, "file_read", "File read failed");
    return output;
}
json::object error_json(const Error &e, bool openai) {
    if (openai)
        return {{"error",
                 {{"message", e.what()}, {"type", e.code}, {"code", e.code}, {"param", nullptr}}}};
    std::string status = "INTERNAL";
    if (e.status == 400)
        status = "INVALID_ARGUMENT";
    else if (e.status == 401)
        status = "UNAUTHENTICATED";
    else if (e.status == 403)
        status = "PERMISSION_DENIED";
    else if (e.status == 404)
        status = "NOT_FOUND";
    else if (e.status == 413 || e.status == 429)
        status = "RESOURCE_EXHAUSTED";
    else if (e.status == 503)
        status = "UNAVAILABLE";
    else if (e.status == 504)
        status = "DEADLINE_EXCEEDED";
    return {{"error",
             {{"code", e.status}, {"message", e.what()}, {"status", status}, {"reason", e.code}}}};
}
} // namespace ais2api
