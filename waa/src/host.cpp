// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "host.hpp"
#include "host_scripts.hpp"
#include "text.hpp"
#include <cctype>
#include <random>
#include <zlib.h>

namespace ais2api::waa {
namespace {
#include "waa_host.inc"

using js::Handle;
using js::Value;

// The Window properties an iframe shares with the top page.
constexpr std::string_view frame_window_names[] = {"outerWidth", "outerHeight", "screenX", "screenY",
                                                   "screenLeft", "screenTop", "devicePixelRatio", "mozInnerScreenX"};

// Releases a handle when the scope ends.
struct Held {
    js::Context &context;
    Handle handle;
    ~Held() { context.release(handle); }
};
std::string gunzip(std::string_view data) {
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) throw Error(500, "waa_runtime_failed", "Could not start inflating the shape table");
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    std::string out;
    int status = Z_OK;
    while (status == Z_OK) {
        const auto offset = out.size();
        out.resize(offset + 1024 * 1024);
        stream.next_out = reinterpret_cast<Bytef *>(out.data() + offset);
        stream.avail_out = 1024 * 1024;
        status = inflate(&stream, Z_NO_FLUSH);
        out.resize(offset + 1024 * 1024 - stream.avail_out);
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END) throw Error(500, "waa_runtime_failed", "The embedded Firefox shape table is damaged");
    return out;
}
// Closure's event library names its listener map with a random number when
// the page loads; the captured table holds one page's name.
std::string with_listener_map_name(std::string shape) {
    constexpr std::string_view prefix = "closure_lm_";
    std::mt19937 generator(std::random_device{}());
    const auto name = std::string(prefix) + std::to_string(std::uniform_int_distribution<int>(0, 999999)(generator));
    std::string out;
    out.reserve(shape.size() + 64);
    std::size_t at = 0;
    for (;;) {
        const auto found = shape.find(prefix, at);
        if (found == std::string::npos) break;
        auto end = found + prefix.size();
        while (end < shape.size() && shape[end] >= '0' && shape[end] <= '9') ++end;
        if (end == found + prefix.size()) {
            // No digits: not the listener map's name.
            out.append(shape, at, end - at);
        } else {
            out.append(shape, at, found - at);
            out += name;
        }
        at = end;
    }
    out.append(shape, at, std::string::npos);
    return out;
}
// A page address split into the Location attributes.
json::object location_profile(std::string_view raw) {
    const auto slashes = raw.find("//");
    const auto protocol = slashes == std::string_view::npos ? raw : raw.substr(0, slashes);
    auto rest = slashes == std::string_view::npos ? std::string_view{} : raw.substr(slashes + 2);
    std::string_view host = rest, path = "/";
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        host = rest.substr(0, slash);
        path = rest.substr(slash);
    }
    std::string_view search, hash;
    if (const auto mark = path.find('#'); mark != std::string_view::npos) {
        hash = path.substr(mark);
        path = path.substr(0, mark);
    }
    if (const auto mark = path.find('?'); mark != std::string_view::npos) {
        search = path.substr(mark);
        path = path.substr(0, mark);
    }
    return {{"href", raw}, {"origin", std::string(protocol) + "//" + std::string(host)}, {"protocol", protocol},
            {"host", host}, {"hostname", host}, {"port", ""}, {"pathname", path}, {"search", search}, {"hash", hash}};
}
double epoch_milliseconds(std::chrono::system_clock::time_point time) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count());
}
} // namespace

std::string_view dom_script() { return {reinterpret_cast<const char *>(embedded_waa_dom), sizeof(embedded_waa_dom)}; }
std::string_view host_interface_script() { return {reinterpret_cast<const char *>(embedded_waa_host), sizeof(embedded_waa_host)}; }
std::string_view shape_table_gzip() { return {reinterpret_cast<const char *>(embedded_waa_shape), sizeof(embedded_waa_shape)}; }

json::object host_versions() {
    return {{"dom", embedded_waa_dom_sha256}, {"host", embedded_waa_host_sha256}, {"shape", embedded_waa_shape_sha256},
            {"spidermonkey", AIS2API_SPIDERMONKEY_VERSION}};
}

