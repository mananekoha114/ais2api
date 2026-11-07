// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/runtime.hpp"
#include <map>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
namespace ais2api {
namespace {
std::map<std::string, std::string> child_overrides(const Config &effective) {
    auto base = Config::load(effective.config_file).to_json();
    auto actual = effective.to_json();
    std::map<std::string, std::string> result;
    for (auto pair : {std::pair{"bind", "AIS2API_BIND"},
                      {"port", "AIS2API_PORT"},
                      {"data_dir", "AIS2API_DATA_DIR"},
                      {"browser_executable", "AIS2API_BROWSER_EXECUTABLE"},
                      {"browser_backend", "AIS2API_BROWSER_BACKEND"},
                      {"proxy", "AIS2API_PROXY"},
                      {"browser_proxy_relay", "AIS2API_BROWSER_PROXY_RELAY"},
                      {"browser_remote_allow_origins", "AIS2API_BROWSER_REMOTE_ALLOW_ORIGINS"},
                      {"browser_site_isolation", "AIS2API_BROWSER_SITE_ISOLATION"},
                      {"headless", "AIS2API_HEADLESS"},
                      {"continuation_retry_enabled", "AIS2API_CONTINUATION_RETRY_ENABLED"},
                      {"browser_idle_seconds", "AIS2API_BROWSER_IDLE_SECONDS"},
                      {"limits", "AIS2API_LIMITS"},
                      {"model_aliases", "AIS2API_MODEL_ALIASES"},
                      {"configured_models", "AIS2API_CONFIGURED_MODELS"},
                      {"model_defaults", "AIS2API_MODEL_DEFAULTS"}}) {
        auto &value = actual.at(pair.first);
        if (value != base.at(pair.first))
            result[pair.second] =
                value.is_string() ? std::string(value.as_string()) : json::serialize(value);
    }
    const auto &limits = actual.at("limits").as_object();
    const auto &base_limits = base.at("limits").as_object();
    for (auto pair : {std::pair{"request_bytes", "AIS2API_REQUEST_BYTES"},
                      {"object_bytes", "AIS2API_OBJECT_BYTES"},
                      {"relay_bytes", "AIS2API_RELAY_BYTES"},
                      {"queue_size", "AIS2API_QUEUE_SIZE"},
                      {"queue_seconds", "AIS2API_QUEUE_SECONDS"},
                      {"generation_seconds", "AIS2API_GENERATION_SECONDS"},
                      {"stream_idle_seconds", "AIS2API_STREAM_IDLE_SECONDS"},
                      {"max_attempts", "AIS2API_MAX_ATTEMPTS"},
                      {"max_continuation_retries", "AIS2API_MAX_CONTINUATION_RETRIES"}}) {
        if (limits.at(pair.first) != base_limits.at(pair.first))
            result[pair.second] = json::serialize(limits.at(pair.first));
    }
    return result;
}
#ifdef _WIN32
std::wstring wide(std::string_view input) {
    if (input.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                static_cast<int>(input.size()), nullptr, 0);
    if (!n)
        throw Error(400, "process", "Invalid UTF-8 argument");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                        out.data(), n);
    return out;
}
std::wstring quote(const std::wstring &arg) {
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (auto ch : arg) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out += ch;
        } else {
            out.append(slashes, L'\\');
            out += ch;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
std::wstring command_line(const std::vector<std::string> &args) {
    std::wstring out;
    for (auto &a : args) {
        if (!out.empty())
            out += L' ';
        out += quote(wide(a));
    }
    return out;
}
struct HandleList {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    HandleList(HANDLE *handles, std::size_t count) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
            list = nullptr;
            throw Error(500, "process", "Cannot initialize child handle list");
        }
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                       count * sizeof(HANDLE), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list);
            list = nullptr;
            throw Error(500, "process", "Cannot restrict child handle inheritance");
        }
    }
    ~HandleList() {
        if (list)
            DeleteProcThreadAttributeList(list);
    }
};
struct EnvironmentLess {
    bool operator()(const std::wstring &a, const std::wstring &b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
std::vector<wchar_t> child_environment(const Config &config) {
    std::map<std::wstring, std::wstring, EnvironmentLess> values;
    auto block = GetEnvironmentStringsW();
    if (!block)
        throw Error(500, "process", "Cannot read process environment");
    for (auto item = block; *item; item += wcslen(item) + 1) {
        std::wstring entry = item;
        auto equals = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
        if (equals != std::wstring::npos)
            values[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
    FreeEnvironmentStringsW(block);
    for (auto &[name, value] : child_overrides(config))
        values[wide(name)] = wide(value);
    std::vector<wchar_t> result;
    for (auto &[name, value] : values) {
        result.insert(result.end(), name.begin(), name.end());
        result.push_back(L'=');
        result.insert(result.end(), value.begin(), value.end());
        result.push_back(0);
    }
    result.push_back(0);
    if (result.size() == 1)
        result.push_back(0);
    return result;
}
std::wstring user_sid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw Error(500, "permissions", "Cannot inspect user");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token, TokenUser, data.data(), size, &size)) {
        CloseHandle(token);
        throw Error(500, "permissions", "Cannot inspect user");
    }
    CloseHandle(token);
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid))
        throw Error(500, "permissions", "Cannot inspect SID");
    std::wstring result = sid;
    LocalFree(sid);
    return result;
}
std::string utf8(const fs::path &p) {
    auto s = p.u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
#endif
} // namespace
fs::path executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        auto n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!n)
            throw Error(500, "executable", "Cannot resolve executable");
        if (n < buffer.size() - 1)
            return fs::path(std::wstring(buffer.data(), n));
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768)
            throw Error(500, "executable", "Executable path too long");
    }
