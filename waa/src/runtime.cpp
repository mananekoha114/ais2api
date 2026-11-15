// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The BotGuard lifecycle. After AIStudio2API's internal/waa/runtime.go (MIT).
#include "ais2api/waa/runtime.hpp"
#include "host.hpp"
#include "host_scripts.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>

namespace ais2api::waa {
namespace {
using js::Handle;
using js::Value;
using Clock = std::chrono::steady_clock;

// How long after its last use the virtual machine compacts its heap: Firefox
// release's javascript.options.compact_on_user_inactive_delay.
constexpr std::chrono::nanoseconds idle_shrink_delay = std::chrono::minutes(5);
// How long closing waits for the program's shutdown function, and then for
// the running task, before it interrupts the script.
constexpr auto close_grace = std::chrono::seconds(1);
// How long after a click on Run the page's application asks for the
// GenerateContent proof.
constexpr auto run_proof_delay = std::chrono::milliseconds(50);
// At most this many metrics wait for take_metrics; the oldest go first.
constexpr std::size_t metric_limit = 1024;

// Releases the handles it collected when the scope ends.
class Roots {
  public:
    explicit Roots(js::Context &context) : context_(context) {}
    ~Roots() {
        for (const auto handle : handles_) context_.release(handle);
    }
    Roots(const Roots &) = delete;
    Roots &operator=(const Roots &) = delete;
    Handle add(Handle handle) {
        handles_.push_back(handle);
        return handle;
    }
    Value add(Value value) {
        if (value.kind == js::Kind::handle) handles_.push_back(value.handle);
        return value;
    }

  private:
    js::Context &context_;
    std::vector<Handle> handles_;
};

// One result handed from the loop's thread to a waiting caller.
class Result {
  public:
    void finish(std::string value) { set(std::move(value), std::nullopt); }
    void fail(std::string message) { set({}, std::move(message)); }
    // 499 when `cancel` is requested before the result arrives, 502 when the
    // loop closes first or the result is a failure.
    std::string wait(std::stop_token cancel, std::stop_token closed) {
        const auto wake = [this] {
            std::lock_guard lock(mutex_);
            arrived_.notify_all();
        };
        std::stop_callback on_cancel(cancel, wake), on_close(closed, wake);
        std::unique_lock lock(mutex_);
        arrived_.wait(lock, [&] { return done_ || cancel.stop_requested() || closed.stop_requested(); });
        if (!done_ || error_) {
            const auto message = done_ ? *error_ : std::string("The WAA runtime closed");
            lock.unlock();
            check_cancel(cancel);
            throw Error(502, "waa_runtime_failed", message);
        }
        return std::move(value_);
    }
    bool wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return arrived_.wait_for(lock, timeout, [&] { return done_; });
    }

  private:
    void set(std::string value, std::optional<std::string> error) {
        {
            std::lock_guard lock(mutex_);
            if (done_) return;
            done_ = true;
            value_ = std::move(value);
            error_ = std::move(error);
        }
        arrived_.notify_all();
    }
    std::mutex mutex_;
    std::condition_variable arrived_;
    bool done_ = false;
    std::string value_;
    std::optional<std::string> error_;
};

// A thrown value for an error message: its message and the top of its stack.
std::string describe(const js::Exception &exception) {
    constexpr std::size_t stack_limit = 600;
    auto text = exception.message;
    if (!exception.stack.empty()) text += "\n" + exception.stack.substr(0, stack_limit);
    return text;
}
// A textarea's value: CRLF and CR become LF.
std::string textarea_value(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\r') {
            out += text[i];
            continue;
        }
        out += '\n';
        if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    }
    return out;
}
std::string number_text(double number) {
    if (std::isfinite(number) && number == std::trunc(number) && std::fabs(number) < 1e15)
        return std::to_string(static_cast<long long>(number));
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", number);
    return buffer;
}
Error closed_error() { return Error(502, "waa_runtime_failed", "The WAA runtime closed"); }
double milliseconds_since(Clock::time_point started) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}
// Waits `delay` on the caller's thread: 499 when `cancel` is requested first,
// 502 when the loop closes first.
void pause(std::chrono::milliseconds delay, std::stop_token cancel, std::stop_token closed) {
    if (delay.count() > 0) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::stop_callback on_close(closed, [&] {
            std::lock_guard lock(mutex);
            wake.notify_all();
        });
        std::unique_lock lock(mutex);
        wake.wait_for(lock, cancel, delay, [&] { return closed.stop_requested(); });
    }
    check_cancel(cancel);
    if (closed.stop_requested()) throw closed_error();
}
} // namespace

