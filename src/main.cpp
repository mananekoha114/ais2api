// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/management.hpp"
#include "ais2api/console.hpp"
#include <array>
#include <charconv>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ais2api {
namespace {
struct Arguments {
    fs::path config_file;
    json::object overrides;
    std::vector<std::string> positional;
    std::optional<fs::path> import_file;
    std::string name;
    bool stdin_import = false;
    bool yes = false;
    bool all = false;
    bool help = false;
};

[[noreturn]] void usage_error(std::string text) {
    throw Error(400, "invalid_command", std::move(text));
}

std::uint64_t unsigned_integer(std::string_view value) {
    std::uint64_t result{};
    auto converted = std::from_chars(value.data(), value.data() + value.size(), result);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size())
        usage_error("Expected an unsigned integer option value");
    return result;
}

Arguments parse_arguments(int argc, char **argv) {
    Arguments result;
    const std::map<std::string, std::string> string_options{{"--data-dir", "data_dir"},
                                                            {"--bind", "bind"},
                                                            {"--browser", "browser_executable"},
                                                            {"--proxy", "proxy"}};
    const std::map<std::string, std::string> limit_options{
        {"--request-bytes", "request_bytes"},
        {"--object-bytes", "object_bytes"},
        {"--relay-bytes", "relay_bytes"},
        {"--queue-size", "queue_size"},
        {"--queue-seconds", "queue_seconds"},
        {"--generation-seconds", "generation_seconds"},
        {"--stream-idle-seconds", "stream_idle_seconds"},
        {"--max-attempts", "max_attempts"}};
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            result.help = true;
            continue;
        }
        if (!arg.starts_with("--")) {
            result.positional.push_back(std::move(arg));
            continue;
        }
        auto equals = arg.find('=');
        std::string option = arg.substr(0, equals);
        if (option == "--yes" || option == "--stdin" || option == "--all") {
            if (equals != std::string::npos)
                usage_error("Boolean switches do not take a value");
            if (option == "--yes")
                result.yes = true;
            else if (option == "--stdin")
                result.stdin_import = true;
            else
                result.all = true;
            continue;
        }
        std::string value;
        if (equals != std::string::npos)
            value = arg.substr(equals + 1);
        else {
            if (++i >= argc || std::string_view(argv[i]).starts_with("--"))
                usage_error("Option requires a value: " + option);
            value = argv[i];
        }
        if (option == "--config")
            result.config_file = fs::u8path(value);
        else if (option == "--file")
            result.import_file = fs::u8path(value);
        else if (option == "--name")
            result.name = value;
        else if (auto found = string_options.find(option); found != string_options.end())
            result.overrides[found->second] = value;
        else if (option == "--port")
            result.overrides["port"] = unsigned_integer(value);
        else if (option == "--browser-idle-seconds")
            result.overrides["browser_idle_seconds"] = unsigned_integer(value);
        else if (option == "--headless" || option == "--browser-proxy-relay") {
            if (value != "true" && value != "false")
                usage_error(option + " requires true or false");
            result.overrides[option == "--headless" ? "headless" : "browser_proxy_relay"] = value == "true";
        } else if (auto found = limit_options.find(option); found != limit_options.end()) {
            if (!result.overrides.if_contains("limits"))
                result.overrides["limits"] = json::object{};
            result.overrides["limits"].as_object()[found->second] = unsigned_integer(value);
        } else
            usage_error("Unknown option: " + option);
    }
    return result;
}