Agent::Agent(EventLoop &loop, Profile profile, Loader load_image, Loader load_frame, std::stop_token closing)
    : loop_(loop), profile_(std::move(profile)), load_image_(std::move(load_image)), load_frame_(std::move(load_frame)),
      closing_(std::move(closing)), timezone_(profile_.timezone.empty() ? "UTC" : profile_.timezone) {
    // The account's language as Camoufox gives it to mozilla::intl: the
    // language in lower case, the region in upper case, and the whole tag.
    const auto dash = profile_.locale.find('-');
    std::string language = profile_.locale.substr(0, dash), region = dash == std::string::npos ? "" : profile_.locale.substr(dash + 1);
    for (auto &character : language) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    for (auto &character : region) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    js::set_locale_override(language, region, profile_.locale);
    context_ = std::make_unique<js::Context>(1u << 30, profile_.locale);
}

Agent::~Agent() {
    std::vector<std::jthread> loaders;
    {
        std::lock_guard lock(loaders_mutex_);
        loaders.swap(loaders_);
    }
    // jthread joins; the loaders stop on the closing token.
}

void Agent::close() {
    std::lock_guard lock(context_mutex_);
    context_.reset();
}

void Agent::interrupt() {
    std::lock_guard lock(context_mutex_);
    if (context_) context_->interrupt();
}

Value Agent::invoke(Handle function, Handle receiver, std::initializer_list<Value> arguments) {
    ++depth_;
    struct Leave {
        Agent &agent;
        ~Leave() {
            if (--agent.depth_ == 0) agent.context_->run_jobs();
        }
    } leave{*this};
    return context_->call(function, receiver, std::span<const Value>(arguments.begin(), arguments.size()));
}

Value Agent::evaluate(Handle global, std::string_view source, std::string_view name) {
    ++depth_;
    struct Leave {
        Agent &agent;
        ~Leave() {
            if (--agent.depth_ == 0) agent.context_->run_jobs();
        }
    } leave{*this};
    return context_->evaluate(global, source, name, 1);
}

Value Agent::new_array(std::initializer_list<Value> items) {
    ++depth_;
    struct Leave {
        Agent &agent;
        ~Leave() { --agent.depth_; }
    } leave{*this};
    return context_->call(array, 0, std::span<const Value>(items.begin(), items.size()));
}

void Agent::release(const js::Exception &exception) { context_->release(exception.value); }

void Agent::report(RealmHost &realm, const js::Exception &exception) {
    if (!exception.value) return;
    Held thrown{*context_, exception.value};
    context_->set_throw_location(exception.filename, exception.line, exception.column);
    try {
        context_->release(invoke(realm.report, 0, {Value::object(exception.value)}));
    } catch (const js::Exception &failure) {
        release(failure);
    } catch (const std::exception &) {
        // An interrupted script: the runtime is closing.
    }
}

void Agent::dispatch(RealmHost &realm, double id, const Value &argument) {
    try {
        context_->release(invoke(realm.dispatch, 0, {Value::of(id), argument}));
    } catch (const js::Exception &exception) {
        report(realm, exception);
    } catch (const std::exception &) {
        // An interrupted script: the runtime is closing.
    }
}

void Agent::loaded() {
    if (--pending_ > 0) return;
    std::vector<std::function<void()>> settled;
    settled.swap(settled_);
    for (const auto &done : settled) done();
}

void Agent::when_settled(std::function<void()> done) {
    if (pending_ == 0) done();
    else settled_.push_back(std::move(done));
}

void Agent::after_task() {
    if (!context_) return;
    struct Leave {
        js::Context &context;
        ~Leave() { context.clear_kept_objects(); }
    } leave{*context_};
    if (!track_) return;
    ++depth_;
    try {
        context_->flush_rejections(track_);
        --depth_;
    } catch (const js::Exception &exception) {
        --depth_;
        release(exception);
    }
    try {
        context_->release(invoke(notify_, 0));
    } catch (const js::Exception &exception) {
        release(exception);
    }
}