struct Runtime::Impl {
    explicit Impl(Options given) : options(std::move(given)), pointer(options.pointer ? options.pointer : std::make_shared<Pointer>()) {}
    ~Impl();

    void open(std::stop_token cancel);
    // Types `prompt` when there is one, clicks through `steps`, then takes the
    // proof for `digest`; the prompt box is empty again afterwards.
    std::string proof(std::string_view digest, std::optional<std::string_view> prompt, const std::vector<PointerStep> *steps,
                      std::stop_token cancel);
    void fill_prompt(std::string prompt, std::stop_token cancel);
    // The steps of a trusted click at their intervals, then the wait until the
    // page asks for the proof.
    void click(const std::vector<PointerStep> &steps, std::stop_token cancel);
    // Dispatches one pointer step's events; with a result, reports when they
    // are through.
    void pointer_input(const PointerStep &step, std::shared_ptr<Result> result);
    std::string snapshot_with(std::optional<std::string> digest, std::stop_token cancel);
    // Clears the prompt box once the queued work is through.
    void clear_prompt();
    // Restarts the wait for the idle compaction.
    void used();
    // Keeps a metric for take_metrics; on the loop's thread.
    void record(Metric metric) {
        if (metrics.size() >= metric_limit) metrics.erase(metrics.begin());
        metrics.push_back(std::move(metric));
    }

    // On the loop's thread.
    void start(const std::string &source_name, const std::shared_ptr<Result> &ready);
    Handle new_deliver();
    std::pair<Value, Value> experiment_signals(Handle global);
    void watch_idle(std::chrono::nanoseconds delay);
    // Runs one step of the start; what the script throws becomes an error
    // naming the step.
    template <typename Body> auto step(std::string_view name, Body &&body) -> decltype(body()) {
        try {
            return body();
        } catch (const js::Exception &exception) {
            agent->release(exception);
            throw std::runtime_error(std::string(name) + ": " + describe(exception));
        }
    }

    Options options;
    EventLoop loop;
    // Guards the pointer: the loop's thread sets it, closing reads it.
    std::mutex agent_mutex;
    std::unique_ptr<Agent> agent;
    const std::shared_ptr<Pointer> pointer;
    // The loop's thread owns everything from here to `idle_armed`.
    Handle snapshot_function = 0, shutdown_function = 0, prompt_function = 0, mouse_function = 0, clear_function = 0;
    Handle options_function = 0, bind_function = 0, deliver_function = 0;
    // A snapshot waiting for its result, and when it was asked for.
    struct Pending {
        std::shared_ptr<Result> result;
        Clock::time_point started;
    };
    std::map<double, Pending> results;
    double next_result = 0;
    // What the page's BotGuard wrapper recorded since the last take_metrics.
    std::vector<Metric> metrics;
    bool idle_armed = false;
    std::atomic<Clock::rep> last_used{0};
    // One proof or snapshot at a time, as the page has one prompt box.
    std::mutex operation;
};

