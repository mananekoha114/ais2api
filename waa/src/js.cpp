// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "js.hpp"
#include "mozjs.h"
#include <algorithm>

namespace ais2api::waa::js {
namespace {
// The shim reports a null string for a missing one.
std::string take_string(char *text) {
    if (!text) return {};
    std::string result(text);
    mz_free(text);
    return result;
}
// Takes the shim's value, freeing the string it allocated.
Value from_shim(mz_value &value) {
    Value result{static_cast<Kind>(value.kind), value.boolean, value.number, {}, value.handle};
    if (result.kind == Kind::string) {
        if (value.str) result.string.assign(value.str, value.len);
        mz_free(value.str);
        value.str = nullptr;
    }
    return result;
}
// A view the shim reads during the call; `value` must outlive it.
mz_value to_shim(const Value &value) {
    mz_value result{};
    result.kind = static_cast<std::int32_t>(value.kind);
    result.boolean = value.boolean;
    result.number = value.number;
    result.handle = value.handle;
    if (value.kind == Kind::string) {
        // The shim takes the length and never writes through the pointer.
        result.str = const_cast<char *>(value.string.data());
        result.len = value.string.size();
    }
    return result;
}
// The process-wide engine starts once; a failure is permanent.
void initialise() {
    static const std::string failure = [] {
        const char *reason = mz_init();
        return reason ? std::string(reason) : std::string();
    }();
    if (!failure.empty()) throw std::runtime_error("SpiderMonkey did not start: " + failure);
}
// Room left on the stack for the shim and the host beneath a script.
constexpr std::size_t stack_slack = 256 * 1024, minimum_stack_quota = 512 * 1024;
} // namespace

static_assert(static_cast<int>(Kind::undefined) == MZ_UNDEFINED && static_cast<int>(Kind::null) == MZ_NULL &&
              static_cast<int>(Kind::boolean) == MZ_BOOL && static_cast<int>(Kind::number) == MZ_NUMBER &&
              static_cast<int>(Kind::string) == MZ_STRING && static_cast<int>(Kind::handle) == MZ_HANDLE);

Exception::Exception(std::string text, std::string trace, std::string file, unsigned at_line, unsigned at_column, Handle thrown)
    : std::runtime_error(trace.empty() ? text : text + "\n" + trace), message(std::move(text)), stack(std::move(trace)),
      filename(std::move(file)), line(at_line), column(at_column), value(thrown) {}

Value Call::arg(unsigned index) {
    mz_value out{};
    if (!mz_call_arg(call_, index, &out)) return {};
    return from_shim(out);
}
std::string Call::arg_string(unsigned index) {
    auto value = arg(index);
    context_.release(value);
    return std::move(value.string);
}
double Call::arg_number(unsigned index) {
    const auto value = arg(index);
    context_.release(value);
    return value.number;
}
std::optional<std::string> Call::arg_bytes(unsigned index) {
    std::uint8_t *data = nullptr;
    std::size_t size = 0;
    if (!mz_call_arg_bytes(call_, index, &data, &size)) return std::nullopt;
    std::string result(reinterpret_cast<const char *>(data), size);
    mz_free(data);
    return result;
}
void Call::set_return(const Value &value) {
    const auto converted = to_shim(value);
    mz_call_return(call_, &converted);
}
void Call::return_bytes(std::string_view data) {
    mz_call_return_bytes(call_, reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

Context::Context(std::uint32_t heap_bytes, std::string_view locale) {
    initialise();
    const auto available = mz_thread_stack_size();
    const auto quota = std::max(minimum_stack_quota, available > stack_slack ? available - stack_slack : 0);
    // The shim hands this pointer back to dispatch_native as the context's id.
    context_ = mz_context_new(reinterpret_cast<std::uintptr_t>(this), heap_bytes, quota, std::string(locale).c_str());
    if (!context_) throw std::runtime_error("SpiderMonkey could not create a context");
}
Context::~Context() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    mz_context_free(context_);
}
void Context::fail() {
    mz_exception exception{};
    if (!mz_take_exception(context_, &exception)) throw std::runtime_error("A SpiderMonkey call failed without an exception");
    auto message = take_string(exception.message), stack = take_string(exception.stack), filename = take_string(exception.filename);
    throw Exception(std::move(message), std::move(stack), std::move(filename), exception.line, exception.column, exception.value);
}
Handle Context::new_global(Handle same, std::string_view timezone) {
    const auto handle = mz_global_new(context_, same, std::string(timezone).c_str());
    if (!handle) fail();
    return handle;
}
Value Context::evaluate(Handle global, std::string_view source, std::string_view filename, unsigned line) {
    mz_value out{};
    if (!mz_evaluate(context_, global, source.data(), source.size(), std::string(filename).c_str(), line, &out)) fail();
    return from_shim(out);
}
Value Context::call(Handle function, Handle receiver, std::span<const Value> arguments) {
    std::vector<mz_value> converted;
    converted.reserve(arguments.size());
    for (const auto &argument : arguments) converted.push_back(to_shim(argument));
    mz_value out{};
    if (!mz_call_function(context_, function, receiver, converted.empty() ? nullptr : converted.data(),
                          static_cast<int>(converted.size()), &out))
        fail();
    return from_shim(out);
}
Value Context::get(Handle object, std::string_view name) {
    mz_value out{};
    if (!mz_get_property(context_, object, std::string(name).c_str(), &out)) fail();
    return from_shim(out);
}
void Context::set(Handle object, std::string_view name, const Value &value) {
    const auto converted = to_shim(value);
    if (!mz_set_property(context_, object, std::string(name).c_str(), &converted)) fail();
}
Handle Context::new_object(Handle global) {
    const auto handle = mz_new_object(context_, global);
    if (!handle) fail();
    return handle;
}
Value Context::parse_json(Handle global, std::string_view text) {
    mz_value out{};
    if (!mz_parse_json(context_, global, text.data(), text.size(), &out)) fail();
    return from_shim(out);
}
Handle Context::new_function(Handle global, std::string_view name, unsigned arity, Native native) {
    natives_.push_back(std::move(native));
    const auto handle = mz_new_function(context_, global, std::string(name).c_str(), arity,
                                        static_cast<std::int32_t>(natives_.size() - 1));
    if (!handle) fail();
    return handle;
}
void Context::install_host_natives(Handle target) {
    if (!mz_install_host_natives(context_, target)) fail();
}
void Context::install_window_natives(Handle global) {
    if (!mz_install_window_natives(context_, global)) fail();
}
void Context::run_jobs() { mz_run_jobs(context_); }
void Context::clear_kept_objects() { mz_clear_kept_objects(context_); }
void Context::set_microtask_reporter(Handle function) { mz_set_microtask_reporter(context_, function); }
void Context::flush_rejections(Handle function) {
    if (!mz_flush_rejections(context_, function)) fail();
}
void Context::set_trusted_types(Handle registry, bool block_strings) { mz_set_trusted_types(context_, registry, block_strings); }
void Context::set_throw_location(std::string_view filename, unsigned line, unsigned column) {
    mz_set_throw_location(context_, std::string(filename).c_str(), line, column);
}
void Context::interrupt() {
    std::lock_guard lock(mutex_);
    if (!closed_) mz_request_interrupt(context_);
}
void Context::gc(bool shrink) { mz_gc(context_, shrink); }
void Context::release(Handle handle) {
    if (handle) mz_release(context_, handle);
}
void Context::release(const Value &value) {
    if (value.kind == Kind::handle) release(value.handle);
}

void set_locale_override(std::string_view locale) { mz_set_camou_locale(std::string(locale).c_str()); }
double now() { return mz_now(); }

// Runs the native function a script called. The shim is compiled without
// exceptions, so none may leave: each becomes an error thrown in the script.
bool dispatch_native(std::uintptr_t id, std::int32_t native, mz_call *raw) {
    auto &context = *reinterpret_cast<Context *>(id);
    Call call(raw, context);
    try {
        context.natives_.at(static_cast<std::size_t>(native))(call);
        return true;
    } catch (const Throw &thrown) {
        mz_call_throw(raw, thrown.type.c_str(), thrown.message.c_str());
    } catch (const std::exception &error) {
        mz_call_throw(raw, "Error", error.what());
    } catch (...) {
        mz_call_throw(raw, "Error", "native function failed");
    }
    return false;
}
} // namespace ais2api::waa::js

// The shim keeps the reference project's name for its host callback.
extern "C" bool mzGoNative(std::uintptr_t id, std::int32_t native, mz_call *call) {
    return ais2api::waa::js::dispatch_native(id, native, call);
}