void Agent::load(RealmHost &realm, const Loader &loader, std::string address, double id) {
    ++pending_;
    std::lock_guard lock(loaders_mutex_);
    loaders_.emplace_back([this, &realm, &loader, address = std::move(address), id] {
        bool received = false;
        try {
            received = loader(address, closing_);
        } catch (...) {
        }
        loop_.post([this, &realm, id, received] {
            dispatch(realm, id, Value::of(received));
            loaded();
        });
    });
}

std::vector<std::pair<std::string, Agent::Native>> Agent::natives(RealmHost &realm) {
    std::vector<std::pair<std::string, Native>> result;
    result.push_back({"schedule", {2, [this, &realm](js::Call &call) {
        const double delay = call.arg_number(0), id = call.arg_number(1);
        // NaN and negative delays are due at once.
        const auto wait = delay > 0 ? std::chrono::nanoseconds(static_cast<std::int64_t>(delay * 1e6)) : std::chrono::nanoseconds::zero();
        loop_.schedule(wait, [this, &realm, id] { dispatch(realm, id, {}); });
    }}});
    result.push_back({"digest", {2, [](js::Call &call) {
        const auto data = call.arg_bytes(1).value_or(std::string());
        call.return_bytes(digest_bytes(call.arg_string(0), data));
    }}});
    result.push_back({"encodingName", {1, [](js::Call &call) {
        call.set_return(Value::of(encoding_name(call.arg_string(0))));
    }}});
    result.push_back({"decodeText", {3, [this](js::Call &call) {
        const auto data = call.arg_bytes(1).value_or(std::string());
        const auto fatal = call.arg(2);
        context_->release(fatal);
        auto text = decode_text(call.arg_string(0), data, fatal.boolean);
        if (!text) throw js::Throw{"TypeError", "TextDecoder.decode: Decoding failed."};
        call.set_return(Value::of(std::move(*text)));
    }}});
    result.push_back({"parseURL", {2, [this](js::Call &call) {
        const auto base = call.arg(1);
        context_->release(base);
        auto record = parse_url(call.arg_string(0), base.kind == js::Kind::string ? &base.string : nullptr);
        call.set_return(record ? Value::of(std::move(*record)) : Value::null());
    }}});
    result.push_back({"updateURL", {3, [](js::Call &call) {
        auto record = update_url(call.arg_string(0), call.arg_string(1), call.arg_string(2));
        call.set_return(record ? Value::of(std::move(*record)) : Value::null());
    }}});
    result.push_back({"randomUUID", {0, [](js::Call &call) { call.set_return(Value::of(random_uuid())); }}});
    result.push_back({"randomBytes", {1, [](js::Call &call) {
        const double count = call.arg_number(0);
        // A count no ArrayBuffer could hold is the script's mistake.
        call.return_bytes(random_bytes(count > 0 && count <= 65536 ? static_cast<std::size_t>(count) : 0));
    }}});
    if (load_image_)
        result.push_back({"loadImage", {2, [this, &realm](js::Call &call) {
            load(realm, load_image_, call.arg_string(0), call.arg_number(1));
        }}});
    if (load_frame_)
        result.push_back({"loadFrame", {2, [this, &realm](js::Call &call) {
            load(realm, load_frame_, call.arg_string(0), call.arg_number(1));
        }}});
    if (realm.name == "top")
        result.push_back({"createIframeWindow", {0, [this](js::Call &call) {
            try {
                call.set_return(Value::object(install_realm("frame", top->global).global));
            } catch (const js::Exception &exception) {
                release(exception);
                throw std::runtime_error(exception.message);
            }
        }}});
    return result;
}