Runtime::Impl::~Impl() {
    const auto done = std::make_shared<Result>();
    if (loop.post([this, done] {
            if (agent && shutdown_function) {
                try {
                    agent->context().release(agent->invoke(shutdown_function, 0));
                } catch (const js::Exception &exception) {
                    agent->release(exception);
                } catch (const std::exception &) {
                }
            }
            done->finish({});
        }))
        done->wait_for(close_grace);
    loop.close();
    if (!loop.wait_exit(close_grace)) {
        std::lock_guard lock(agent_mutex);
        if (agent) agent->interrupt();
    }
    loop.join();
    // Joins the page requests in flight, which may still post to the loop.
    agent.reset();
}

void Runtime::Impl::open(std::stop_token cancel) {
    if (options.interpreter.empty()) throw Error(502, "waa_runtime_failed", "The WAA interpreter is empty");
    if (options.challenge.program.empty() || options.challenge.global_name.empty())
        throw Error(502, "waa_challenge_invalid", "The WAA challenge has no program");
    auto source_name = options.challenge.interpreter_url.empty()
                           ? "https://www.google.com/js/bg/" + options.challenge.interpreter_hash + ".js"
                           : options.challenge.interpreter_url;
    const auto ready = std::make_shared<Result>();
    if (!loop.post([this, ready, source_name = std::move(source_name)] {
            try {
                auto created = std::make_unique<Agent>(loop, options.profile, options.load_image, options.load_frame, loop.closed());
                {
                    std::lock_guard lock(agent_mutex);
                    agent = std::move(created);
                }
                loop.cleanup = [this] { agent->close(); };
                loop.after_task = [this] { agent->after_task(); };
                start(source_name, ready);
            } catch (const js::Exception &exception) {
                if (agent) agent->release(exception);
                ready->fail(describe(exception));
            } catch (const std::exception &error) {
                ready->fail(error.what());
            }
        }))
        throw closed_error();
    ready->wait(cancel, loop.closed());
    // What the start left behind goes; the compiled code stays.
    loop.post([this] {
        options.interpreter = {};
        agent->context().gc();
    });
    used();
}

void Runtime::Impl::start(const std::string &source_name, const std::shared_ptr<Result> &ready) {
    auto &state = *agent;
    auto &context = state.context();
    auto &top = step("Installing the top realm's host", [&]() -> RealmHost & { return state.install_realm("top", 0); });
    state.top = &top;
    Roots roots(context);
    const auto helpers = roots.add(handle_of(state.evaluate(top.global, scripts::helper_script, scripts::host_script_name)));
    state.array = handle_of(context.get(helpers, "array"));
    options_function = handle_of(context.get(helpers, "options"));
    bind_function = handle_of(context.get(helpers, "bind"));
    prompt_function = handle_of(state.evaluate(top.global, scripts::prompt_script, scripts::host_script_name));
    mouse_function = handle_of(state.evaluate(top.global, scripts::pointer_script, scripts::host_script_name));
    clear_function = handle_of(state.evaluate(top.global, scripts::clear_prompt_script, scripts::host_script_name));
    context.release(step("Running the WAA interpreter", [&] { return state.evaluate(top.global, options.interpreter, source_name); }));
    context.release(step("Ordering the top global's keys", [&] { return state.invoke(top.order, 0); }));
    const auto object = roots.add(context.get(top.global, options.challenge.global_name));
    if (object.kind != js::Kind::handle) throw std::runtime_error("The WAA interpreter defined no global object");
    const auto initialize = roots.add(context.get(object.handle, "a"));
    if (initialize.kind != js::Kind::handle) throw std::runtime_error("The WAA interpreter has no initialisation function");
    auto [signals, stored] = experiment_signals(top.global);
    roots.add(signals);
    // The wrapper counts the initialisation and times it until the program
    // hands over its snapshot function.
    const auto init_started = Clock::now();
    record({"ec", "t", 1});
    const auto callback = roots.add(context.new_function(top.global, "", 2, [this, ready, init_started](js::Call &call) {
        const auto snapshot = call.arg(0);
        if (snapshot.kind != js::Kind::handle) {
            ready->fail("The WAA interpreter returned no snapshot function");
            return;
        }
        record({"el", "t", milliseconds_since(init_started)});
        snapshot_function = snapshot.handle;
        if (const auto shutdown = call.arg(1); shutdown.kind == js::Kind::handle) shutdown_function = shutdown.handle;
        ready->finish({});
    }));
    deliver_function = new_deliver();
    const auto metric_sink = roots.add(context.new_function(top.global, "", 3, [this](js::Call &call) {
        const auto label = call.arg(1);
        agent->context().release(label);
        record({call.arg_string(0), label.kind == js::Kind::number ? number_text(label.number) : label.string, call.arg_number(2)});
    }));
    // An empty source name: the page's own script, visible in stacks.
    const auto recorder = roots.add(handle_of(state.evaluate(top.global, scripts::recorder_script, "")));
    const auto recorded = roots.add(handle_of(state.invoke(recorder, 0, {Value::object(metric_sink)})));
    std::array<Value, 5> callbacks;
    for (std::size_t index = 0; index < callbacks.size(); ++index) callbacks[index] = roots.add(context.get(recorded, std::to_string(index)));
    const auto loggers = roots.add(state.new_array({callbacks[1], callbacks[2], callbacks[3], callbacks[4]}));
    context.release(step("Initialising the WAA program", [&] {
        return state.invoke(initialize.handle, object.handle,
                            {Value::of(options.challenge.program), Value::object(callback), Value::of(true), Value{}, callbacks[0],
                             signals, stored, Value::of(false), loggers});
    }));
}

