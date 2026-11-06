// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../browser/process.hpp"
#include <algorithm>
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
void OwnedProcess::start(const fs::path& executable, const std::vector<std::string>& args, const fs::path& private_root) {
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
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &pi)) { stop(); throw Error(500, "browser_launch", "CreateProcessW failed"); }
    process_ = pi.hProcess;
    if (!AssignProcessToJobObject(job_, process_)) { TerminateProcess(process_, 1); CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot assign browser to Job Object"); }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) { CloseHandle(pi.hThread); stop(); throw Error(500, "browser_launch", "Cannot resume browser process"); }
    CloseHandle(pi.hThread);
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
    inherited["XDG_CONFIG_HOME"] = settings.string();
    inherited["XDG_CACHE_HOME"] = cache.string();
    inherited["XDG_DATA_HOME"] = data.string();
    inherited["XDG_STATE_HOME"] = state.string();
    inherited["TMPDIR"] = temporary.string();
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
    if (!termux) inherited["HOME"] = home.string();
    std::vector<std::string> environment;
    for (const auto& [key, value] : inherited) environment.push_back(key + "=" + value);
    std::vector<char*> envp;
    for (auto& value : environment) envp.push_back(value.data());
    envp.push_back(nullptr);
    pid_ = fork();
    if (pid_ < 0) throw Error(500, "browser_launch", "fork failed");
    if (pid_ == 0) {
        if (setsid() < 0) _exit(126);
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); dup2(nullfd, STDOUT_FILENO); dup2(nullfd, STDERR_FILENO); if (nullfd > 2) ::close(nullfd); }
        execve(argv[0], argv.data(), envp.data()); _exit(127);
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
}
} // namespace ais2api::browser_detail