#else
    std::vector<char> buffer(512);
    for (;;) {
        auto n = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (n < 0)
            throw Error(500, "executable", "Cannot resolve executable");
        if (static_cast<std::size_t>(n) < buffer.size())
            return fs::path(std::string(buffer.data(), static_cast<std::size_t>(n)));
        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 1024 * 1024)
            throw Error(500, "executable", "Executable path too long");
    }
#endif
}
int run_process(const std::vector<std::string> &args) {
    if (args.empty() || args[0].empty())
        throw Error(400, "process", "Executable argument is required");
    for (auto &a : args)
        if (a.find('\0') != std::string::npos)
            throw Error(400, "process", "NUL in process argument");
#ifdef _WIN32
    auto line = command_line(args);
    HANDLE output = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    auto error_handle = GetStdHandle(STD_ERROR_HANDLE);
    if (!error_handle || error_handle == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), error_handle, GetCurrentProcess(), &output, 0, TRUE,
                         DUPLICATE_SAME_ACCESS))
        output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, 0, nullptr);
    auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, 0, nullptr);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE)
            CloseHandle(output);
        if (input != INVALID_HANDLE_VALUE)
            CloseHandle(input);
        throw Error(500, "process", "Cannot prepare child standard handles");
    }
    HANDLE inherited[] = {input, output};
    std::unique_ptr<HandleList> handle_list;
    try {
        handle_list = std::make_unique<HandleList>(inherited, 2);
    } catch (...) {
        CloseHandle(input);
        CloseHandle(output);
        throw;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.lpAttributeList = handle_list->list;
    PROCESS_INFORMATION process{};
    auto created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                  &startup.StartupInfo, &process);
    CloseHandle(input);
    CloseHandle(output);
    if (!created)
        throw Error(500, "process", "Cannot start process");
    CloseHandle(process.hThread);
    auto wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    bool ok = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (!ok)
        throw Error(500, "process", "Cannot wait for process");
    return static_cast<int>(code);
#else
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid{};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
        throw Error(500, "process", "Cannot prepare child standard handles");
    auto prepared = posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
    auto launched = prepared
                        ? prepared
                        : ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (launched != 0)
        throw Error(500, "process", "Cannot start process");
    int status;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        throw Error(500, "process", "Cannot wait for process");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status)
                             : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}