// The native function that hands (id, result) to the request waiting for it.
Handle Runtime::Impl::new_deliver() {
    return agent->context().new_function(agent->top->global, "deliver", 2, [this](js::Call &call) {
        const double id = call.arg_number(0);
        const auto value = call.arg(1);
        agent->context().release(value);
        const auto found = results.find(id);
        if (found == results.end()) return;
        const auto pending = std::move(found->second);
        results.erase(found);
        auto text = value.kind == js::Kind::string ? value.string : number_text(value.number);
        record({"el", "n", milliseconds_since(pending.started)});
        record({"frs", "", static_cast<double>(text.size())});
        pending.result->finish(std::move(text));
    });
}

// The signal lists and the stored state the page derives from the client
// experiments blob: [values, types], type 1 for keys up to 53 and 2 above.
std::pair<Value, Value> Runtime::Impl::experiment_signals(Handle global) {
    auto &context = agent->context();
    const auto &blob = options.challenge.client_experiments_state_blob;
    if (blob.empty()) return {context.parse_json(global, "[[],[]]"), Value{}};
    boost::system::error_code failed;
    const auto parsed = json::parse(blob, failed);
    if (failed || !parsed.is_array()) throw std::runtime_error("The client experiments blob is not a JSON array");
    const auto &fields = parsed.as_array();
    json::array low, high;
    // A null counts as 0, as it does where the page's rule was taken from.
    const auto number = [](const json::value &value) -> std::optional<double> {
        if (value.is_null()) return 0.0;
        if (value.is_number()) return value.to_number<double>();
        return std::nullopt;
    };
    if (fields.size() > 5 && fields[5].is_array())
        for (const auto &entry : fields[5].as_array()) {
            if (!entry.is_array() || entry.as_array().size() < 2) continue;
            const auto value = number(entry.as_array()[0]), key = number(entry.as_array()[1]);
            if (!value || !key) continue;
            (*key <= 53 ? low : high).emplace_back(*value);
        }
    Value stored;
    if (fields.size() > 4 && fields[4].is_string() && !fields[4].as_string().empty())
        stored = Value::of(std::string(fields[4].as_string()));
    json::array list, types;
    for (const auto &value : low) {
        list.push_back(value);
        types.emplace_back(1);
    }
    for (const auto &value : high) {
        list.push_back(value);
        types.emplace_back(2);
    }
    json::array signals;
    signals.emplace_back(std::move(list));
    signals.emplace_back(std::move(types));
    return {context.parse_json(global, json::serialize(signals)), stored};
}

