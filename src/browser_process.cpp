// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../browser/process.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
extern char **environ;
#endif

namespace ais2api::browser_detail {
using namespace std::chrono_literals;

void prune_browser_logs(const fs::path& directory, std::string_view prefix, std::size_t keep) {
    std::error_code error;
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto name = path_utf8(it->path().filename());
        if (!name.starts_with(prefix) || !name.ends_with(".log") || !it->is_regular_file(error)) continue;
        logs.emplace_back(it->last_write_time(error), it->path());
    }
    if (logs.size() <= keep) return;
    std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = keep; i < logs.size(); ++i) fs::remove(logs[i].second, error);
}
void pause(std::stop_token token, std::chrono::milliseconds duration) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) { check_cancel(token); std::this_thread::sleep_for(20ms); }
}
std::vector<std::pair<std::string, std::string>> private_environment(const fs::path& private_root) {
    const auto root = fs::absolute(private_root);
    return {{"HOME", path_utf8(root / "home")},           {"XDG_CONFIG_HOME", path_utf8(root / "settings")},
            {"XDG_CACHE_HOME", path_utf8(root / "cache")}, {"XDG_DATA_HOME", path_utf8(root / "files")},
            {"XDG_STATE_HOME", path_utf8(root / "state")}, {"TMPDIR", path_utf8(root / "tmp")}};
}
void OwnedProcess::start(const fs::path& executable, const std::vector<std::string>& args, const fs::path& private_root,
                         const ProcessOptions& options) {
    stop();
    // Build a child-only environment before fork/CreateProcess. Never mutate
    // this multithreaded service's process-wide environment.
    const auto home = fs::absolute(private_root / "home");
    const auto cache = fs::absolute(private_root / "cache");
    const auto settings = fs::absolute(private_root / "settings");
    const auto temporary = fs::absolute(private_root / "tmp");
    const auto data = fs::absolute(private_root / "files");
    const auto state = fs::absolute(private_root / "state");
    for (const auto& directory : {home, cache, settings, temporary, data, state}) secure_directory(directory);
    const auto prefix = std::getenv("PREFIX");
    const bool termux = std::getenv("TERMUX_VERSION") || (prefix && std::string_view(prefix).find("com.termux") != std::string_view::npos);
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        if (s.empty()) return std::wstring{};
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (!n) throw Error(500, "browser_launch", "Invalid UTF-8 launch argument");
        std::wstring result(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
        return result;
    };
    auto quote = [](const std::wstring& value) {
        std::wstring result = L"\"";
        unsigned slashes = 0;
        for (wchar_t c : value) {
            if (c == L'\\') { ++slashes; continue; }
            if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
            else { result.append(slashes, L'\\'); result += c; }
            slashes = 0;
        }
        result.append(slashes * 2, L'\\'); result += L'"'; return result;
    };
    std::wstring command = quote(executable.wstring());
    for (const auto& arg : args) command += L" " + quote(wide(arg));
    auto environment_less = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    };
    std::map<std::wstring, std::wstring, decltype(environment_less)> inherited(environment_less);
    auto raw_environment = GetEnvironmentStringsW();
    if (!raw_environment) throw Error(500, "browser_launch", "Cannot read browser environment");
    for (auto entry = raw_environment; *entry; entry += std::wcslen(entry) + 1) {
        std::wstring value(entry);
        // Hidden drive-current-directory variables begin with '=' (e.g. =C:).
        const auto separator = value.find(L'=', value.front() == L'=' ? 1 : 0);
        if (separator != std::wstring::npos) inherited[value.substr(0, separator)] = value.substr(separator + 1);
    }
    FreeEnvironmentStringsW(raw_environment);
    for (const auto& unset : options.unset_prefixes) {
        const auto name = wide(unset);
        std::erase_if(inherited, [&](const auto& item) {
            return item.first.size() >= name.size() && CompareStringOrdinal(item.first.c_str(), static_cast<int>(name.size()),
                       name.c_str(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
        });
    }
    inherited[L"LOCALAPPDATA"] = cache.wstring();
    inherited[L"APPDATA"] = settings.wstring();
    inherited[L"TEMP"] = temporary.wstring();
    inherited[L"TMP"] = temporary.wstring();
    if (!termux) {
        inherited[L"USERPROFILE"] = home.wstring();
        inherited[L"HOME"] = home.wstring();
        inherited[L"HOMEDRIVE"] = home.root_name().wstring();
        inherited[L"HOMEPATH"] = home.relative_path().empty() ? L"\\" : L"\\" + home.relative_path().wstring();
    }
    for (const auto& [key, value] : options.environment) inherited[wide(key)] = wide(value);
    std::vector<wchar_t> environment;
    for (const auto& [key, value] : inherited) {
        environment.insert(environment.end(), key.begin(), key.end()); environment.push_back(L'=');
        environment.insert(environment.end(), value.begin(), value.end()); environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) throw Error(500, "browser_launch", "CreateJobObject failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { stop(); throw Error(500, "browser_launch", "Cannot configure browser Job Object"); }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = options.on_output ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    // The child's ends of the output pipe; the parent closes them on every path.
    HANDLE output_write = nullptr, input = nullptr;
    struct ChildEnds {
        HANDLE& write; HANDLE& input;
        ~ChildEnds() { if (write) CloseHandle(write); if (input) CloseHandle(input); }
    } child_ends{output_write, input};
    std::vector<unsigned char> attributes;
    HANDLE inheritable_handles[2]{};
    if (options.on_output) {
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        HANDLE read = nullptr;
        if (!CreatePipe(&read, &output_write, &inheritable, 0)) { stop(); throw Error(500, "browser_launch", "Cannot create browser output pipe"); }
        output_ = read;
        input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
        if (input == INVALID_HANDLE_VALUE) input = nullptr;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes.resize(size);
        // Only these two handles reach the child, not every inheritable
        // handle this multithreaded service happens to hold.
        inheritable_handles[0] = input;
        inheritable_handles[1] = output_write;
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!input || !SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) || !InitializeProcThreadAttributeList(list, 1, 0, &size)) {
            stop(); throw Error(500, "browser_launch", "Cannot prepare browser output pipe");
        }
        startup.lpAttributeList = list;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritable_handles, sizeof(inheritable_handles), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list); stop(); throw Error(500, "browser_launch", "Cannot prepare browser output pipe");
        }
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = output_write;
        startup.StartupInfo.hStdError = output_write;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, options.on_output ? TRUE : FALSE, flags,
                                        environment.data(), nullptr, &startup.StartupInfo, &pi);
    if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) { stop(); throw Error(500, "browser_launch", "CreateProcessW failed"); }
    process_ = pi.hProcess;
    if (!AssignProcessToJobObject(job_, process_)) { TerminateProcess(process_, 1); CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot assign browser to Job Object"); }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) { CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot resume browser process"); }
    CloseHandle(pi.hThread);
    if (options.on_output) reader_ = std::thread([this, on_output = options.on_output] { read_output(on_output); });