void help() {
    std::cout << "ais2api: Chromium/CDP Gemini and OpenAI gateway\n\n"
                 "Commands:\n"
                 "  init | serve | start | stop | status | setup | doctor\n"
                 "  browser update [BACKEND] | browser rollback [BACKEND]\n"
                 "  accounts list | import (--file PATH | --stdin) [--name NAME]\n"
                 "  accounts select ID | remove ID | reset ID | probe (ID | all)\n"
                 "  accounts backend (ID | --all) (cloakbrowser | auto | camoufox | spidermonkey) [--yes]\n"
                 "  keys list (edit api_key in config.json to set or rotate)\n"
                 "  stats | diagnostics | models list | config show | config validate | config reload\n"
                 "  operations status ID\n\n"
                 "Global options: --config PATH --data-dir PATH --bind ADDRESS --port PORT\n"
                 "  --browser PATH --proxy URL --browser-proxy-relay=true|false\n"
                 "  --headless=true|false --browser-idle-seconds N\n"
                 "  --request-bytes N --object-bytes N --relay-bytes N --queue-size N\n"
                 "  --queue-seconds N --generation-seconds N --stream-idle-seconds N\n"
                 "  --max-attempts N\n"
                 "setup/browser update accept --yes to approve required system package changes;\n"
                 "accounts backend needs --yes to move accounts between CloakBrowser and Firefox.\n"
                 "All non-help results are JSON. Set api_key in config.json; secrets are never printed.\n";
}

void require_offline(const Config &config) {
    try {
        (void)local_call(config, {{"command", "status"}});
    } catch (const Error &error) {
        if (error.code == "not_running")
            return;
        throw;
    }
    throw Error(409, "already_running", "Stop the running service before this operation");
}

json::object wait_operation(const Config &config, json::object result) {
    auto id = string_or(result, "operation_id");
    if (id.empty())
        return result;
    auto deadline = Clock::now() + std::chrono::seconds(120);
    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        result = local_call(config, {{"command", "operation.status"}, {"id", id}});
        auto status = string_or(result, "status");
        if (status == "done") {
            auto payload = result.if_contains("result");
            if (!payload || !payload->is_object())
                throw Error(502, "control_protocol", "Operation returned an invalid result");
            return payload->as_object();
        }
        if (status == "failed") {
            auto payload = result.if_contains("error");
            if (!payload || !payload->is_object())
                throw Error(500, "operation_failed", "Background operation failed");
            auto &error = payload->as_object();
            int code = 500;
            if (auto p = error.if_contains("status"); p && p->is_int64())
                code = static_cast<int>(p->as_int64());
            throw Error(code, string_or(error, "code", "operation_failed"),
                        string_or(error, "message", "Background operation failed"));
        }
    }
    throw Error(504, "operation_pending",
                "Operation " + id + " is still pending; use operations status " + id);
}

json::object dispatch(Config &config, const json::object &command, bool online_only = false) {
    try {
        return wait_operation(config, local_call(config, command));
    } catch (const Error &error) {
        if (error.code != "not_running")
            throw;
        if (online_only) {
            if (string_or(command, "command") == "models.list" && !config.configured_models.empty())
                return {{"models", config.configured_models}, {"source", "configured"}};
            throw Error(503, "not_running",
                        "Start the service with start or serve before this command");
        }
    }
    // Lock acquisition closes the race with a service starting after local_call.
    InstanceLock lock(config.data_dir);
    Store store(config.data_dir);
    return manage(store, config, command);
}

json::value import_input(const Arguments &args, const Config &config) {
    if (args.import_file.has_value() == args.stdin_import)
        usage_error("accounts import requires exactly one of --file PATH or --stdin");
    std::string text;
    if (args.import_file)
        text = read_file(*args.import_file, config.limits.request_bytes);
    else {
        std::array<char, 16384> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > config.limits.request_bytes - text.size())
                throw Error(413, "input_too_large",
                            "Account input exceeds configured request limit");
            text.append(buffer.data(), count);
        }
        if (!std::cin.eof())
            throw Error(400, "input_read", "Cannot read account input");
    }
    if (text.empty())
        usage_error("Account input is empty");
    boost::system::error_code error;
    auto value = json::parse(text, error);
    return error ? json::value(text) : value;
}