std::string Runtime::Impl::proof(std::string_view digest, std::optional<std::string_view> prompt, const std::vector<PointerStep> *steps,
                                 std::stop_token cancel) {
    std::lock_guard lock(operation);
    struct After {
        Impl &impl;
        ~After() {
            impl.clear_prompt();
            impl.used();
        }
    } after{*this};
    if (prompt) fill_prompt(textarea_value(*prompt), cancel);
    if (steps) click(*steps, cancel);
    return snapshot_with(std::string(digest), cancel);
}

void Runtime::Impl::fill_prompt(std::string prompt, std::stop_token cancel) {
    const auto result = std::make_shared<Result>();
    if (!loop.post([this, result, prompt = std::move(prompt), cancel] {
            if (cancel.stop_requested()) {
                result->fail("cancelled");
                return;
            }
            try {
                const auto value = agent->invoke(prompt_function, 0, {Value::of(prompt)});
                agent->context().release(value);
                if (value.kind == js::Kind::string && value.string == prompt) result->finish({});
                else result->fail("The page's prompt box did not take the prompt");
            } catch (const js::Exception &exception) {
                agent->release(exception);
                result->fail("Typing the prompt into the page: " + describe(exception));
            } catch (const std::exception &error) {
                result->fail(error.what());
            }
        }))
        throw closed_error();
    result->wait(cancel, loop.closed());
}

void Runtime::Impl::click(const std::vector<PointerStep> &steps, std::stop_token cancel) {
    for (const auto &step : steps) {
        pause(step.delay, cancel, loop.closed());
        const auto result = std::make_shared<Result>();
        pointer_input(step, result);
        result->wait(cancel, loop.closed());
    }
    pause(run_proof_delay, cancel, loop.closed());
}

void Runtime::Impl::pointer_input(const PointerStep &step, std::shared_ptr<Result> result) {
    const bool posted = loop.post([this, step, result] {
        const auto events = pointer->step(step);
        try {
            if (!events.types.empty()) {
                auto &context = agent->context();
                Roots roots(context);
                std::vector<Value> names;
                for (const auto &type : events.types) names.push_back(Value::of(type));
                const auto list = roots.add(agent->new_array(names));
                context.release(agent->invoke(mouse_function, 0,
                                              {Value::object(agent->mouse_input), Value::of(events.from), Value::of(events.to),
                                               Value::of(events.blurred), Value::of(events.focused),
                                               Value::of(static_cast<double>(events.rounds)), list}));
            }
            if (result) result->finish({});
        } catch (const js::Exception &exception) {
            agent->release(exception);
            if (result) result->fail("Dispatching mouse events: " + describe(exception));
        } catch (const std::exception &error) {
            if (result) result->fail(error.what());
        }
    });
    if (!posted && result) throw closed_error();
}

std::string Runtime::Impl::snapshot_with(std::optional<std::string> digest, std::stop_token cancel) {
    const auto result = std::make_shared<Result>();
    if (!loop.post([this, result, digest = std::move(digest), cancel] {
            if (cancel.stop_requested()) {
                result->fail("cancelled");
                return;
            }
            const double id = ++next_result;
            record({"ec", "n", 1});
            results[id] = Pending{result, Clock::now()};
            try {
                auto &context = agent->context();
                Roots roots(context);
                const auto callback = roots.add(handle_of(agent->invoke(bind_function, 0, {Value::object(deliver_function), Value::of(id)})));
                const auto snapshot_options = roots.add(handle_of(agent->invoke(options_function, 0, {digest ? Value::of(*digest) : Value{}})));
                context.release(agent->invoke(snapshot_function, 0, {Value::object(callback), Value::object(snapshot_options)}));
            } catch (const js::Exception &exception) {
                results.erase(id);
                agent->release(exception);
                result->fail("Running the WAA snapshot: " + describe(exception));
            } catch (const std::exception &error) {
                results.erase(id);
                result->fail(error.what());
            }
        }))
        throw closed_error();
    return result->wait(cancel, loop.closed());
}

