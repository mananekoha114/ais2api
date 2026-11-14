// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// SpiderMonkey behind the embedding shim's C interface (mozjs.h): contexts,
// rooted handles, evaluation, native functions, microtasks and interrupts.
// The shim is built with clang-cl beside SpiderMonkey; this side is plain
// C++ and never includes a SpiderMonkey header.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct mz_context;
struct mz_call;

namespace ais2api::waa::js {
// A value rooted in its context's table; 0 is undefined.
using Handle = std::uint32_t;

enum class Kind { undefined, null, boolean, number, string, handle };

// A value crossing the boundary: primitives by value, strings as UTF-8,
// everything else as a handle the receiver releases.
struct Value {
    Kind kind = Kind::undefined;
    bool boolean = false;
    double number = 0;
    std::string string;
    Handle handle = 0;

    static Value null() { return {Kind::null}; }
    static Value of(bool value) { return {Kind::boolean, value}; }
    static Value of(double value) { return {Kind::number, false, value}; }
    static Value of(std::string value) { return {Kind::string, false, 0, std::move(value)}; }
    static Value object(Handle handle) { return {Kind::handle, false, 0, {}, handle}; }
};

// An exception a script threw. what() is the message followed by the stack;
// `value` is the thrown value's handle, which the catcher releases.
struct Exception : std::runtime_error {
    std::string message, stack, filename;
    unsigned line = 0, column = 0;
    Handle value = 0;
    Exception(std::string message, std::string stack, std::string filename, unsigned line, unsigned column, Handle value);
};

// Thrown by a native function to raise `type`, an error constructor of the
// calling global, in the script. Any other exception raises an Error.
struct Throw {
    std::string type, message;
};

class Context;

// One call of a native function.
class Call {
  public:
    // The argument at `index`; undefined when there is none. The caller
    // releases a handle.
    Value arg(unsigned index);
    // Empty when the argument is not a string.
    std::string arg_string(unsigned index);
    // 0 when the argument is not a number.
    double arg_number(unsigned index);
    // A copy of the bytes of an ArrayBuffer or view; nothing for other values.
    std::optional<std::string> arg_bytes(unsigned index);
    void set_return(const Value &value);
    // Returns a new ArrayBuffer holding `data`.
    void return_bytes(std::string_view data);

  private:
    friend class Context;
    friend bool dispatch_native(std::uintptr_t, std::int32_t, mz_call *);
    Call(mz_call *call, Context &context) : call_(call), context_(context) {}
    mz_call *call_;
    Context &context_;
};

using Native = std::function<void(Call &)>;

// A JSContext with its roots, microtask queue and host state. It belongs to
// the thread that created it: only interrupt() may be called from another.
// Failed calls throw Exception.
class Context {
  public:
    // `heap_bytes` bounds the garbage-collected heap; `locale` is the default
    // locale of every global.
    Context(std::uint32_t heap_bytes, std::string_view locale);
    ~Context();
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    // A new Window global; with `same`, in that global's compartment.
    Handle new_global(Handle same, std::string_view timezone);
    Value evaluate(Handle global, std::string_view source, std::string_view filename, unsigned line = 1);
    // Calls `function` with `receiver` as this; 0 is undefined.
    Value call(Handle function, Handle receiver, std::span<const Value> arguments = {});
    Value get(Handle object, std::string_view name);
    void set(Handle object, std::string_view name, const Value &value);
    Handle new_object(Handle global);
    Value parse_json(Handle global, std::string_view text);
    Handle new_function(Handle global, std::string_view name, unsigned arity, Native native);
    // Defines the shim's own host functions (now, kindOf, throwLocation,
    // incumbent) on `target`, and queueMicrotask, atob and btoa on a global.
    void install_host_natives(Handle target);
    void install_window_natives(Handle global);

    // A microtask checkpoint. Exceptions go to the microtask reporter.
    void run_jobs();
    // Ends the current task's WeakRef liveness.
    void clear_kept_objects();
    // `function(global, exception)` receives what microtasks throw.
    void set_microtask_reporter(Handle function);
    // Calls `function(promise, handled, global)` for each recorded promise
    // rejection, in order, and forgets them.
    void flush_rejections(Handle function);
    // The TrustedScript source registry (a WeakMap), and whether compiling
    // code from plain strings is refused as the page's CSP does.
    void set_trusted_types(Handle registry, bool block_strings);
    void set_throw_location(std::string_view filename, unsigned line, unsigned column);

    // Stops the running script. Safe from any thread, and after the context
    // is gone it does nothing.
    void interrupt();
    // A full collection; `shrink` also returns free memory and drops JIT code.
    void gc(bool shrink = false);
    void release(Handle handle);
    void release(const Value &value);

  private:
    friend class Call;
    friend bool dispatch_native(std::uintptr_t, std::int32_t, mz_call *);
    [[noreturn]] void fail();
    mz_context *context_ = nullptr;
    std::vector<Native> natives_;
    // Keeps interrupt() from another thread apart from destruction.
    std::mutex mutex_;
    bool closed_ = false;
};

// Camoufox's locale override for mozilla::intl on the calling thread: the
// language, the region and navigator.language.
void set_locale_override(std::string_view language, std::string_view region, std::string_view locale);
// Milliseconds of a monotonic clock, to the microsecond.
double now();
} // namespace ais2api::waa::js