json::object execute(Arguments args) {
    Config config = Config::load(args.config_file, args.overrides);
    auto &p = args.positional;
    if (p.empty())
        usage_error("A command is required; use --help");
    const auto &command = p[0];
    auto count = [&](std::size_t minimum, std::size_t maximum) {
        if (p.size() < minimum || p.size() > maximum)
            usage_error("Unexpected command arguments");
    };
    if ((args.import_file || args.stdin_import) &&
        !(command == "accounts" && p.size() > 1 && p[1] == "import"))
        usage_error("--file and --stdin are only valid for accounts import");
    const bool account_backend = command == "accounts" && p.size() > 1 && p[1] == "backend";
    if (args.all && !(command == "accounts" && p.size() > 1 && p[1] == "probe") && !account_backend)
        usage_error("--all is only valid for accounts probe or accounts backend");
    if (!args.name.empty() && !(p.size() > 1 && ((command == "accounts" && p[1] == "import") ||
                                                 (command == "keys" && p[1] == "create"))))
        usage_error("--name is only valid for account import or key creation");
    if (args.yes &&
        !(command == "setup" || (command == "browser" && p.size() > 1 && p[1] == "update") || account_backend))
        usage_error("--yes is only valid for setup, browser update or accounts backend");
    if (command == "serve") {
        count(1, 1);
        auto result = serve(config);
        if (result != 0)
            throw Error(500, "service_failed", "Service stopped with a failure");
        return {{"stopped", true}};
    }
    if (command == "status") {
        count(1, 1);
        try {
            return local_call(config, {{"command", "status"}});
        } catch (const Error &error) {
            if (error.code != "not_running")
                throw;
            return {{"running", false}, {"data_dir", path_utf8(config.data_dir)}};
        }
    }
    if (command == "stop") {
        count(1, 1);
        return local_call(config, {{"command", "stop"}});
    }
    if (command == "start") {
        count(1, 1);
        try {
            return local_call(config, {{"command", "status"}});
        } catch (const Error &error) {
            if (error.code != "not_running")
                throw;
        }
        spawn_service(config);
        auto deadline = Clock::now() + std::chrono::seconds(5);
        do {
            try {
                return local_call(config, {{"command", "status"}});
            } catch (const Error &error) {
                if (error.code != "not_running")
                    throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (Clock::now() < deadline);
        throw Error(504, "start_timeout",
                    "Service did not become ready within five seconds; inspect service.log");
    }
    if (command == "init") {
        count(1, 1);
        require_offline(config);
        InstanceLock lock(config.data_dir);
        const auto marker = config.data_dir / "initialized.json";
        const bool initialized = fs::exists(marker);
        Store store(config.data_dir);
        if (!fs::exists(config.config_file))
            config.save();
        json::object result{{"initialized", true},
                            {"config_file", path_utf8(config.config_file)},
                            {"data_dir", path_utf8(config.data_dir)},
                            {"api_key_configured", !config.api_key.empty()},
                            {"next_step", config.api_key.empty()
                                ? "Set api_key in config.json before using generation or model endpoints"
                                : "API key is configured in config.json"}};
        // "admin" is public, so reporting that it is still active reveals nothing.
        result["admin_password_default"] = config.admin_key_is_default();
        if (config.admin_key_is_default())
            result["management_next_step"] = "Start the service and open http://127.0.0.1:" +
                std::to_string(config.port) + "/ on this machine or a local network; sign in with the "
                "default management password 'admin' and set a new password when prompted";
        if (!initialized)
            atomic_write(marker, "{\"version\":1}", true);
        return result;
    }
    if (command == "doctor") {
        count(1, 1);
        return doctor(config);
    }
    if (command == "setup" || command == "browser") {
        std::string backend;
        if (command == "setup")
            count(1, 1);
        else {
            count(2, 3);
            if (p[1] != "update" && p[1] != "rollback")
                usage_error("Unknown browser command");
            if (p.size() == 3) {
                backend = p[2];
                if (backend != "cloakbrowser" && backend != "camoufox")
                    usage_error("The browser backend must be cloakbrowser or camoufox");
            }
        }
        require_offline(config);
        InstanceLock lock(config.data_dir);
        if (command == "browser" && p[1] == "rollback")
            return rollback_runtime(config, Config::default_directory(), backend);
        return setup_runtime(config, args.yes, backend);
    }
    if (command == "accounts") {
        count(2, 4);
        const auto &action = p[1];
        json::object request{{"command", "accounts." + action}};
        if (action == "import") {
            count(2, 2);
            request["input"] = import_input(args, config);
            if (!args.name.empty())
                request["name"] = args.name;
        } else if (action == "list")
            count(2, 2);
        else if (action == "probe") {
            if (args.all && p.size() == 3)
                usage_error("Use either --all or a probe id");
            if (args.all || (p.size() == 3 && p[2] == "all"))
                request["all"] = true;
            else {
                count(3, 3);
                request["id"] = p[2];
            }
            return dispatch(config, request, true);
        } else if (action == "select" || action == "remove" || action == "reset") {
            count(3, 3);
            request["id"] = p[2];
        } else if (action == "backend") {
            // accounts backend ID VALUE, or accounts backend --all VALUE
            if (args.all) {
                count(3, 3);
                request = {{"command", "accounts.backend.all"}, {"backend", p[2]}};
            } else {
                count(4, 4);
                request = {{"command", "accounts.fingerprint.update"}, {"id", p[2]}, {"backend", p[3]}};
            }
            request["confirm_identity_change"] = args.yes;
        } else
            usage_error("Unknown accounts command");
        return dispatch(config, request);
    }
    if (command == "keys") {
        count(2, 3);
        const auto &action = p[1];
        json::object request{{"command", "keys." + action}};
        if (action == "create") {
            if (p.size() == 3 && !args.name.empty())
                usage_error("Specify the key name once");
            request["name"] = p.size() == 3 ? p[2] : (args.name.empty() ? "API key" : args.name);
        } else if (action == "list")
            count(2, 2);
        else if (action == "revoke") {
            count(3, 3);
            request["id"] = p[2];
        } else
            usage_error("Unknown keys command");
        return dispatch(config, request);
    }
    if (command == "stats") {
        count(1, 1);
        return dispatch(config, {{"command", "stats"}});
    }
    if (command == "diagnostics") {
        count(1, 1);
        return dispatch(config, {{"command", "diagnostics.report"}});
    }
    if (command == "config") {
        count(2, 2);
        if (p[1] != "show" && p[1] != "validate" && p[1] != "reload")
            usage_error("Unknown config command");
        return dispatch(config, {{"command", "config." + p[1]}}, p[1] == "reload");
    }
    if (command == "models") {
        count(2, 2);
        if (p[1] != "list")
            usage_error("Unknown models command");
        return dispatch(config, {{"command", "models.list"}}, true);
    }
    if (command == "operations") {
        count(3, 3);
        if (p[1] != "status")
            usage_error("Unknown operations command");
        return local_call(config, {{"command", "operation.status"}, {"id", p[2]}});
    }
    usage_error("Unknown command; use --help");
}
} // namespace
} // namespace ais2api