void spawn_service(const Config &config) {
    if (config.config_file.empty())
        throw Error(400, "invalid_config", "Service requires a configuration file path");
    secure_directory(config.data_dir);
    if (fs::is_symlink(fs::symlink_status(config.data_dir / "service.log")))
        throw Error(400, "unsafe_path", "Service log cannot be a symbolic link");
#ifdef _WIN32
    auto line = command_line(
        {utf8(executable_path()), "serve", "--config", utf8(fs::absolute(config.config_file))});
    auto environment = child_environment(config);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log = CreateFileW((config.data_dir / "service.log").c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        throw Error(500, "process", "Cannot open service log");
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        CloseHandle(log);
        throw Error(500, "process", "Cannot open service input");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = log;
    startup.StartupInfo.hStdError = log;
    HANDLE inherited[] = {input, log};
    std::unique_ptr<HandleList> handle_list;
    try {
        handle_list = std::make_unique<HandleList>(inherited, 2);
    } catch (...) {
        CloseHandle(input);
        CloseHandle(log);
        throw;
    }
    startup.lpAttributeList = handle_list->list;
    PROCESS_INFORMATION process{};
    auto ok = CreateProcessW(executable_path().c_str(), line.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                                 CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                             environment.data(), nullptr, &startup.StartupInfo, &process);
    CloseHandle(input);
    CloseHandle(log);
    if (!ok)
        throw Error(500, "process", "Cannot start service");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    auto file = fs::absolute(config.config_file).string();
    auto exe = executable_path().string();
    auto log_path = (config.data_dir / "service.log").string();
    std::map<std::string, std::string> inherited;
    for (char **p = environ; *p; ++p) {
        std::string entry = *p;
        auto equals = entry.find('=');
        if (equals != std::string::npos)
            inherited[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
    for (auto &[key, value] : child_overrides(config))
        inherited[key] = value;
    std::vector<std::string> environment;
    for (auto &[key, value] : inherited)
        environment.push_back(key + "=" + value);
    std::vector<char *> envp;
    for (auto &entry : environment)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<std::string> arguments{exe, "serve", "--config", file};
    std::vector<char *> argv;
    for (auto &argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0)
        throw Error(500, "process", "Cannot fork service");
    if (pid == 0) {
        if (::setsid() < 0)
            _exit(126);
        auto second = ::fork();
        if (second < 0)
            _exit(126);
        if (second > 0)
            _exit(0);
        int log = ::open(log_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0600);
        int input = ::open("/dev/null", O_RDONLY);
        if (log < 0 || input < 0)
            _exit(126);
        ::dup2(input, STDIN_FILENO);
        ::dup2(log, STDOUT_FILENO);
        ::dup2(log, STDERR_FILENO);
        if (input > 2)
            ::close(input);
        if (log > 2)
            ::close(log);
        ::execve(exe.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    int status;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        throw Error(500, "process", "Cannot wait for launcher");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw Error(500, "process", "Service launcher failed");
#endif
}
struct InstanceLock::Impl {
#ifdef _WIN32
    HANDLE handle{};
#else
    int fd = -1;
#endif
    explicit Impl(const fs::path &directory) {
        secure_directory(directory);
#ifdef _WIN32
        auto sid = user_sid();
        auto canonical = fs::weakly_canonical(directory).native();
        CharLowerBuffW(canonical.data(), static_cast<DWORD>(canonical.size()));
        auto identity = fs::path(canonical).u8string();
        std::string text(reinterpret_cast<const char *>(identity.data()), identity.size());
        auto name = L"Local\\ais2api-" + sid + L"-" + wide(sha256(text));
        auto sddl = L"D:P(A;;GA;;;" + sid + L")";
        PSECURITY_DESCRIPTOR descriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                  &descriptor, nullptr))
            throw Error(500, "permissions", "Cannot create lock ACL");
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        handle = CreateMutexW(&attributes, TRUE, name.c_str());
        auto error = GetLastError();
        LocalFree(descriptor);
        if (!handle)
            throw Error(500, "instance_lock", "Cannot acquire instance mutex");
        if (error == ERROR_ALREADY_EXISTS) {
            CloseHandle(handle);
            handle = nullptr;
            throw Error(409, "already_running", "An instance already owns this data directory");
        }
#else
        fd = ::open((directory / "instance.lock").c_str(),
                    O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            throw Error(500, "instance_lock", "Cannot open instance lock");
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
            throw Error(409, "already_running", "An instance already owns this data directory");
        }
#endif
    }
    ~Impl() {
#ifdef _WIN32
        if (handle) {
            ReleaseMutex(handle);
            CloseHandle(handle);
        }
#else
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
#endif
    }
};
InstanceLock::InstanceLock(const fs::path &d) : impl_(std::make_unique<Impl>(d)) {}
InstanceLock::~InstanceLock() = default;
} // namespace ais2api
