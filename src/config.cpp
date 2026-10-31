// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/config.hpp"
#include "ais2api/runtime.hpp"
#include "ais2api/bundle.hpp"
#include "ais2api/proxy.hpp"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <openssl/crypto.h>
#include <boost/asio/ip/address.hpp>
#include <cctype>

namespace ais2api {
namespace {
void merge(json::object &a, const json::object &b) {
  for (auto &v : b) {
    if (v.value().is_object() && a.if_contains(v.key()) &&
        a.at(v.key()).is_object())
      merge(a.at(v.key()).as_object(), v.value().as_object());
    else
      a[v.key()] = v.value();
  }
}
std::uint64_t number(const json::value &v) {
  if (v.is_uint64())
    return v.as_uint64();
  if (v.is_int64() && v.as_int64() >= 0)
    return static_cast<std::uint64_t>(v.as_int64());
  throw Error(400, "invalid_config", "Expected unsigned integer");
}
void fields(const json::object &o,
            std::initializer_list<std::string_view> allowed) {
  for (auto &p : o) {
    bool ok = false;
    for (auto k : allowed)
      if (p.key() == k)
        ok = true;
    if (!ok)
      throw Error(400, "invalid_config",
                  "Unknown configuration field: " + std::string(p.key()));
  }
}
bool env_exists(const char *key) {
#ifdef _WIN32
  std::wstring wide_key;
  while (*key)
    wide_key.push_back(static_cast<wchar_t>(*key++));
  return _wgetenv(wide_key.c_str()) != nullptr;
#else
  return std::getenv(key) != nullptr;
#endif
}
std::string path_text(const fs::path &path) {
  auto text = path.u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}
std::string env(const char *k) {
#ifdef _WIN32
  std::wstring key;
  while (*k)
    key.push_back(static_cast<wchar_t>(*k++));
  auto value = _wgetenv(key.c_str());
  return value ? path_text(fs::path(value)) : "";
#else
  auto p = std::getenv(k);
  return p ? p : "";
#endif
}
fs::path absolute_path(const fs::path &path, const fs::path &base) {
  return (path.is_absolute() ? path : base / path).lexically_normal();
}
std::string stored_path(const fs::path &path, const fs::path &config_file, bool portable) {
  if (!portable || path.empty() || config_file.empty())
    return path_text(path);
  auto relative = fs::absolute(path).lexically_normal().lexically_relative(
      fs::absolute(config_file).parent_path().lexically_normal());
  if (!relative.empty() && *relative.begin() != "..")
    return path_text(relative);
  return path_text(path);
}
void append_json(std::string &out, const json::value &value, std::size_t depth) {
  if (value.is_object()) {
    const auto &object = value.as_object();
    out += '{';
    std::size_t index = 0;
    for (const auto &entry : object) {
      out += '\n';
      out.append((depth + 1) * 2, ' ');
      out += json::serialize(json::value(entry.key()));
      out += ": ";
      append_json(out, entry.value(), depth + 1);
      if (++index < object.size())
        out += ',';
    }
    if (!object.empty()) {
      out += '\n';
      out.append(depth * 2, ' ');
    }
    out += '}';
  } else if (value.is_array()) {
    const auto &array = value.as_array();
    out += '[';
    for (std::size_t index = 0; index < array.size(); ++index) {
      out += '\n';
      out.append((depth + 1) * 2, ' ');
      append_json(out, array[index], depth + 1);
      if (index + 1 < array.size())
        out += ',';
    }
    if (!array.empty()) {
      out += '\n';
      out.append(depth * 2, ' ');
    }
    out += ']';
  } else {
    out += json::serialize(value);
  }
}
std::string formatted_config(const json::object &object) {
  std::string out;
  append_json(out, object, 0);
  out += '\n';
  return out;
}
} // namespace
fs::path Config::default_directory() {
  return executable_path().parent_path();
}
json::object Config::to_json(bool portable_paths) const {
  return {{"version", version},
          {"bind", bind},
          {"port", port},
          {"api_key", api_key},
          {"admin_key", admin_key},
          {"admin_origins", admin_origins},
          {"cookie_import_key", cookie_import_key},
          {"cookie_import_max_accounts", cookie_import_max_accounts},
          {"cookie_import_timeout_seconds", cookie_import_timeout_seconds},
          {"data_dir", stored_path(data_dir, config_file, portable_paths)},
          {"browser_executable", stored_path(browser_executable, config_file, portable_paths)},
          {"proxy", proxy},
          {"browser_proxy_relay", browser_proxy_relay},
          {"browser_remote_allow_origins", browser_remote_allow_origins},
          {"browser_site_isolation", browser_site_isolation},
          {"headless", headless},
          {"ignore_unsupported_parameters", ignore_unsupported_parameters},
          {"search_model_drop_functions", search_model_drop_functions},
          {"browser_idle_seconds", browser_idle_seconds},
          {"request_retention_days", request_retention_days},
          {"limits",
           {{"request_bytes", limits.request_bytes},
            {"object_bytes", limits.object_bytes},
            {"relay_bytes", limits.relay_bytes},
            {"queue_size", limits.queue_size},
            {"queue_seconds", limits.queue_seconds},
            {"generation_seconds", limits.generation_seconds},
            {"stream_idle_seconds", limits.stream_idle_seconds},
            {"max_attempts", limits.max_attempts}}},
          {"model_aliases", model_aliases},
          {"configured_models", configured_models},
          {"model_defaults", model_defaults}};
}
void Config::save() const {
  validate();
  atomic_write(config_file, formatted_config(to_json(true)), true);
}
static Config parse_config(Config c, json::object o, bool explicit_browser);
Config Config::load(const fs::path &file, const json::object &overrides) {
  Config c;
  c.config_file = fs::absolute(file.empty() ? default_directory() / "config.json" : file).lexically_normal();
  c.data_dir = default_directory() / "data";
  // A brand-new installation starts with the well-known management password;
  // the service forces it to be replaced on first sign-in. Existing files keep
  // their stored value, including an empty key that disables management.
  if (!fs::exists(c.config_file))
    c.admin_key = std::string(default_admin_key);
  bool explicit_browser = false;
  auto o = c.to_json();
  std::optional<json::object> upgraded;
  std::string original;
  if (fs::exists(c.config_file)) {
    original = read_file(c.config_file);
    auto v = json::parse(original);
    if (!v.is_object())
      throw Error(400, "invalid_config", "Configuration must be an object");
    // Fill missing fields recursively without changing stored paths or values.
    // Keep this separate from runtime settings so temporary overrides stay out
    // of the file, and browser auto-detection is repeated after relocation.
    upgraded = c.to_json(true);
    merge(*upgraded, v.as_object());
    if (auto browser = v.as_object().if_contains("browser_executable"))
      explicit_browser = !browser->is_string() || !browser->as_string().empty();
    // File paths belong to the configuration directory, regardless of the
    // shell's working directory or the detached service's startup location.
    for (auto key : {"data_dir", "browser_executable"})
      if (auto value = v.as_object().if_contains(key);
          value && value->is_string() && !value->as_string().empty())
        *value = path_text(absolute_path(fs::u8path(std::string(value->as_string())),
                                        c.config_file.parent_path()));
    merge(o, v.as_object());
    // Never rewrite an invalid file, even when an override could mask it.
    (void)parse_config(c, o, explicit_browser);
  }
  json::object e;
  for (auto pair : {std::pair{"AIS2API_ADMIN_KEY", "admin_key"},
                    {"AIS2API_BIND", "bind"},
                    {"AIS2API_DATA_DIR", "data_dir"},
                    {"AIS2API_BROWSER_EXECUTABLE", "browser_executable"},
                    {"AIS2API_PROXY", "proxy"}}) {
    auto v = env(pair.first);
    if (env_exists(pair.first))
      e[pair.second] = v;
  }
  for (auto pair :
       {std::pair{"AIS2API_PORT", "port"}, {"AIS2API_HEADLESS", "headless"},
        {"AIS2API_BROWSER_PROXY_RELAY", "browser_proxy_relay"},
        {"AIS2API_BROWSER_REMOTE_ALLOW_ORIGINS", "browser_remote_allow_origins"},
        {"AIS2API_BROWSER_SITE_ISOLATION", "browser_site_isolation"},
        {"AIS2API_IGNORE_UNSUPPORTED_PARAMETERS", "ignore_unsupported_parameters"},
        {"AIS2API_SEARCH_MODEL_DROP_FUNCTIONS", "search_model_drop_functions"},
        {"AIS2API_BROWSER_IDLE_SECONDS", "browser_idle_seconds"},
        {"AIS2API_REQUEST_RETENTION_DAYS", "request_retention_days"}}) {
    auto v = env(pair.first);
    if (env_exists(pair.first)) {
      try {
        e[pair.second] = json::parse(v);
      } catch (...) {
        throw Error(400, "invalid_config", "Invalid environment value");
      }
    }
  }
  for (auto pair : {std::pair{"AIS2API_ADMIN_ORIGINS", "admin_origins"},
                    {"AIS2API_LIMITS", "limits"},
                    {"AIS2API_MODEL_ALIASES", "model_aliases"},
                    {"AIS2API_CONFIGURED_MODELS", "configured_models"},
                    {"AIS2API_MODEL_DEFAULTS", "model_defaults"}}) {
    auto value = env(pair.first);
    if (env_exists(pair.first)) {
      try {
        e[pair.second] = json::parse(value);
      } catch (...) {
        throw Error(400, "invalid_config", "Invalid JSON environment value");
      }
    }
  }
  json::object limit_env;
  for (auto pair : {std::pair{"AIS2API_REQUEST_BYTES", "request_bytes"},
                    {"AIS2API_OBJECT_BYTES", "object_bytes"},
                    {"AIS2API_RELAY_BYTES", "relay_bytes"},
                    {"AIS2API_QUEUE_SIZE", "queue_size"},
                    {"AIS2API_QUEUE_SECONDS", "queue_seconds"},
                    {"AIS2API_GENERATION_SECONDS", "generation_seconds"},
                    {"AIS2API_STREAM_IDLE_SECONDS", "stream_idle_seconds"},
                    {"AIS2API_MAX_ATTEMPTS", "max_attempts"}}) {
    auto value = env(pair.first);
    if (env_exists(pair.first)) {
      try {
        limit_env[pair.second] = json::parse(value);
      } catch (...) {
        throw Error(400, "invalid_config", "Invalid numeric environment value");
      }
    }
  }
  if (!limit_env.empty()) {
    if (!e.if_contains("limits"))
      e["limits"] = json::object{};
    if (!e.at("limits").is_object())
      throw Error(400, "invalid_config", "AIS2API_LIMITS must be an object");
    merge(e.at("limits").as_object(), limit_env);
  }
  merge(o, e);
  merge(o, overrides);
  explicit_browser = explicit_browser || e.contains("browser_executable") ||
                     overrides.contains("browser_executable");
  auto result = parse_config(std::move(c), std::move(o), explicit_browser);
  if (upgraded) {
    const auto formatted = formatted_config(*upgraded);
    if (formatted != original)
      atomic_write(result.config_file, formatted, true);
  }
  return result;
}
static Config parse_config(Config c, json::object o, bool explicit_browser) {
  fields(o, {"version", "bind", "port", "api_key", "admin_key", "admin_origins", "cookie_import_key",
             "cookie_import_max_accounts", "cookie_import_timeout_seconds", "data_dir", "browser_executable",
             "proxy", "browser_proxy_relay", "browser_remote_allow_origins", "browser_site_isolation", "headless", "ignore_unsupported_parameters", "search_model_drop_functions", "browser_idle_seconds", "limits", "model_aliases",
             "configured_models", "model_defaults", "request_retention_days"});
  auto text = [&](const char *k) {
    if (!o.at(k).is_string())
      throw Error(400, "invalid_config", "Expected string: " + std::string(k));
    return std::string(o.at(k).as_string());
  };
  if (number(o.at("version")) != 1)
    throw Error(400, "invalid_config", "Unsupported configuration version");
  auto port = number(o.at("port"));
  if (port == 0 || port > 65535)
    throw Error(400, "invalid_config", "Port outside 1..65535");
  c.port = static_cast<unsigned short>(port);
  c.bind = text("bind");
  c.api_key = text("api_key");
  c.admin_key = text("admin_key");
  if (!o.at("admin_origins").is_array())
    throw Error(400, "invalid_config", "admin_origins must be an array");
  c.admin_origins = o.at("admin_origins").as_array();
  c.cookie_import_key = text("cookie_import_key");
  auto import_accounts = number(o.at("cookie_import_max_accounts"));
  auto import_timeout = number(o.at("cookie_import_timeout_seconds"));
  if (import_accounts < 1 || import_accounts > 32 || import_timeout < 1 || import_timeout > 240)
    throw Error(400, "invalid_config", "Cookie import permits 1..32 accounts and a 1..240 second timeout");
  c.cookie_import_max_accounts = static_cast<unsigned>(import_accounts);
  c.cookie_import_timeout_seconds = static_cast<unsigned>(import_timeout);
  c.data_dir = fs::u8path(text("data_dir"));
  c.browser_executable = fs::u8path(text("browser_executable"));
  // Explicit CLI/environment relative paths retain their normal shell meaning.
  if (!c.data_dir.empty())
    c.data_dir = fs::absolute(c.data_dir).lexically_normal();
  if (!c.browser_executable.empty())
    c.browser_executable = fs::absolute(c.browser_executable).lexically_normal();
  else if (!explicit_browser)
    c.browser_executable = bundled_browser(Config::default_directory());
  c.proxy = text("proxy");
  if (!o.at("browser_proxy_relay").is_bool())
    throw Error(400, "invalid_config", "browser_proxy_relay must be boolean");
  c.browser_proxy_relay = o.at("browser_proxy_relay").as_bool();
  if (!o.at("browser_remote_allow_origins").is_bool())
    throw Error(400, "invalid_config", "browser_remote_allow_origins must be boolean");
  c.browser_remote_allow_origins = o.at("browser_remote_allow_origins").as_bool();
  if (!o.at("browser_site_isolation").is_bool())
    throw Error(400, "invalid_config", "browser_site_isolation must be boolean");
  c.browser_site_isolation = o.at("browser_site_isolation").as_bool();
  if (!o.at("headless").is_bool())
    throw Error(400, "invalid_config", "headless must be boolean");
  c.headless = o.at("headless").as_bool();
  if (!o.at("ignore_unsupported_parameters").is_bool())
    throw Error(400, "invalid_config", "ignore_unsupported_parameters must be boolean");
  c.ignore_unsupported_parameters = o.at("ignore_unsupported_parameters").as_bool();
  if (!o.at("search_model_drop_functions").is_bool())
    throw Error(400, "invalid_config", "search_model_drop_functions must be boolean");
  c.search_model_drop_functions = o.at("search_model_drop_functions").as_bool();
  const auto idle = number(o.at("browser_idle_seconds"));
  if (idle > 86400)
    throw Error(400, "invalid_config", "browser_idle_seconds must be 0 or 10..86400");
  c.browser_idle_seconds = static_cast<unsigned>(idle);
  const auto retention = number(o.at("request_retention_days"));
  if (retention < 1 || retention > 3650)
    throw Error(400, "invalid_config", "request_retention_days must be 1..3650");
  c.request_retention_days = static_cast<unsigned>(retention);
  if (!o.at("limits").is_object() || !o.at("model_aliases").is_object() ||
      !o.at("configured_models").is_array() ||
      !o.at("model_defaults").is_object())
    throw Error(400, "invalid_config", "Invalid configuration container");
  auto &l = o.at("limits").as_object();
  fields(l, {"request_bytes", "object_bytes", "relay_bytes", "queue_size",
             "queue_seconds", "generation_seconds", "stream_idle_seconds",
             "max_attempts"});
  auto n = [&](const char *k) {
    auto x = number(l.at(k));
    if (!x || x > std::numeric_limits<unsigned>::max())
      throw Error(400, "invalid_config", "Limit outside supported range");
    return static_cast<unsigned>(x);
  };
  c.limits.request_bytes = n("request_bytes");
  c.limits.object_bytes = n("object_bytes");
  c.limits.relay_bytes = n("relay_bytes");
  c.limits.queue_size = n("queue_size");
  c.limits.queue_seconds = n("queue_seconds");
  c.limits.generation_seconds = n("generation_seconds");
  c.limits.stream_idle_seconds = n("stream_idle_seconds");
  c.limits.max_attempts = n("max_attempts");
  c.model_aliases = o.at("model_aliases").as_object();
  c.configured_models = o.at("configured_models").as_array();
  c.model_defaults = o.at("model_defaults").as_object();
  c.validate();
  return c;
}

Config Config::patched(const json::object& patch) const {
  auto o = to_json();
  for (const auto& entry : patch) {
    if (entry.key() == "limits" && entry.value().is_object())
      merge(o.at("limits").as_object(), entry.value().as_object());
    else
      o[entry.key()] = entry.value();
  }
  for (auto key : {"api_key", "admin_key", "cookie_import_key"})
    if (auto value = patch.if_contains(key); value && value->is_string() && value->as_string() == "***")
      throw Error(400, "invalid_config", "Redacted secrets cannot be saved");
  if (auto value = patch.if_contains("admin_key"); value && value->is_string() && value->as_string() == default_admin_key)
    throw Error(400, "admin_password_invalid", "The default management password cannot be configured");
  // Only limits merge recursively. Model maps and arrays replace the old value.
  return parse_config(*this, std::move(o), true);
}
bool restart_required(const Config& previous, const Config& next) {
  return next.bind != previous.bind || next.port != previous.port ||
    next.data_dir != previous.data_dir || next.browser_executable != previous.browser_executable ||
    next.proxy != previous.proxy || next.browser_proxy_relay != previous.browser_proxy_relay ||
    next.browser_remote_allow_origins != previous.browser_remote_allow_origins ||
    next.browser_site_isolation != previous.browser_site_isolation ||
    next.headless != previous.headless ||
    next.limits.generation_seconds != previous.limits.generation_seconds ||
    next.limits.stream_idle_seconds != previous.limits.stream_idle_seconds ||
    next.limits.object_bytes != previous.limits.object_bytes || next.limits.relay_bytes != previous.limits.relay_bytes;
}
static bool valid_admin_origin(std::string_view origin) {
  auto start = origin.starts_with("https://") ? 8u : origin.starts_with("http://") ? 7u : 0u;
  if (!start) return false;
  auto authority = origin.substr(start);
  if (authority.empty() || authority.find_first_of("/?#@*\\") != std::string_view::npos) return false;
  for (unsigned char ch : authority) if (ch <= 32 || ch >= 127) return false;
  std::string_view host, port;
  if (authority.front() == '[') {
    auto end = authority.find(']');
    if (end == std::string_view::npos) return false;
    host = authority.substr(1, end - 1);
    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(std::string(host), ec);
    if (ec || !address.is_v6() || host.find('%') != std::string_view::npos) return false;
    auto rest = authority.substr(end + 1);
    if (!rest.empty()) { if (rest.front() != ':') return false; port = rest.substr(1); if (port.empty()) return false; }
  } else {
    auto colon = authority.find(':');
    host = authority.substr(0, colon);
    if (colon != std::string_view::npos) { port = authority.substr(colon + 1); if (port.empty()) return false; }
    if (host.empty() || host.size() > 253) return false;
    if (host.back() == '.') host.remove_suffix(1);
    if (host.empty()) return false;
    while (!host.empty()) {
      auto dot = host.find('.'); auto label = host.substr(0, dot);
      if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
      for (unsigned char ch : label) if (!std::isalnum(ch) && ch != '-') return false;
      if (dot == std::string_view::npos) break;
      host.remove_prefix(dot + 1);
    }
  }
  if (!port.empty()) {
    unsigned value = 0;
    for (unsigned char ch : port) { if (ch < '0' || ch > '9') return false; value = value * 10 + ch - '0'; if (value > 65535) return false; }
    if (!value) return false;
  }
  return true;
}

void Config::validate() const {
  if (admin_key == "***" || api_key == "***" || cookie_import_key == "***")
    throw Error(400, "invalid_config", "Redacted secrets cannot be saved");
  if (admin_key.size() > 4096 || std::any_of(admin_key.begin(), admin_key.end(), [](unsigned char ch) { return ch < 33 || ch > 126; }))
    throw Error(400, "invalid_config", "admin_key must be empty or visible ASCII without whitespace");
  for (const auto& origin : admin_origins)
    if (!origin.is_string() || !valid_admin_origin(std::string_view(origin.as_string().data(), origin.as_string().size())))
      throw Error(400, "invalid_config", "admin_origins must contain exact HTTP(S) origins");
  if (bind.empty() || data_dir.empty() || !port)
    throw Error(400, "invalid_config",
                "Bind, data directory and port are required");
  if (api_key.size() > 4096 ||
      std::any_of(api_key.begin(), api_key.end(), [](unsigned char ch) {
        return ch < 33 || ch > 126;
      }))
    throw Error(400, "invalid_config",
                "api_key must be empty or at most 4096 visible ASCII characters without whitespace");
  if (cookie_import_key.size() > 4096 ||
      std::any_of(cookie_import_key.begin(), cookie_import_key.end(), [](unsigned char ch) {
        return ch < 33 || ch > 126;
      }))
    throw Error(400, "invalid_config", "cookie_import_key must be empty or at most 4096 visible ASCII characters without whitespace");
  // Only the relay needs a single parseable proxy URL; the direct path keeps
  // passing other forms (such as Chromium proxy rules) through unchanged.
  if (browser_proxy_relay && !proxy.empty()) {
    const auto endpoint = parse_proxy(proxy);
    if (!endpoint || !relay_supports(*endpoint))
      throw Error(400, "invalid_config", "browser_proxy_relay needs proxy as http://[user:password@]host:port or socks5://[user:password@]host:port");
  }
  if (browser_idle_seconds && (browser_idle_seconds < 10 || browser_idle_seconds > 86400))
    throw Error(400, "invalid_config", "browser_idle_seconds must be 0 or 10..86400");
  if (request_retention_days < 1 || request_retention_days > 3650)
    throw Error(400, "invalid_config", "request_retention_days must be 1..3650");
  if (!cookie_import_max_accounts || cookie_import_max_accounts > 32 ||
      !cookie_import_timeout_seconds || cookie_import_timeout_seconds > 240)
    throw Error(400, "invalid_config", "Cookie import permits 1..32 accounts and a 1..240 second timeout");
  for (auto &p : model_aliases)
    if (!p.value().is_string() || p.value().as_string().empty())
      throw Error(400, "invalid_config",
                  "Model aliases must map to nonempty strings");
  for (auto &m : configured_models)
    if (!m.is_string() && !m.is_object())
      throw Error(400, "invalid_config", "Invalid configured model");
  if (!limits.request_bytes || !limits.object_bytes || !limits.relay_bytes ||
      !limits.queue_size || !limits.queue_seconds ||
      !limits.generation_seconds || !limits.stream_idle_seconds ||
      !limits.max_attempts || limits.max_attempts > 16 ||
      limits.queue_size > 4096 || limits.relay_bytes > limits.object_bytes)
    throw Error(400, "invalid_config", "Inconsistent resource limits");
}
std::optional<std::string> Config::authenticate(std::string_view secret) const {
  if (api_key.empty() || secret.empty() || secret.size() > 4096)
    return std::nullopt;
  const auto expected = sha256(api_key);
  const auto presented = sha256(secret);
  if (CRYPTO_memcmp(expected.data(), presented.data(), expected.size()) != 0)
    return std::nullopt;
  // The same configured key keeps tool-signature state across restarts. A
  // rotation gets its own principal, so an old caller cannot share that state.
  return "config-key-" + expected;
}
std::optional<std::string> Config::authenticate_cookie_import(std::string_view secret) const {
  if (cookie_import_key.empty() || secret.empty() || secret.size() > 4096)
    return std::nullopt;
  const auto expected = sha256(cookie_import_key);
  const auto presented = sha256(secret);
  if (CRYPTO_memcmp(expected.data(), presented.data(), expected.size()) != 0)
    return std::nullopt;
  return "cookie-import-key-" + expected;
}
void validate_admin_password(std::string_view next, std::string_view current) {
  if (next.size() < 8 || next.size() > 4096 ||
      std::any_of(next.begin(), next.end(), [](unsigned char ch) { return ch < 33 || ch > 126; }))
    throw Error(400, "admin_password_invalid",
                "The management password must be 8..4096 visible ASCII characters without whitespace");
  if (next == default_admin_key)
    throw Error(400, "admin_password_invalid", "The default management password cannot be reused");
  if (next == current)
    throw Error(400, "admin_password_invalid", "The new management password must differ from the current one");
}
bool local_network_address(std::string_view text) {
  namespace ip = boost::asio::ip;
  boost::system::error_code ec;
  auto address = ip::make_address(std::string(text), ec);
  if (ec) return false;
  if (address.is_v6()) {
    const auto v6 = address.to_v6();
    if (!v6.is_v4_mapped()) {
      const auto bytes = v6.to_bytes();
      // ::1, fe80::/10 link-local and fc00::/7 unique local addresses.
      return v6.is_loopback() || v6.is_link_local() || (bytes[0] & 0xfe) == 0xfc;
    }
    address = ip::make_address_v4(ip::v4_mapped, v6);
  }
  const auto value = address.to_v4().to_uint();
  const auto within = [value](std::uint32_t network, unsigned bits) {
    return (value >> (32 - bits)) == (network >> (32 - bits));
  };
  // Loopback, RFC 1918 private ranges and 169.254/16 link-local.
  return within(0x7f000000, 8) || within(0x0a000000, 8) || within(0xac100000, 12) ||
         within(0xc0a80000, 16) || within(0xa9fe0000, 16);
}
std::optional<std::string> Config::authenticate_admin(std::string_view secret) const {
  if (admin_key.empty() || secret.empty() || secret.size() > 4096) return std::nullopt;
  const auto expected = sha256(admin_key);
  const auto presented = sha256(secret);
  if (CRYPTO_memcmp(expected.data(), presented.data(), expected.size()) != 0) return std::nullopt;
  return "admin-key-" + expected;
}
} // namespace ais2api