void Runtime::Impl::clear_prompt() {
    loop.post([this] {
        try {
            agent->context().release(agent->invoke(clear_function, 0));
        } catch (const js::Exception &exception) {
            agent->release(exception);
        }
    });
}

void Runtime::Impl::used() {
    last_used = Clock::now().time_since_epoch().count();
    loop.post([this] {
        if (!std::exchange(idle_armed, true)) watch_idle(idle_shrink_delay);
    });
}

void Runtime::Impl::watch_idle(std::chrono::nanoseconds delay) {
    loop.schedule(delay, [this] {
        const auto idle = Clock::now() - Clock::time_point(Clock::duration(last_used.load()));
        if (idle < idle_shrink_delay) {
            watch_idle(idle_shrink_delay - idle);
            return;
        }
        idle_armed = false;
        agent->context().gc(true);
    });
}

Runtime::Runtime(Options options, std::stop_token cancel) : impl_(std::make_unique<Impl>(std::move(options))) {
    impl_->open(std::move(cancel));
}

Runtime::~Runtime() = default;

void Runtime::settle(std::stop_token cancel) {
    const auto done = std::make_shared<Result>();
    if (!impl_->loop.post([impl = impl_.get(), done] { impl->agent->when_settled([done] { done->finish({}); }); })) throw closed_error();
    done->wait(cancel, impl_->loop.closed());
}

std::string Runtime::proof(std::string_view digest, std::string_view prompt, std::stop_token cancel) {
    return impl_->proof(digest, prompt, nullptr, std::move(cancel));
}

std::string Runtime::run_proof(std::string_view digest, std::string_view prompt, std::stop_token cancel) {
    return impl_->proof(digest, prompt, &run_click(), std::move(cancel));
}

std::string Runtime::rerun_proof(std::string_view digest, int rounds, std::stop_token cancel) {
    impl_->pointer->set_rounds(rounds);
    return impl_->proof(digest, std::nullopt, &rerun_click(), std::move(cancel));
}

void Runtime::replaced() { impl_->pointer_input({std::chrono::milliseconds(0), PointerStep::Action::replace, {}}, nullptr); }

void Runtime::activate(std::stop_token cancel) {
    const auto result = std::make_shared<Result>();
    if (!impl_->loop.post([impl = impl_.get(), result] {
            try {
                impl->agent->context().release(impl->agent->invoke(impl->agent->activate, 0));
                result->finish({});
            } catch (const js::Exception &exception) {
                impl->agent->release(exception);
                result->fail("Recording a user action: " + describe(exception));
            } catch (const std::exception &error) {
                result->fail(error.what());
            }
        }))
        throw closed_error();
    result->wait(cancel, impl_->loop.closed());
}

std::vector<Metric> Runtime::take_metrics(std::stop_token cancel) {
    const auto result = std::make_shared<Result>();
    const auto taken = std::make_shared<std::vector<Metric>>();
    if (!impl_->loop.post([impl = impl_.get(), result, taken] {
            *taken = std::exchange(impl->metrics, {});
            result->finish({});
        }))
        throw closed_error();
    result->wait(cancel, impl_->loop.closed());
    return std::move(*taken);
}

std::string Runtime::refresh_snapshot(std::stop_token cancel) {
    std::lock_guard lock(impl_->operation);
    struct After {
        Impl &impl;
        ~After() { impl.used(); }
    } after{*impl_};
    return impl_->snapshot_with(std::nullopt, std::move(cancel));
}
} // namespace ais2api::waa
