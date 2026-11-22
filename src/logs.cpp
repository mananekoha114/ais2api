// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/logs.hpp"
#include "ais2api/console.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ais2api::logs {
namespace {
void inspect(const fs::path &file) {
    const auto parent = file.parent_path();
    if (!parent.empty() && fs::is_symlink(fs::symlink_status(parent)))
        throw Error(500, "log_path", "Journal directory cannot be a symbolic link");
    const auto status = fs::symlink_status(file);
    if (fs::is_symlink(status) || (fs::exists(status) && !fs::is_regular_file(status)))
        throw Error(500, "log_path", "Journal must be a regular file");
}
// The journal opened for one batch of appends: checked and protected once.
class Writer {
  public:
    explicit Writer(const fs::path &file) {
        inspect(file);
#ifdef _WIN32
        handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE | WRITE_DAC, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw Error(500, "log_write", "Cannot open journal");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            throw Error(500, "log_path", "Journal must be a regular file");
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        auto parent = file.parent_path();
        auto result = GetNamedSecurityInfoW(const_cast<LPWSTR>(parent.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                           nullptr, nullptr, &acl, nullptr, &descriptor);
        if (result == ERROR_SUCCESS)
            result = SetSecurityInfo(handle_, SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, acl, nullptr);
        if (descriptor) LocalFree(descriptor);
        if (result != ERROR_SUCCESS)
            throw Error(500, "log_write", "Cannot protect journal");
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size))
            throw Error(500, "log_read", "Cannot inspect journal before appending");
        size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
        fd_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
        if (fd_ < 0) throw Error(500, "log_write", "Cannot open journal");
        struct stat status{};
        if (::fstat(fd_, &status) || !S_ISREG(status.st_mode) || ::fchmod(fd_, 0600))
            throw Error(500, "log_write", "Cannot protect journal");
        size_ = static_cast<std::uint64_t>(status.st_size);
#endif
    }
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    ~Writer() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    std::uint64_t size() const { return size_; }
    // Reads up to `bytes` from `offset`; returns what was read.
    std::string read(std::uint64_t offset, std::size_t bytes) const {
        std::string data(bytes, '\0');
#ifdef _WIN32
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD count = 0;
        if (!ReadFile(handle_, data.data(), static_cast<DWORD>(bytes), &count, &at))
            throw Error(500, "log_read", "Cannot inspect journal before appending");
        data.resize(count);
#else
        auto count = ::pread(fd_, data.data(), bytes, static_cast<off_t>(offset));
        if (count < 0) throw Error(500, "log_read", "Cannot inspect journal before appending");
        data.resize(static_cast<std::size_t>(count));
#endif
        return data;
    }
    bool complete() const { return !size_ || read(size_ - 1, 1) == "\n"; }
    // Truncates any incomplete trailing line back to the last newline (or 0) so
    // crashes or ENOSPC mid-write cannot wedge future logging.
    void repair_incomplete() {
        if (complete()) return;
        std::uint64_t new_size = 0;
        std::uint64_t pos = size_;
        while (pos > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(pos, 65536));
            pos -= chunk;
            const auto buf = read(pos, chunk);
            if (buf.empty()) break;
            const auto nl = buf.rfind('\n');
            if (nl != std::string::npos) {
                new_size = pos + nl + 1;
                break;
            }
        }
#ifdef _WIN32
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(new_size);
        if (!SetFilePointerEx(handle_, offset, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_))
            throw Error(500, "log_write", "Cannot truncate incomplete journal entry");
#else
        if (::ftruncate(fd_, static_cast<off_t>(new_size)) != 0)
            throw Error(500, "log_write", "Cannot truncate incomplete journal entry");
#endif
        size_ = new_size;
        flush();
    }
    void write(std::string_view data) {
#ifdef _WIN32
        LARGE_INTEGER end{};
        if (!SetFilePointerEx(handle_, end, nullptr, FILE_END))
            throw Error(500, "log_write", "Cannot seek journal");
        while (!data.empty()) {
            DWORD written = 0;
            const auto part = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30));
            if (!WriteFile(handle_, data.data(), part, &written, nullptr) || !written)
                throw Error(500, "log_write", "Cannot persist journal entry");
            data.remove_prefix(written);
            size_ += written;
        }
#else
        while (!data.empty()) {
            auto count = ::write(fd_, data.data(), data.size());
            if (count <= 0) throw Error(500, "log_write", "Cannot persist journal entry");
            data.remove_prefix(static_cast<std::size_t>(count));
            size_ += static_cast<std::uint64_t>(count);
        }
#endif
    }
    void flush() {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) throw Error(500, "log_write", "Cannot persist journal entry");
#else
        if (::fsync(fd_)) throw Error(500, "log_write", "Cannot flush journal");