int cli_main(int argc, char **argv) {
    try {
        auto args = ais2api::parse_arguments(argc, argv);
        if (args.help || (!args.positional.empty() && args.positional[0] == "help")) {
            ais2api::help();
            return 0;
        }
        ais2api::console::emit_result(ais2api::execute(std::move(args)));
        return 0;
    } catch (const ais2api::Error &error) {
        ais2api::console::emit_error(ais2api::error_json(error));
        return 1;
    } catch (const std::exception &) {
        // Third-party exceptions may contain request or credential text.
        ais2api::console::emit_error(ais2api::error_json(
            ais2api::Error(500, "internal", "Command failed unexpectedly")));
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    try {
        std::vector<std::string> text;
        text.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0,
                                             nullptr, nullptr);
            if (!count)
                throw ais2api::Error(400, "invalid_command", "Invalid Unicode argument");
            std::string value(static_cast<std::size_t>(count), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(),
                                     count, nullptr, nullptr))
                throw ais2api::Error(400, "invalid_command", "Cannot decode Unicode argument");
            value.pop_back();
            text.push_back(std::move(value));
        }
        std::vector<char *> pointers;
        for (auto &value : text)
            pointers.push_back(value.data());
        return cli_main(argc, pointers.data());
    } catch (const ais2api::Error &error) {
        ais2api::console::emit_error(ais2api::error_json(error));
        return 1;
    } catch (const std::exception &) {
        ais2api::console::emit_error(ais2api::error_json(
            ais2api::Error(500, "internal", "Cannot initialize command arguments")));
        return 1;
    }
}
#else
int main(int argc, char **argv) { return cli_main(argc, argv); }
#endif
