// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/console.hpp"
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <optional>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ais2api::console {
namespace {
std::mutex write_mutex;

std::string environment(const char *key) {
#ifdef _WIN32
    std::wstring wide;
    while (*key) wide.push_back(static_cast<wchar_t>(*key++));
    const auto *value = _wgetenv(wide.c_str());
    std::string text;
    // Only ASCII settings are meaningful here.
    for (; value && *value; ++value) text.push_back(*value < 128 ? static_cast<char>(*value) : '?');
    return text;
#else
    const auto *value = std::getenv(key);
    return value ? value : "";
#endif
}
#ifdef _WIN32
HANDLE handle(Stream stream) { return GetStdHandle(stream == Stream::out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE); }
#endif
bool interactive(Stream stream) {
#ifdef _WIN32
    DWORD mode = 0;
    const auto h = handle(stream);
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
#else
    return ::isatty(stream == Stream::out ? STDOUT_FILENO : STDERR_FILENO);
#endif
}
// ANSI colors for an interactive text console unless NO_COLOR is set.
bool colored(Stream stream) {
    static const auto out = [] {
        if (format(Stream::out) != Format::text || !environment("NO_COLOR").empty()) return false;
#ifdef _WIN32
        DWORD mode = 0;
        return GetConsoleMode(handle(Stream::out), &mode) &&
               SetConsoleMode(handle(Stream::out), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
        return true;
#endif
    }();
    static const auto err = [] {
        if (format(Stream::err) != Format::text || !environment("NO_COLOR").empty()) return false;
#ifdef _WIN32
        DWORD mode = 0;
        return GetConsoleMode(handle(Stream::err), &mode) &&
               SetConsoleMode(handle(Stream::err), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
        return true;
#endif
    }();
    return stream == Stream::out ? out : err;
}

std::string text_of(const json::object &record, std::string_view key) {
    const auto *value = record.if_contains(key);
    if (!value) return {};
    if (value->is_string()) return std::string(value->as_string());
    if (value->is_null()) return {};
    return json::serialize(*value);
}
std::int64_t integer(const json::object &record, std::string_view key) {
    const auto *value = record.if_contains(key);
    if (value && value->is_int64()) return value->as_int64();
    if (value && value->is_uint64()) return static_cast<std::int64_t>(value->as_uint64());
    if (value && value->is_double()) return static_cast<std::int64_t>(value->as_double());
    return 0;
}
bool flag(const json::object &record, std::string_view key) {
    const auto *value = record.if_contains(key);
    return value && value->is_bool() && value->as_bool();
}
std::string joined(const json::object &record, std::string_view key) {
    const auto *value = record.if_contains(key);
    if (!value || !value->is_array()) return text_of(record, key);
    std::string out;
    for (const auto &item : value->as_array()) {
        if (!out.empty()) out += ", ";
        out += item.is_string() ? std::string(item.as_string()) : json::serialize(item);
    }
    return out;
}
// Keeps a recognizable id prefix such as chatcmpl-1a2b3c4d….
std::string short_id(std::string id) {
    const auto split = id.find_first_of("-_");
    const auto start = split == std::string::npos ? 0 : split + 1;
    if (id.size() > start + 8) id = id.substr(0, start + 8) + "…";
    return id;
}
std::string duration(std::int64_t ms) {
    char buffer[32];
    if (ms < 1000) std::snprintf(buffer, sizeof buffer, "%lldms", static_cast<long long>(ms));
    else if (ms < 60000) std::snprintf(buffer, sizeof buffer, "%.1fs", static_cast<double>(ms) / 1000);
    else std::snprintf(buffer, sizeof buffer, "%lldm%02llds", static_cast<long long>(ms / 60000),
                       static_cast<long long>(ms / 1000 % 60));
    return buffer;
}
std::string size(std::int64_t bytes) {
    char buffer[32];
    if (bytes < 1024) std::snprintf(buffer, sizeof buffer, "%lld B", static_cast<long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(buffer, sizeof buffer, "%.1f KB", static_cast<double>(bytes) / 1024);
    else std::snprintf(buffer, sizeof buffer, "%.1f MB", static_cast<double>(bytes) / (1024 * 1024));
    return buffer;
}
std::string clock_text(std::int64_t seconds) {
    if (seconds <= 0) seconds = unix_seconds();
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time)) return "--:--:--";
#else
    if (!localtime_r(&time, &local)) return "--:--:--";
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%m-%d %H:%M:%S", &local);
    return buffer;
}
// Terminal cells: CJK and fullwidth characters take two.
std::size_t width(std::string_view text) {
    std::size_t cells = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead >> 5 == 6 ? 2 : lead >> 4 == 14 ? 3 : 4;
        std::uint32_t code = lead < 0x80 ? lead : lead & (0x3f >> (length - 1));
        for (std::size_t k = 1; k < length && i + k < text.size(); ++k)
            code = code << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        cells += code >= 0x2e80 && code <= 0xffdc ? 2 : 1;
        i += length;
    }
    return cells;
}
std::string account_problem(const std::string &kind) {
    if (kind == "rate_limit_minute") return "每分钟限额";
    if (kind == "rate_limit_day") return "每日限额";
    if (kind == "rate_limit_model") return "模型限额";
    if (kind == "permission_temporary") return "暂时无权限";
    if (kind == "login_expired") return "登录已过期";
    if (kind == "auth") return "身份校验失败";
    return kind;
}
std::string setup_stage(const std::string &stage) {
    if (stage == "system-check") return "检查系统";
    if (stage == "bundle-check") return "检查整合包";
    if (stage == "download") return "下载浏览器";
    if (stage == "extract") return "解压浏览器";
    if (stage == "verify") return "校验浏览器";
    if (stage == "complete") return "浏览器安装完成";
    return stage;
}
enum class Level { info, note, warning, error };
struct Line {
    Level level = Level::info;
    std::string title;
    std::vector<std::string> details;
    void add(std::string value) {
        if (!value.empty()) details.push_back(std::move(value));
    }
};
std::optional<Line> describe(const json::object &r) {
    const auto event = text_of(r, "event");
    const auto request = short_id(text_of(r, "request_id"));
    const auto account = text_of(r, "account_id").empty() ? "" : "账号 " + short_id(text_of(r, "account_id"));
    Line line;
    if (event == "listening") {
        line.title = "服务已启动";
        line.add("http://" + text_of(r, "bind") + ":" + text_of(r, "port"));
    } else if (event == "stopped") {
        line.title = "服务已停止";
    } else if (event == "http_request") {
        const auto status = integer(r, "status");
        if (status < 400) return std::nullopt;
        line.level = status >= 500 ? Level::error : Level::warning;
        line.title = "HTTP " + std::to_string(status);
        const auto category = text_of(r, "category");
        line.add(category == "management" ? "管理接口" : category == "http" ? "数据接口" : category);
        line.add(text_of(r, "error"));
    } else if (event == "generation") {
        const auto error = text_of(r, "error");
        const auto attempts = integer(r, "attempts");
        line.add(request);
        line.add(text_of(r, "model"));
        if (error.empty()) {
            line.title = "生成完成";
            if (flag(r, "stream")) line.add("流式");
            line.add("耗时 " + duration(integer(r, "generation_ms") + integer(r, "normalize_ms")));
            if (integer(r, "queue_ms") >= 100) line.add("排队 " + duration(integer(r, "queue_ms")));
            line.add("输出 " + size(integer(r, "event_bytes")));
            if (attempts > 1) line.add(std::to_string(attempts) + " 次尝试");
        } else if (error == "cancelled") {
            line.level = Level::warning;
            line.title = "生成已取消";
            line.add("耗时 " + duration(integer(r, "generation_ms") + integer(r, "normalize_ms")));
        } else {
            line.level = Level::error;
            line.title = "生成失败";
            line.add(error);
            if (attempts > 1) line.add(std::to_string(attempts) + " 次尝试");
        }
    } else if (event == "generation_attempt") {
        const auto attempt = integer(r, "attempt");
        line.level = attempt > 1 ? Level::warning : Level::info;
        line.title = attempt > 1 ? "重试生成" : "生成开始";
        line.add(request);
        line.add(account);
        if (attempt > 1) line.add("第 " + std::to_string(attempt) + " 次");
    } else if (event == "generation_parameters_ignored") {
        line.level = Level::note;
        line.title = "已忽略参数";
        line.add(request);
        line.add(joined(r, "parameters"));
    } else if (event == "upstream_fields_ignored") {
        line.level = Level::note;
        line.title = "已跳过上游字段";
        line.add(request);
        line.add(joined(r, "locations"));
    } else if (event == "template_refresh") {
        line.level = Level::warning;
        line.title = "刷新请求模板";
        line.add(request);
        line.add(account);
        line.add(text_of(r, "code"));
    } else if (event == "session_recovery") {
        line.level = Level::warning;
        line.title = text_of(r, "action") == "restart_browser" ? "重启浏览器" : "重建浏览器会话";
        line.add(request);
        line.add(account);
        line.add(text_of(r, "code"));
    } else if (event == "account_failure") {
        line.level = Level::warning;
        line.title = "账号暂不可用";
        line.add(request);
        line.add(account);
        line.add(account_problem(text_of(r, "kind")));
        line.add(text_of(r, "code"));
    } else if (event == "browser_failure") {
        line.level = Level::error;
        line.title = "浏览器请求失败";
        line.add(request);
        line.add(text_of(r, "code"));
        if (!text_of(r, "phase").empty()) line.add("阶段 " + text_of(r, "phase"));
        if (integer(r, "status")) line.add("HTTP " + text_of(r, "status"));
    } else if (event == "cookie_import") {
        line.title = "Cookie 导入完成";
        line.add(text_of(r, "imported_count") + " 个账号");
        line.add("耗时 " + duration(integer(r, "elapsed_ms")));
    } else if (event == "cookie_import_failed") {
        line.level = Level::error;
        line.title = "Cookie 导入失败";
        line.add(text_of(r, "reason"));
        if (integer(r, "status")) line.add("HTTP " + text_of(r, "status"));
        line.add("耗时 " + duration(integer(r, "elapsed_ms")));
    } else if (event == "config_saved") {
        line.title = "配置已保存";
        line.add(flag(r, "restart_required") ? "重启后生效" : "已生效");
    } else if (event == "admin_password_changed") {
        line.title = "管理密码已修改";
    } else if (event == "browser_idle_released") {
        line.title = "浏览器已空闲回收";
        line.add("空闲 " + duration(integer(r, "idle_seconds") * 1000));
    } else if (event == "maintenance_started") {
        const auto action = text_of(r, "action");
        line.title = action == "models.list" ? "开始获取模型列表" : action == "accounts.probe" ? "开始探测账号" : "后台任务开始";
        if (line.title == "后台任务开始") line.add(action);
        line.add(short_id(text_of(r, "operation_id")));
    } else if (event == "maintenance_done") {
        line.title = "后台任务完成";
        line.add(short_id(text_of(r, "operation_id")));
    } else if (event == "maintenance_failed") {
        line.level = Level::error;
        line.title = "后台任务失败";
        line.add(short_id(text_of(r, "operation_id")));
        line.add(text_of(r, "code"));
    } else if (event == "output_limit_reached") {
        line.level = Level::warning;
        line.title = "输出达到上游单次上限，按 MAX_TOKENS 结束";
        line.add(text_of(r, "limit") + " token");
        line.add(short_id(text_of(r, "request_id")));
    } else if (event == "model_discovery_scheduled") {
        line.title = "导入的账号将获取模型列表";
        line.add(text_of(r, "account_id"));
        line.add(short_id(text_of(r, "operation_id")));
    } else if (event == "model_discovery_not_scheduled") {
        line.level = Level::warning;
        line.title = "未能安排获取模型列表";
        line.add(text_of(r, "reason"));
    } else if (event == "journal_rotated") {
        line.title = "日志已轮转";
    } else if (event == "setup_stage") {
        line.title = setup_stage(text_of(r, "stage"));
        line.add(text_of(r, "browser_version"));
        line.add(text_of(r, "platform"));
    } else if (event == "setup_download") {
        const auto received = integer(r, "received_bytes"), total = integer(r, "maximum_bytes");
        line.title = "下载中";
        std::string progress = size(received);
        if (total > 0) {
            char percent[16];
            std::snprintf(percent, sizeof percent, "%.0f%%", 100.0 * static_cast<double>(received) / static_cast<double>(total));
            progress += " / " + size(total) + "  " + percent;
        }
        line.add(progress);
    } else {
        line.title = event.empty() ? "日志" : event;
        for (const auto &kv : r)
            if (kv.key() != "event" && kv.key() != "timestamp" && kv.key() != "id")
                line.add(std::string(kv.key()) + "=" + text_of(r, kv.key()));
    }
    return line;
}
void pretty_into(std::string &out, const json::value &value, std::size_t depth) {
    const std::string indent((depth + 1) * 2, ' '), closing(depth * 2, ' ');
    if (value.is_object() && !value.as_object().empty()) {
        out += "{\n";
        bool first = true;
        for (const auto &kv : value.as_object()) {
            if (!first) out += ",\n";
            first = false;
            out += indent + json::serialize(json::value(kv.key())) + ": ";
            pretty_into(out, kv.value(), depth + 1);
        }
        out += "\n" + closing + "}";
    } else if (value.is_array() && !value.as_array().empty()) {
        out += "[\n";
        bool first = true;
        for (const auto &item : value.as_array()) {
            if (!first) out += ",\n";
            first = false;
            out += indent;
            pretty_into(out, item, depth + 1);
        }
        out += "\n" + closing + "]";
    } else
        out += json::serialize(value);
}
} // namespace

Format choose(std::string_view setting, bool interactive) {
    if (setting == "json") return Format::json;
    if (setting == "text") return Format::text;
    return interactive ? Format::text : Format::json;
}
Format format(Stream stream) {
    static const auto setting = environment("AIS2API_CONSOLE_FORMAT");
    static const auto out = choose(setting, interactive(Stream::out));
    static const auto err = choose(setting, interactive(Stream::err));
    return stream == Stream::out ? out : err;
}
void write(Stream stream, std::string_view utf8) {
    if (utf8.empty()) return;
    std::lock_guard lock(write_mutex);
    auto &standard = stream == Stream::out ? std::cout : std::cerr;
    standard.flush();
#ifdef _WIN32
    if (interactive(stream)) {
        const auto count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
        for (std::size_t offset = 0; offset < wide.size();) {
            DWORD written = 0;
            const auto part = static_cast<DWORD>(std::min<std::size_t>(wide.size() - offset, 16384));
            if (!WriteConsoleW(handle(stream), wide.data() + offset, part, &written, nullptr) || !written) return;
            offset += written;
        }
        return;
    }
#endif
    standard.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    standard.flush();
}
std::string log_line(const json::object &record, bool color) {
    auto line = describe(record);
    if (!line) return {};
    static constexpr const char *labels[] = {"信息", "提示", "警告", "错误"};
    static constexpr const char *colors[] = {"\x1b[32m", "\x1b[36m", "\x1b[33m", "\x1b[31m"};
    const auto level = static_cast<std::size_t>(line->level);
    std::string out;
    if (color) out += "\x1b[90m";
    out += clock_text(integer(record, "timestamp"));
    out += color ? "\x1b[0m " : " ";
    if (color) out += colors[level];
    out += labels[level];
    if (color) out += "\x1b[0m";
    out += " ";
    if (color && line->level == Level::error) out += "\x1b[1m";
    out += line->title;
    if (color && line->level == Level::error) out += "\x1b[0m";
    if (!line->details.empty())
        for (auto cells = width(line->title); cells < 14; ++cells) out += ' ';
    for (const auto &detail : line->details) out += "  " + detail;
    out += '\n';
    return out;
}
std::string pretty(const json::value &value) {
    std::string out;
    pretty_into(out, value, 0);
    return out;
}
void emit_records(const std::vector<json::object> &records, std::string_view json_lines) {
    if (format(Stream::err) == Format::json) return write(Stream::err, json_lines);
    std::string text;
    for (const auto &record : records) text += log_line(record, colored(Stream::err));
    write(Stream::err, text);
}
void emit_record(const json::object &record) {
    emit_records({record}, json::serialize(record) + '\n');
}
void emit_result(const json::value &result) {
    write(Stream::out, (format(Stream::out) == Format::text ? pretty(result) : json::serialize(result)) + '\n');
}
void emit_error(const json::object &error) {
    if (format(Stream::err) == Format::json) return write(Stream::err, json::serialize(error) + '\n');
    const auto *detail = error.if_contains("error");
    const auto &e = detail && detail->is_object() ? detail->as_object() : error;
    std::string text = colored(Stream::err) ? "\x1b[31m错误\x1b[0m：" : "错误：";
    text += text_of(e, "message").empty() ? json::serialize(error) : text_of(e, "message");
    std::string context = text_of(e, "reason");
    if (integer(e, "code")) context += (context.empty() ? "HTTP " : "，HTTP ") + text_of(e, "code");
    if (!context.empty()) text += "（" + context + "）";
    write(Stream::err, text + '\n');
}
} // namespace ais2api::console