#endif
    }
  private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    std::uint64_t size_ = 0;
};
// Offset of a file's first byte: zero unless it starts with a rotation record.
std::uint64_t base_of(std::string_view first_line) {
    if (!first_line.starts_with("{\"event\":\"journal_rotated\"")) return 0;
    boost::system::error_code error;
    auto record = json::parse(first_line, error);
    if (error || !record.is_object()) return 0;
    const auto *base = record.as_object().if_contains("base");
    if (base && base->is_uint64()) return base->as_uint64();
    if (base && base->is_int64() && base->as_int64() >= 0) return static_cast<std::uint64_t>(base->as_int64());
    return 0;
}
// The first line when it is a rotation record, else empty.
std::string first_line(std::istream &in) {
    constexpr std::string_view prefix = "{\"event\":\"journal_rotated\"";
    std::string line;
    char c = 0;
    in.seekg(0);
    while (line.size() < Journal::max_record_bytes && in.get(c) && c != '\n') {
        line += c;
        if (line.size() <= prefix.size() && !prefix.starts_with(line)) { line.clear(); break; }
    }
    in.clear();
    return line;
}
std::string rotation_record(std::uint64_t base) {
    return json::serialize(json::object{{"event", "journal_rotated"}, {"base", base},
                                        {"timestamp", unix_seconds()}, {"id", random_hex(12)}}) + '\n';
}
fs::path previous(const fs::path &file) {
    auto name = file.stem();
    name += ".1";
    name += file.extension();
    return file.parent_path() / name;
}
// Appends complete records from `local` (a line boundary) of a file starting at `base`.
struct Page { std::uint64_t next; bool more; };
Page page(std::istream &in, std::uint64_t base, std::uint64_t local, std::size_t limit,
          json::array &entries) {
    in.seekg(static_cast<std::streamoff>(local));
    auto next = base + local;
    std::size_t batch_bytes = 0;
    while (in.peek() != std::char_traits<char>::eof()) {
        std::string line;
        char c = 0;
        bool complete = false;
        while (in.get(c)) {
            if (c == '\n') { complete = true; break; }
            if (line.size() >= Journal::max_record_bytes - 1)
                throw Error(500, "log_record", "Journal entry exceeds size limit");
            line += c;
        }
        if (!complete) break;
        if (entries.size() == limit ||
            (!entries.empty() && batch_bytes + line.size() + 1 > 256 * 1024))
            return {next, true};
        boost::system::error_code error;
        auto record = json::parse(line, error);
        if (error || !record.is_object())
            throw Error(500, "log_record", "Journal contains an invalid entry");
        entries.emplace_back(json::object{{"cursor", std::to_string(next)}, {"record", std::move(record)}});
        next += line.size() + 1;
        batch_bytes += line.size() + 1;
    }
    if (in.bad()) throw Error(500, "log_read", "Cannot read journal");
    return {next, false};
}
std::uint64_t file_size(std::istream &in) {
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) throw Error(500, "log_read", "Cannot inspect journal");
    return static_cast<std::uint64_t>(size);
}
void check_boundary(std::istream &in, std::uint64_t local) {
    if (!local) return;
    in.seekg(static_cast<std::streamoff>(local - 1));
    if (in.get() != '\n')
        throw Error(400, "invalid_cursor", "Journal cursor is not a line boundary");
}
// True when this process's stderr is `file`.
bool is_stderr(const fs::path &file) {
#ifdef _WIN32
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (!error || error == INVALID_HANDLE_VALUE || GetFileType(error) != FILE_TYPE_DISK) return false;
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    if (!GetFileInformationByHandle(error, &a)) return false;
    HANDLE handle = CreateFileW(file.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool same = GetFileInformationByHandle(handle, &b) &&
                      !(b.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                      a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
                      a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
    CloseHandle(handle);
    return same;
#else
    struct stat a{}, b{};
    return !::fstat(STDERR_FILENO, &a) && S_ISREG(a.st_mode) && !::lstat(file.c_str(), &b) &&
           S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
}
bool truncate_file(const fs::path &file) {
#ifdef _WIN32
    // Other handles append (FILE_APPEND_DATA), so they continue at the new end.
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER start{};
    const bool ok = SetFilePointerEx(handle, start, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
    CloseHandle(handle);
    return ok;
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool ok = !::ftruncate(fd, 0);
    ::close(fd);
    return ok;
#endif
}
}
Journal::Journal(fs::path data_dir, std::uint64_t rotate_bytes)
    : file_(fs::absolute(data_dir) / "management.log"), rotate_bytes_(rotate_bytes) {
    inspect(file_);
    secure_directory(file_.parent_path());
    Writer writer{file_};
    writer.repair_incomplete();
    writer.flush();
    if (const auto service = file_.parent_path() / "service.log"; is_stderr(service)) mirror_ = service;
}
void Journal::append(json::object record) {
    std::vector<json::object> records;
    records.push_back(std::move(record));
    append(std::move(records));
}
void Journal::append(std::vector<json::object> records) {
    std::string data;
    std::vector<json::object> written;
    bool oversized = false;
    for (auto &record : records) {
        if (!record.contains("timestamp")) record["timestamp"] = unix_seconds();
        if (!record.contains("id")) record["id"] = random_hex(12);
        auto line = json::serialize(record) + '\n';
        if (line.size() > max_record_bytes) oversized = true;
        else {
            data += line;
            written.push_back(record);
        }
    }
    if (!data.empty()) {
        std::lock_guard lock(mutex_);
        // Emit once even when persistence fails; the caller still receives the error.
        console::emit_records(written, data);
        std::optional<Writer> writer(std::in_place, file_);
        writer->repair_incomplete();
        const auto size = writer->size();
        if (rotate_bytes_ && size && size + data.size() > rotate_bytes_ && size >= rotate_retry_) {
            auto head = writer->read(0, max_record_bytes);
            const auto base = base_of(std::string_view(head).substr(0, head.find('\n'))) + size;
            const auto rotation = rotation_record(base);
            fs::path staged = file_;
            staged += ".rot-" + random_hex(8);
            struct RemoveStaged {
                fs::path path;
                ~RemoveStaged() {
                    if (!path.empty()) {
                        std::error_code ignored;
                        fs::remove(path, ignored);
                    }
                }
            } staged_cleanup{staged};
            atomic_write(staged, rotation, true);
            writer.reset();
            std::error_code error;
            fs::rename(file_, previous(file_), error);
            // Retry later, e.g. after a Windows reader that held the file closes it.
            if (error) {
                rotate_retry_ = size + rotate_bytes_ / 8;
                writer.emplace(file_);
            } else {
                rotate_retry_ = 0;
                std::error_code stage_error;
                fs::rename(staged, file_, stage_error);
                if (!stage_error) {
                    staged_cleanup.path.clear();
                    writer.emplace(file_);
                } else {
                    writer.emplace(file_);
                    writer->write(rotation);
                }
            }
        }
        writer->write(data);
        writer->flush();
        writer.reset();
        trim_mirror();
    }
    if (oversized) throw Error(413, "log_limit", "Journal entry exceeds size limit");
}
void Journal::trim_mirror() {
    // service.log repeats every journal line; keep one previous generation.
    if (mirror_.empty() || !rotate_bytes_) return;
    std::error_code error;
    const auto size = fs::file_size(mirror_, error);
    if (error || size <= rotate_bytes_ || size < mirror_retry_) return;
    mirror_retry_ = size + rotate_bytes_ / 8;
    if (fs::is_symlink(fs::symlink_status(mirror_, error)) || error) return;
    std::ifstream in(mirror_, std::ios::binary);
    if (!in) return;
    std::ostringstream contents;
    contents << in.rdbuf();
    if (!contents) return;
    try { atomic_write(previous(mirror_), contents.view(), true); }
    catch (...) { return; }
    if (truncate_file(mirror_)) mirror_retry_ = 0;
}
json::object Journal::read(std::string_view cursor, std::size_t limit) const {
    std::uint64_t offset = 0;
    const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
    if (cursor.empty() || parsed.ec != std::errc{} || parsed.ptr != cursor.data() + cursor.size() ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        limit == 0 || limit > 500)
        throw Error(400, "invalid_cursor", "Invalid journal cursor or limit");
    // Readers observe complete lines only and may retry an unfinished tail.
    // Do not wait on a writer doing fsync or a backpressured stderr write.
    std::ifstream in;
    std::uint64_t size = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        inspect(file_);
        in = std::ifstream(file_, std::ios::binary);
        if (in) {
            size = file_size(in);
            std::error_code prev_ec;
            if (size > 0 || (offset == 0 && !fs::exists(previous(file_), prev_ec))) break;
        }
        if (attempt + 1 < 10)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!in) throw Error(500, "log_read", "Cannot read journal");
    const auto base = base_of(first_line(in));
    json::array entries;
    if (offset < base) {
        // Finish the previous generation when it still holds the cursor;
        // otherwise resume at the oldest retained record.
        const auto old_file = previous(file_);
        std::ifstream old;
        std::error_code old_ec;
        if (!old_ec && fs::is_regular_file(fs::symlink_status(old_file, old_ec)) && !old_ec)
            old.open(old_file, std::ios::binary);
        if (old) {
            const auto old_size = file_size(old);
            const auto old_base = base_of(first_line(old));
            if (old_base + old_size == base) {
                if (offset < old_base) offset = old_base;
                else check_boundary(old, offset - old_base);
                auto result = page(old, old_base, offset - old_base, limit, entries);
                if (result.more)
                    return {{"entries", std::move(entries)}, {"next_cursor", std::to_string(result.next)}, {"has_more", true}};
            }
        }
        offset = base;
        if (!entries.empty())
            return {{"entries", std::move(entries)}, {"next_cursor", std::to_string(base)}, {"has_more", size > 0}};
    }
    const auto local = offset - base;
    if (local > size)
        throw Error(400, "invalid_cursor", "Journal cursor is beyond end of file");
    check_boundary(in, local);
    auto result = page(in, base, local, limit, entries);
    return {{"entries", std::move(entries)}, {"next_cursor", std::to_string(result.next)}, {"has_more", result.more}};
}
} // namespace ais2api::logs