json::object Agent::realm_profile(std::string_view realm) const {
    const bool is_top = realm == "top";
    json::object values, window;
    if (!profile_.navigator.empty()) values["navigator"] = profile_.navigator;
    if (!profile_.screen.empty()) values["screen"] = profile_.screen;
    if (is_top) {
        window = profile_.window;
        if (!profile_.document.empty()) values["document"] = profile_.document;
        if (const auto width = profile_.window.if_contains("innerWidth")) {
            const auto height = profile_.window.if_contains("innerHeight");
            values["visualViewport"] = json::object{{"width", *width}, {"height", height ? *height : json::value()}};
        }
    } else {
        for (const auto name : frame_window_names)
            if (const auto value = profile_.window.if_contains(name)) window[name] = *value;
        json::object document;
        for (const std::string_view name : {"cookie", "lastModified"})
            if (const auto value = profile_.document.if_contains(name)) document[name] = *value;
        if (!document.empty()) values["document"] = std::move(document);
    }
    json::object result{{"hasFocus", profile_.has_focus}, {"window", std::move(window)}, {"values", std::move(values)}};
    if (is_top && !profile_.globals.empty()) result["globals"] = profile_.globals;
    if (!profile_.local_storage.empty()) {
        json::object storage;
        for (const auto &[key, value] : profile_.local_storage) storage[key] = value;
        result["localStorage"] = std::move(storage);
    }
    if (is_top && !profile_.page_url.empty()) result["location"] = location_profile(profile_.page_url);
    return result;
}

RealmHost &Agent::install_realm(std::string name, Handle same) {
    auto &context = *context_;
    auto &realm = realms_.emplace_back();
    realm.name = std::move(name);
    realm.global = context.new_global(same, timezone_);
    context.install_window_natives(realm.global);
    const Held install_dom{context, handle_of(evaluate(realm.global, dom_script(), scripts::host_script_name))};
    const Held install{context, handle_of(evaluate(realm.global, scripts::host_script, scripts::host_script_name))};
    const Held build{context, handle_of(evaluate(realm.global, host_interface_script(), scripts::host_script_name))};
    const Held host_natives{context, context.new_object(realm.global)};
    context.install_host_natives(host_natives.handle);
    for (auto &[native_name, native] : natives(realm)) {
        const Held function{context, context.new_function(realm.global, native_name, native.arity, std::move(native.call))};
        context.set(host_natives.handle, native_name, Value::object(function.handle));
    }
    const Held config{context, context.new_object(realm.global)};
    const bool is_top = realm.name == "top";
    const auto wall = std::chrono::system_clock::now();
    const auto started = is_top && profile_.navigation_start ? *profile_.navigation_start : wall;
    // `started` on the script's monotonic clock: now, less the time since.
    const auto since = std::chrono::duration_cast<std::chrono::microseconds>(wall - started).count();
    context.set(config.handle, "realm", Value::of(realm.name));
    context.set(config.handle, "trustedTypes", Value::of(profile_.trusted_types));
    context.set(config.handle, "profileJSON", Value::of(sorted_json(realm_profile(realm.name))));
    context.set(config.handle, "timeOrigin", Value::of(epoch_milliseconds(started)));
    context.set(config.handle, "started", Value::of(js::now() - static_cast<double>(since) / 1000.0));
    // The first realm parses the shape table; the others share its result.
    if (!shared_) context.set(config.handle, "shapeJSON", Value::of(with_listener_map_name(gunzip(shape_table_gzip()))));
    const Held result{context, handle_of(invoke(build.handle, 0,
                                                {Value::object(host_natives.handle), Value::object(install_dom.handle),
                                                 Value::object(install.handle), Value::object(config.handle),
                                                 shared_ ? Value::object(shared_) : Value{}}))};
    realm.dispatch = handle_of(context.get(result.handle, "dispatch"));
    realm.report = handle_of(context.get(result.handle, "report"));
    realm.order = handle_of(context.get(result.handle, "orderGlobal"));
    if (!shared_) install_agent(result.handle);
    return realm;
}

void Agent::install_agent(Handle result) {
    auto &context = *context_;
    shared_ = handle_of(context.get(result, "shared"));
    track_ = handle_of(context.get(result, "track"));
    notify_ = handle_of(context.get(result, "notify"));
    context.set_microtask_reporter(handle_of(context.get(result, "reportMicrotask")));
    const Held registry{context, handle_of(context.get(shared_, "registry"))};
    context.set_trusted_types(handle_of(context.get(registry.handle, "trusted")), profile_.trusted_types);
}
} // namespace ais2api::waa