#else
    std::vector<std::string> values{executable.string()};
    values.insert(values.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::map<std::string, std::string> inherited;
    for (auto entry = ::environ; entry && *entry; ++entry) {
        std::string value(*entry);
        auto separator = value.find('=');
        if (separator != std::string::npos) inherited[value.substr(0, separator)] = value.substr(separator + 1);
    }
    for (const auto& unset : options.unset_prefixes)
        std::erase_if(inherited, [&](const auto& item) { return item.first.starts_with(unset); });
    for (const auto& [key, value] : private_environment(private_root))
        if (key != "HOME" || !termux) inherited[key] = value;
#if defined(__linux__) && !defined(__ANDROID__)
    if (!termux) {
        // Chromium binds AF_UNIX singleton sockets below TMPDIR. Their
        // 108-byte address limit otherwise breaks portable installs in
        // deeply nested directories. Keep the actual files in this private
        // profile; a held parent directory fd provides a short kernel alias.
        temporary_directory_fd_ = open(temporary.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (temporary_directory_fd_ < 0) throw Error(500, "browser_launch", "Cannot open private browser temporary directory");
        const auto alias = "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(temporary_directory_fd_);
        if (!fs::is_directory(alias)) throw Error(500, "browser_launch", "Cannot access private browser temporary directory through /proc");
        inherited["TMPDIR"] = alias;
    }
#endif
    for (const auto& [key, value] : options.environment) inherited[key] = value;
    std::vector<std::string> environment;
    for (const auto& [key, value] : inherited) environment.push_back(key + "=" + value);
    std::vector<char*> envp;
    for (auto& value : environment) envp.push_back(value.data());
    envp.push_back(nullptr);
    int output_write = -1;
    if (options.on_output) {
        int ends[2];
        if (pipe(ends) != 0) { stop(); throw Error(500, "browser_launch", "Cannot create browser output pipe"); }
        for (int end : ends) fcntl(end, F_SETFD, FD_CLOEXEC);
        output_ = ends[0];
        output_write = ends[1];
    }
    pid_ = fork();
    if (pid_ < 0) {
        if (output_write >= 0) ::close(output_write);
        stop();
        throw Error(500, "browser_launch", "fork failed");
    }
    if (pid_ == 0) {
        if (setsid() < 0) _exit(126);
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            if (output_write < 0) { dup2(nullfd, STDOUT_FILENO); dup2(nullfd, STDERR_FILENO); }
            if (nullfd > 2) ::close(nullfd);
        }
        if (output_write >= 0) { dup2(output_write, STDOUT_FILENO); dup2(output_write, STDERR_FILENO); }
        execve(argv[0], argv.data(), envp.data()); _exit(127);
    }
    if (output_write >= 0) {
        ::close(output_write);
        reader_ = std::thread([this, on_output = options.on_output] { read_output(on_output); });
    }
#endif
}
bool OwnedProcess::alive() {
#ifdef _WIN32
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
    if (pid_ <= 0) return false;
    siginfo_t info{};
    // Leave an exited leader unreaped until stop(), preventing PID/group
    // reuse while descendants are still owned by this gateway.
    return waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
#endif
}
void OwnedProcess::stop() noexcept {
#ifdef _WIN32
    if (job_) { CloseHandle(job_); job_ = nullptr; }
    if (process_) { WaitForSingleObject(process_, 5000); CloseHandle(process_); process_ = nullptr; }
#else
    if (pid_ > 0) {
        kill(-pid_, SIGTERM);
        kill(pid_, SIGTERM);
        for (unsigned i = 0; i < 30; ++i) { siginfo_t info{}; if (waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid) break; std::this_thread::sleep_for(20ms); }
        kill(-pid_, SIGKILL);
        int status = 0; while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
    if (temporary_directory_fd_ >= 0) { ::close(temporary_directory_fd_); temporary_directory_fd_ = -1; }
#endif
    stop_reader();
}
// Reads until every writer has exited, or until stop() once the pipe is idle
// (a descendant outside the process group could otherwise hold it open).
void OwnedProcess::read_output(std::function<void(std::string_view)> on_output) {
    std::string pending;
    char buffer[4096];
    auto deliver = [&](bool flush) {
        std::size_t start = 0;
        for (std::size_t end; (end = pending.find('\n', start)) != std::string::npos; start = end + 1) {
            auto line = std::string_view(pending).substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            try { on_output(line); } catch (...) {}
        }
        pending.erase(0, start);
        // An endless line is passed on in pieces instead of growing without bound.
        if (flush ? !pending.empty() : pending.size() >= 64 * 1024) {
            try { on_output(pending); } catch (...) {}
            pending.clear();
        }
    };
    for (;;) {
#ifdef _WIN32
        DWORD available = 0;
        // Fails with a broken pipe once every writer has exited.
        if (!PeekNamedPipe(output_, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            if (stopping_) break;
            std::this_thread::sleep_for(20ms);
            continue;
        }
        DWORD count = 0;
        if (!ReadFile(output_, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || !count) break;
#else
        pollfd descriptor{output_, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 20);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) {
            if (stopping_) break;
            continue;
        }
        const auto count = ::read(output_, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
#endif
        pending.append(buffer, static_cast<std::size_t>(count));
        deliver(false);
    }
    deliver(true);
}
void OwnedProcess::stop_reader() noexcept {
    stopping_ = true;
    if (reader_.joinable()) reader_.join();
    stopping_ = false;
#ifdef _WIN32
    if (output_) { CloseHandle(output_); output_ = nullptr; }
#else
    if (output_ >= 0) { ::close(output_); output_ = -1; }
#endif
}
} // namespace ais2api::browser_detail
