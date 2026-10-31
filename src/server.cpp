// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ais2api/browser.hpp"
#include "ais2api/flow.hpp"
#include "ais2api/retry_policy.hpp"
#include "ais2api/management.hpp"
#include "ais2api/cookie_import.hpp"
#include "ais2api/logs.hpp"
#include "ais2api/log_sources.hpp"
#include "ais2api/responses.hpp"
#include "ais2api/webui.hpp"
#include "ais2api/usage.hpp"
#include "ais2api/price_service.hpp"
#include <charconv>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <algorithm>
#include <csignal>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
namespace ais2api {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;
std::atomic<unsigned> live_connections{0};
struct ConnectionSlot {
    ~ConnectionSlot() { live_connections.fetch_sub(1); }
};
struct Work {
    Protocol protocol = Protocol::gemini;
    std::string model, principal;
    bool stream = false, include_usage = false, responses = false;
    std::size_t input_bytes = 0;
    json::object body, response_options;
    std::shared_ptr<EventPipe> pipe;
    std::shared_ptr<RequestUsage> usage;
    std::shared_ptr<net::steady_timer> wakeup;
    Clock::time_point queued = Clock::now();
};
struct Task {
    std::function<void()> execute;
    std::function<void()> cancel;
};
// Time reserved after the account scan for persisting accounts and replying.
constexpr auto cookie_import_commit_margin = 5s;
struct CookieImportJob {
    Config config;
    json::object input;
    std::string principal, request_id = "cookie-" + random_hex(12);
    Clock::time_point queued = Clock::now(), started_at;
    std::stop_source cancellation;
    std::atomic<bool> started{false}, finished{false};
    bool admin = false;
    std::promise<json::object> completion;
    std::future<json::object> result = completion.get_future();
    CookieImportJob(Config cfg, json::object body, std::string owner)
        : config(std::move(cfg)), input(std::move(body)), principal(std::move(owner)) {}
    void resolve(json::object value) {
        if (!finished.exchange(true)) completion.set_value(std::move(value));
    }
    void reject(const Error &error) {
        if (!finished.exchange(true)) completion.set_exception(std::make_exception_ptr(error));
    }
};
class Service {
  public:
    explicit Service(Config config, std::unique_ptr<GenerationBackend> backend)
        : config_(std::move(config)), journal_(config_.data_dir), store_(config_.data_dir),
          prices_(store_, [this] { return this->config(); }, !backend),
          gateway_(backend ? std::move(backend)
                           : std::make_unique<BrowserGateway>(config_, store_)),
          worker_([this](std::stop_token token) { work_loop(token); }) { store_.recover_requests(); }
    ~Service() { shutdown(); log_writer_.join(); }
    void shutdown() {
        stop();
        worker_.request_stop();
        available_.notify_all();
        if (worker_.joinable())
            worker_.join();
        gateway_->close();
        prices_.stop();
    }
    Config config() {
        std::lock_guard lock(config_mutex_);
        return config_;
    }
    Store &store() { return store_; }
    PriceService &prices() { return prices_; }
    void usage_failure(std::string_view id) {
        usage_healthy_ = false;
        log({{"event", "request_history_error"}, {"request_id", id}, {"code", "request_history_write_failed"}});
    }
    std::optional<std::string> authenticate_generation(std::string_view token) {
        if (auto principal = config().authenticate(token)) return principal;
        return store_.authenticate_managed(token);
    }
    logs::Journal& journal() { return journal_; }
    bool log_backlogged() const { return pending_logs_.load() >= 256; }
    void log(json::object record) {
        // Keep fsync and stderr backpressure off the single HTTP reactor.
        // Admission pauses on backlog; all already admitted work keeps its logs.
        // Records queued while the writer is busy share one open and one flush.
        pending_logs_.fetch_add(1);
        bool idle = false;
        {
            std::lock_guard lock(log_queue_mutex_);
            idle = log_queue_.empty();
            log_queue_.push_back(std::move(record));
        }
        if (idle) net::post(log_writer_, [this] { drain_logs(); });
    }
    void drain_logs() {
        std::vector<json::object> batch;
        {
            std::lock_guard lock(log_queue_mutex_);
            batch.swap(log_queue_);
        }
        const auto count = static_cast<unsigned>(batch.size());
        if (!count) return;
        try { journal_.append(std::move(batch)); }
        catch (...) { logging_healthy_ = false; }
        pending_logs_.fetch_sub(count);
    }
    void forget_operation(const std::string& id) {
        std::lock_guard lock(operations_mutex_);
        operations_.erase(id);
    }
    bool stopping() const { return stopping_; }
    void stop() {
        stopping_ = true;
        shutdown_.request_stop();
        std::lock_guard lock(queue_mutex_);
        if (active_)
            active_->cancel();
        for (auto &task : queue_)
            task.cancel();
        available_.notify_all();
    }
    bool enqueue(const std::shared_ptr<Work> &work) {
        auto cfg = config();
        std::lock_guard lock(queue_mutex_);
        if (stopping_ || queue_.size() >= cfg.limits.queue_size)
            return false;
        auto weak = std::weak_ptr<Work>(work);
        queue_.push_back({[this, weak] {
                              if (auto pending = weak.lock())
                                  generate(pending);
                          },
                          [this, weak] {
                              auto work = weak.lock();
                              if (!work)
                                  return;
                              work->pipe->cancel();
                              work->pipe->finish(Error(503, "stopping", "Service is stopping"));
                              try { work->usage->producer_done("stopping"); }
                              catch (const std::exception &) { usage_failure(work->usage->id()); }
                          }});
        available_.notify_one();
        return true;
    }
    json::object model_catalog() {
        auto saved = store_.model_catalog();
        auto out = std::move(saved.models);
        const bool persisted = !out.empty();
        if (out.empty()) {
            auto cfg = config();
            for (auto &item : cfg.configured_models) {
                if (item.is_string())
                    out.push_back(json::object{{"name", "models/" + std::string(item.as_string())},
                                               {"source", "configured"}});
                else if (item.is_object()) {
                    auto o = item.as_object();
                    o["source"] = "configured";
                    out.push_back(std::move(o));
                }
            }
        }
        const auto source = persisted ? "saved" : out.empty() ? "none" : "configured";
        return {{"models", std::move(out)}, {"source", source},
                {"account_id", saved.account_id.empty() ? json::value(nullptr) : json::value(saved.account_id)},
                {"saved_at", saved.saved_at ? json::value(saved.saved_at) : json::value(nullptr)}};
    }
    json::object control(const json::object &command) {
        auto action = string_or(command, "command");
        if (action == "status") {
            std::lock_guard lock(queue_mutex_);
            return {{"running", true},
                    {"version", "0.1.0"},
                    {"logging_healthy", logging_healthy_.load()},
                    {"usage_healthy", usage_healthy_.load()},
                    {"pending_logs", pending_logs_.load()},
                    {"uptime_seconds", std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-started_).count()},
                    {"active", bool(active_)},
                    {"active_generation_account_id", active_account_id_.empty() ? json::value(nullptr) : json::value(active_account_id_)},
                    {"queued", queue_.size()},
                    {"connections", live_connections.load()},
                    {"ready", (!config().api_key.empty() || !store_.managed_keys().empty()) && store_.choose("readiness").has_value() &&
                                  fs::is_regular_file(config().browser_executable)},
                    {"verification", "not_a_generation_check"}};
        }
        if (action == "stop") {
            stop();
            return {{"status", "stopping"}};
        }
        if (action == "operation.status") {
            std::lock_guard lock(operations_mutex_);
            auto it = operations_.find(string_or(command, "id"));
            if (it == operations_.end())
                throw Error(404, "operation_missing", "Unknown or expired operation");
            return it->second.result;
        }
        if (action == "config.reload") {
            std::lock_guard update_lock(config_update_mutex_);
            auto previous = config();
            auto next = Config::load(previous.config_file);
            if (restart_required(previous, next))
                throw Error(
                    409, "restart_required",
                    "Browser, listener, storage and transport-limit changes require a restart");
            std::lock_guard lock(config_mutex_);
            config_ = std::move(next);
            pending_config_.reset();
            return {{"status", "reloaded"}};
        }
        if (action == "config.patch") {
            std::lock_guard update_lock(config_update_mutex_);
            auto value = command.if_contains("patch");
            if (!value || !value->is_object())
                throw Error(400, "invalid_config", "patch must be an object");
            const auto previous = config();
            const auto next = (pending_config_ ? *pending_config_ : previous).patched(value->as_object());
            const bool restart = restart_required(previous, next);
            next.save();
            if (!restart) {
                std::lock_guard lock(config_mutex_);
                config_ = next;
                pending_config_.reset();
            } else {
                pending_config_ = next;
            }
            log({{"event", "config_saved"}, {"restart_required", restart}});
            return {{"saved", true}, {"applied", !restart}, {"restart_required", restart}};
        }
        if (action == "admin.password") {
            // Applies immediately even while restart-only changes are pending:
            // the new password goes into both the running configuration and the
            // saved file, without applying anything else that awaits a restart.
            std::lock_guard update_lock(config_update_mutex_);
            const auto password = string_or(command, "password");
            auto running = config();
            validate_admin_password(password, running.admin_key);
            running.admin_key = password;
            if (pending_config_) {
                auto pending = *pending_config_;
                pending.admin_key = password;
                pending.save();
                pending_config_ = std::move(pending);
            } else {
                running.save();
            }
            {
                std::lock_guard lock(config_mutex_);
                config_.admin_key = password;
            }
            log({{"event", "admin_password_changed"}});
            return {{"changed", true}};
        }
        if (action == "config.validate_patch") {
            std::lock_guard update_lock(config_update_mutex_);
            auto current = config();
            auto patch = command.if_contains("patch");
            if (!patch || !patch->is_object()) throw Error(400, "invalid_config", "patch must be an object");
            auto next = (pending_config_ ? *pending_config_ : current).patched(patch->as_object());
            return {{"valid", true}, {"restart_required", restart_required(current, next)}};
        }
        if (action == "config.show") {
            std::lock_guard update_lock(config_update_mutex_);
            auto current = config();
            auto result = manage(store_, current, command);
            result["restart_required"] = pending_config_.has_value();
            if (pending_config_) {
                auto pending = *pending_config_;
                result["saved_config"] = manage(store_, pending, command).at("config");
            }
            return result;
        }
        if (action == "rotation.status") return store_.rotation_status();
        if (action == "browser.diagnostics") return gateway_->diagnostics();
        if (action == "rotation.next") return {{"preferred_account_id", store_.rotate_account()},
            {"browser_prepared", false}};
        if (action == "rotation.clear") {
            store_.clear_cooldown(string_or(command, "id"), string_or(command, "model"));
            return {{"ok", true}};
        }
        if (action == "stats.reset") { store_.reset_stats(); return {{"ok", true}}; }
        if (action == "models.list" || action == "accounts.probe")
            return maintenance(command);
        // HTTP may reach only the explicitly allowlisted management routes,
        // behind the separate admin credential. IPC remains owner-only.
        auto cfg = config();
        return manage(store_, cfg, command);
    }

    std::shared_ptr<CookieImportJob> enqueue_cookie_import(json::object input,
                                                         std::string principal, bool admin = false) {
        auto cfg = config();
        if ((admin ? cfg.authenticate_admin(cfg.admin_key) : cfg.authenticate_cookie_import(cfg.cookie_import_key)) != principal)
            throw Error(401, "cookie_import_authentication", "A valid Cookie import key is required");
        auto job = std::make_shared<CookieImportJob>(std::move(cfg), std::move(input), std::move(principal));
        job->admin = admin;
        auto weak = std::weak_ptr<CookieImportJob>(job);
        std::lock_guard lock(queue_mutex_);
        if (stopping_ || queue_.size() >= job->config.limits.queue_size)
            throw Error(429, "queue_full", "Work queue is full");
        queue_.push_back({[this, weak] {
            auto job = weak.lock();
            if (!job) return;
            job->started_at = Clock::now();
            job->started.store(true, std::memory_order_release);
            std::stop_callback shutdown(shutdown_.get_token(), [&] { job->cancellation.request_stop(); });
            try {
                check_cancel(job->cancellation.get_token());
                if (job->started_at - job->queued > std::chrono::seconds(job->config.limits.queue_seconds))
                    throw Error(504, "queue_timeout", "Cookie import queue wait exceeded limit");
                gateway_->reconfigure(job->config);
                // Stop scanning early enough to save verified accounts and
                // answer before the HTTP handler's timeout discards them.
                const auto scan_deadline = job->started_at +
                    std::chrono::seconds(job->config.cookie_import_timeout_seconds) - cookie_import_commit_margin;
                auto verified = probe_cookie_import(job->input, job->config.cookie_import_max_accounts,
                                                    *gateway_, job->cancellation.get_token(), scan_deadline);
                check_cancel(job->cancellation.get_token());
                json::array accounts;
                {
                    // Rotation cannot authorize a queued upload with a revoked
                    // import key, including the final credentials transaction.
                    std::lock_guard config_lock(config_mutex_);
                    if ((job->admin ? config_.admin_key : config_.cookie_import_key).empty())
                        throw Error(403, "cookie_import_disabled", "Cookie import was disabled before verification completed");
                    if ((job->admin ? config_.authenticate_admin(job->config.admin_key)
                                   : config_.authenticate_cookie_import(job->config.cookie_import_key)) != job->principal)
                        throw Error(401, "cookie_import_authentication", "Cookie import key changed; submit again");
                    check_cancel(job->cancellation.get_token());
                    accounts = store_.merge_verified_accounts(verified.accounts);
                }
                try { gateway_->close(); } catch (...) {}
                log(json::object{{"event", "cookie_import"},
                    {"request_id", job->request_id}, {"imported_count", accounts.size()},
                    {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-job->queued).count()}});
                const auto count = accounts.size();
                job->resolve({{"imported_count", count}, {"accounts", std::move(accounts)},
                              {"verification", "browser_identity_only"}, {"generation_verified", false},
                              {"details", std::move(verified.details)}});
            } catch (const Error &error) {
                try { gateway_->close(); } catch (...) {}
                log(json::object{{"event", "cookie_import_failed"},
                    {"request_id", job->request_id}, {"reason", error.code}, {"status", error.status},
                    {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-job->queued).count()}});
                job->reject(error);
            } catch (...) {
                try { gateway_->close(); } catch (...) {}
                job->reject(Error(500, "cookie_import_failed", "Cookie import failed internally"));
            }
        }, [weak] {
            if (auto job = weak.lock()) {
                job->cancellation.request_stop();
                job->reject(Error(503, "stopping", "Service stopped"));
            }
        }});
        available_.notify_one();
        return job;
    }

  private:
    struct Operation {
        json::object result;
        Clock::time_point created;
    };
    Config config_;
    std::mutex config_mutex_, config_update_mutex_;
    std::optional<Config> pending_config_;
    logs::Journal journal_;
    net::thread_pool log_writer_{1};
    std::atomic<unsigned> pending_logs_{0};
    std::mutex log_queue_mutex_;
    std::vector<json::object> log_queue_;
    std::atomic<bool> logging_healthy_{true};
    Store store_;
    PriceService prices_;
    std::atomic<bool> usage_healthy_{true};
    std::unique_ptr<GenerationBackend> gateway_;
    std::mutex queue_mutex_, operations_mutex_;
    std::condition_variable available_;
    std::deque<Task> queue_;
    std::shared_ptr<EventPipe> active_;
    std::string active_account_id_;
    Clock::time_point started_ = Clock::now();
    std::map<std::string, Operation> operations_;
    std::atomic<bool> stopping_{false};
    std::stop_source shutdown_;
    std::jthread worker_;
    void work_loop(std::stop_token token) {
        // All browser work runs on this thread, so its idle time is the
        // browser's. `used`: a task ran since the browser was last released.
        auto last_work = Clock::now();
        bool used = false;
        while (!token.stop_requested()) {
            Task task;
            const auto idle = std::chrono::seconds(config().browser_idle_seconds);
            {
                std::unique_lock lock(queue_mutex_);
                // Wake at least every 30 s so a reloaded setting takes effect.
                auto wake = Clock::now() + 30s;
                if (used && idle.count()) wake = std::min(wake, last_work + idle);
                if (!available_.wait_until(lock, wake, [&] { return token.stop_requested() || stopping_ || !queue_.empty(); })) {
                    lock.unlock();
                    if (used && idle.count() && Clock::now() >= last_work + idle) {
                        used = false;
                        release_idle_browser(idle);
                    }
                    continue;
                }
                if (token.stop_requested() || stopping_)
                    break;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                task.execute();
            } catch (...) {
                task.cancel();
            }
            last_work = Clock::now();
            used = true;
        }
    }
    void release_idle_browser(std::chrono::seconds idle) {
        try {
            if (gateway_->release_idle())
                log({{"event", "browser_idle_released"}, {"idle_seconds", idle.count()}});
        } catch (...) {}
    }
    json::object maintenance(const json::object &command) {
        auto cfg = config();
        auto id = random_hex(12);
        {
            std::lock_guard lock(operations_mutex_);
            for (auto i = operations_.begin(); i != operations_.end();)
                if (Clock::now() - i->second.created > 15min)
                    i = operations_.erase(i);
                else
                    ++i;
            if (operations_.size() >= 32)
                throw Error(429, "operations_full", "Too many retained control operations");
            operations_[id] = {{{"status", "queued"}, {"operation_id", id}}, Clock::now()};
        }
        auto finish = [this, id](json::object result) {
            std::lock_guard lock(operations_mutex_);
            auto it = operations_.find(id);
            if (it != operations_.end())
                it->second.result = std::move(result);
        };
        std::lock_guard lock(queue_mutex_);
        if (stopping_ || queue_.size() >= cfg.limits.queue_size) {
            std::lock_guard operation_lock(operations_mutex_);
            operations_.erase(id);
            throw Error(429, "queue_full", "Work queue is full");
        }
        queue_.push_back(
             {[this, command, finish, id] {
                 try {
                     log({{"event", "maintenance_started"}, {"operation_id", id},
                         {"action", string_or(command, "command")}});
                     json::object result;
                     gateway_->reconfigure(config());
                     if (string_or(command, "command") == "models.list") {
                         auto catalog = store_.model_catalog();
                         if (catalog.account_id.empty())
                             throw Error(503, "no_available_account", "No account for model discovery");
                         const auto account = store_.account(catalog.account_id);
                         if (account.disabled)
                             throw Error(503, "no_available_account", "The model discovery account is disabled");
                         auto values = gateway_->models(account, shutdown_.get_token());
                         check_cancel(shutdown_.get_token());
                         auto saved = store_.save_models(account.id, std::move(values));
                         result = {{"models", std::move(saved.models)}, {"source", "saved"},
                                   {"account_id", saved.account_id}, {"saved_at", saved.saved_at}};
                     } else {
                         json::array results;
                         auto id = string_or(command, "id");
                         auto accounts = id.empty() ? store_.accounts()
                                                    : std::vector<Account>{store_.account(id)};
                         for (auto &a : accounts) {
                             check_cancel(shutdown_.get_token());
                             try {
                                 auto value = gateway_->probe(a, shutdown_.get_token());
                                 const auto email = string_or(value, "email");
                                 if (!email.empty() && bool_or(value, "ready") && !bool_or(value, "identityAmbiguous"))
                                     store_.update_email(a.id, email);
                                 value["account_id"] = a.id;
                                 results.push_back(std::move(value));
                             } catch (const Error &e) {
                                 // Only a sign-in redirect or missing credentials
                                 // prove expiry; other auth failures cool down.
                                 if (e.code == "session_expired" || e.code == "missing_cookies")
                                     store_.failure(a.id, "probe", "login_expired");
                                 else if (e.status == 401)
                                     store_.failure(a.id, "probe", "auth");
                                 if (!id.empty() || e.code == "cancelled")
                                     throw;
                                 results.push_back(
                                     json::object{{"account_id", a.id},
                                                  {"status", "failed"},
                                                  {"error", json::object{{"status", e.status},
                                                                         {"code", e.code},
                                                                         {"message", e.what()}}}});
                             }
                         }
                         result = {{"accounts", std::move(results)}};
                     }
                     finish({{"status", "done"}, {"result", std::move(result)}});
                     log({{"event", "maintenance_done"}, {"operation_id", id}});
                 } catch (const Error &e) {
                     log({{"event", "maintenance_failed"}, {"operation_id", id}, {"code", e.code}});
                     finish({{"status", "failed"},
                             {"error",
                              {{"status", e.status}, {"code", e.code}, {"message", e.what()}}}});
                 } catch (...) {
                     finish({{"status", "failed"},
                             {"error",
                              {{"status", 500},
                               {"code", "control_failed"},
                               {"message", "Control operation failed"}}}});
                 }
             },
             [finish] {
                 finish(
                     {{"status", "failed"},
                      {"error",
                       {{"status", 503}, {"code", "stopping"}, {"message", "Service stopped"}}}});
             }});
        available_.notify_one();
        return {{"operation_id", id}, {"status", "queued"}};
    }
    void prepare_media(json::object &body, const Config &cfg, std::stop_token token) {
        auto messages = body.if_contains("messages");
        if (!messages || !messages->is_array())
            return;
        auto size = json::serialize(body).size();
        if (size > cfg.limits.request_bytes)
            throw Error(413, "media_limit", "Normalized media request exceeds the request budget");
        for (auto &message : messages->as_array())
            if (message.is_object()) {
                auto content = message.as_object().if_contains("content");
                if (!content || !content->is_array())
                    continue;
                for (auto &part : content->as_array())
                    if (part.is_object() && string_or(part.as_object(), "type") == "image_url") {
                        check_cancel(token);
                        auto image = part.as_object().if_contains("image_url");
                        if (!image || !image->is_object())
                            continue;
                        auto url = string_or(image->as_object(), "url");
                        if (url.starts_with("https://")) {
                            auto remaining = cfg.limits.request_bytes - size + url.size();
                            if (remaining <= 128)
                                throw Error(413, "media_limit",
                                            "Remote images exceed the cumulative request budget");
                            auto uri = fetch_public_media(url, (remaining - 128) / 4 * 3, cfg.proxy,
                                                          token);
                            if (uri.size() > remaining)
                                throw Error(413, "media_limit",
                                            "Remote images exceed the cumulative request budget");
                            size = size - url.size() + uri.size();
                            image->as_object()["url"] = std::move(uri);
                        }
                    }
            }
    }
    void generate(const std::shared_ptr<Work> &work) {
        if (!work->usage->start()) return;
        auto cfg = config();
        auto token = work->pipe->cancellation.get_token();
        struct Trace {
            const Work &work;
            Service& service;
            Clock::time_point start = Clock::now(), normalized = start;
            std::string error;
            unsigned attempts = 0;
            // Upstream wire locations skipped while decoding, across attempts.
            json::array upstream_ignored;
            ~Trace() {
                try {
                    // Only wire locations are logged, never their content.
                    if (!upstream_ignored.empty())
                        service.log({{"event", "upstream_fields_ignored"}, {"request_id", work.usage->id()},
                                     {"locations", upstream_ignored}});
                    auto elapsed = [](auto begin, auto end) {
                        return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
                            .count();
                    };
                    service.log(json::object{
                                     {"event", "generation"},
                                     {"request_id", work.usage->id()},
                                     {"model", work.model},
                                     {"stream", work.stream},
                                     {"queue_ms", elapsed(work.queued, start)},
                                     {"normalize_ms", elapsed(start, normalized)},
                                     {"generation_ms", elapsed(normalized, Clock::now())},
                                     {"input_bytes", work.input_bytes},
                                     {"event_bytes", work.pipe->produced_bytes.load()},
                                     {"attempts", attempts},
                                     {"error", error}});
                } catch (...) {
                }
            }
        } trace{*work, *this};
        {
            std::lock_guard lock(queue_mutex_);
            active_ = work->pipe;
        }
        struct Clear {
            Service *s;
            ~Clear() {
                std::lock_guard lock(s->queue_mutex_);
                s->active_.reset();
                s->active_account_id_.clear();
            }
        } clear{this};
        work->pipe->started = true;
        try {
            check_cancel(token);
            gateway_->reconfigure(cfg);
            if (Clock::now() - work->queued > std::chrono::seconds(cfg.limits.queue_seconds))
                throw Error(504, "queue_timeout", "Queue wait exceeded limit");
            json::array ignored_responses;
            if (work->responses) {
                if (cfg.ignore_unsupported_parameters)
                    discard_unsupported_responses(work->body, ignored_responses);
                work->response_options = responses_options(work->body);
                work->body = responses_to_chat(work->body);
            }
            if (work->protocol == Protocol::openai)
                prepare_media(work->body, cfg, token);
            auto request = work->protocol == Protocol::gemini
                               ? normalize_gemini(work->model, work->body, work->stream, cfg)
                               : normalize_openai(work->body, cfg, &store_, work->principal);
            for (auto &parameter : ignored_responses)
                request.ignored_parameters.push_back(parameter);
            request.principal = work->principal;
            work->model = request.model;
            work->include_usage = request.include_usage;
            work->usage->normalized(request.model, request.gemini);
            if (!request.ignored_parameters.empty())
                log({{"event", "generation_parameters_ignored"}, {"request_id", work->usage->id()},
                     {"parameters", request.ignored_parameters},
                     {"reason", "No verified private upstream mapping"}});
            trace.normalized = Clock::now();
            json::object().swap(work->body);
            std::vector<std::string> excluded;
            std::optional<Error> previous;
            std::optional<Account> retry_account;
            bool delivered = false;
            // One in-place session rebuild or browser restart per request.
            bool recovered = false;
            for (unsigned attempt = 0; attempt < cfg.limits.max_attempts; ++attempt) {
                trace.attempts = attempt + 1;
                check_cancel(token);
                auto account = retry_account ? std::exchange(retry_account, std::nullopt) : store_.choose(request.model, excluded, true);
                if (!account) {
                    if (previous)
                        throw *previous;
                    throw Error(503, "no_account",
                                "No usable account for this model; import or reset credentials "
                                "with the CLI");
                }
                work->usage->begin_attempt(account->id);
                { std::lock_guard lock(queue_mutex_); active_account_id_ = account->id; }
                log({{"event", "generation_attempt"}, {"request_id", work->usage->id()},
                    {"account_id", account->id}, {"attempt", attempt+1}});
                try {
                    bool received = false;
                    bool completed = false;
                    gateway_->generate(
                        request, *account,
                        [&](json::object event) {
                            work->usage->event(event, has_content(event));
                            check_cancel(token);
                            for (auto &location : take_upstream_ignored(event))
                                if (std::find(trace.upstream_ignored.begin(), trace.upstream_ignored.end(), location) ==
                                    trace.upstream_ignored.end())
                                    trace.upstream_ignored.push_back(location);
                            if (event.empty())
                                return;
                            received = true;
                            if (has_content(event))
                                delivered = true;
                            if (auto candidates = event.if_contains("candidates"); candidates && candidates->is_array())
                                for (auto &candidate : candidates->as_array())
                                    if (candidate.is_object()) {
                                        const auto reason = string_or(candidate.as_object(), "finishReason");
                                        if (!reason.empty() && reason != "FINISH_REASON_UNSPECIFIED") completed = true;
                                    }
                            if (!request.include_thoughts)
                                filter_thoughts(event);
                            work->pipe->push(
                                std::move(event),
                                Clock::now() + std::chrono::seconds(
                                                   request.stream ? cfg.limits.stream_idle_seconds
                                                                  : cfg.limits.generation_seconds));
                        },
                        token);
                    if (!received)
                        throw Error(502, "empty_upstream",
                                    "Upstream returned no generation events");
                    if (!completed)
                        throw Error(502, "incomplete_upstream", "Upstream response ended without a finish reason");
                    store_.success(account->id, request.model);
                    work->usage->end_attempt();
                    work->usage->producer_done();
                    work->pipe->finish();
                    return;
                } catch (const Error &e) {
                    work->usage->end_attempt(e.code);
                    if (token.stop_requested() || delivered)
                        throw;
                    const bool last = attempt + 1 == cfg.limits.max_attempts;
                    auto decision = classify_failure(e, attempt, recovered);
                    // Without an attempt left a recovery cannot run; judge the
                    // failure as if the recovery had already failed.
                    if (last && (decision.action == FailureAction::rebuild_session ||
                                 decision.action == FailureAction::restart_browser))
                        decision = classify_failure(e, attempt, true);
                    switch (decision.action) {
                    case FailureAction::fail:
                        throw;
                    case FailureAction::retry_same: {
                        if (last) throw;
                        // generate() re-encodes the body and obtains a new snapshot.
                        retry_account = account;
                        if (decision.delay.count()) {
                            std::mutex wait_mutex;
                            std::condition_variable_any wake;
                            std::unique_lock wait_lock(wait_mutex);
                            wake.wait_for(wait_lock, token, decision.delay, [] { return false; });
                            check_cancel(token);
                        }
                        continue;
                    }
                    case FailureAction::refresh_template:
                        if (last) throw;
                        // The page captures a new template on the next attempt;
                        // the request keeps its single session recovery.
                        log({{"event", "template_refresh"}, {"request_id", work->usage->id()},
                             {"account_id", account->id}, {"code", e.code}});
                        gateway_->clear_template();
                        retry_account = account;
                        continue;
                    case FailureAction::rebuild_session:
                    case FailureAction::restart_browser: {
                        recovered = true;
                        const bool rebuild = decision.action == FailureAction::rebuild_session;
                        log({{"event", "session_recovery"}, {"request_id", work->usage->id()},
                             {"account_id", account->id}, {"code", e.code},
                             {"action", rebuild ? "rebuild_session" : "restart_browser"}});
                        if (rebuild) gateway_->reset_session();
                        else gateway_->close();
                        retry_account = account;
                        continue;
                    }
                    case FailureAction::cool_and_switch:
                        store_.failure(account->id, request.model, decision.cooldown, e.retry_after);
                        log({{"event", "account_failure"}, {"request_id", work->usage->id()},
                             {"account_id", account->id}, {"kind", decision.cooldown}, {"code", e.code}});
                        previous = e;
                        excluded.push_back(account->id);
                        if (last) throw;
                        // The next account's session replaces this context; the
                        // browser process itself stays up.
                        continue;
                    }
                }
            }
            throw Error(502, "generation_failed", "Generation failed");
        } catch (const Error &e) {
            trace.error = e.code;
            if (e.code == "browser_incompatible") {
                // Only stable diagnostics, never raw CDP exceptions or page data.
                try {
                    auto diagnostics = gateway_->diagnostics();
                    json::object record{{"event", "browser_failure"}, {"request_id", work->usage->id()}, {"code", e.code}};
                    for (auto key : {"phase", "bridgeSource", "bridgeSha256", "status", "templateCaptured"})
                        if (auto value = diagnostics.if_contains(key)) record[key] = *value;
                    log(std::move(record));
                } catch (...) {}
            }
            work->pipe->finish(e);
            try { work->usage->producer_done(e.code); }
            catch (const std::exception &) { usage_failure(work->usage->id()); }
        } catch (const std::exception &) {
            trace.error = "internal";
            work->pipe->finish(Error(500, "internal", "Generation failed internally"));
            try { work->usage->producer_done("internal"); }
            catch (const std::exception &) { usage_failure(work->usage->id()); }
        }
    }
};
std::string decode_url(std::string_view source) {
    std::string out;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '%' && i + 2 < source.size()) {
            unsigned value = 0;
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            auto a = hex(source[i + 1]), b = hex(source[i + 2]);
            if (a < 0 || b < 0)
                throw Error(400, "bad_url", "Invalid URL encoding");
            value = static_cast<unsigned>(a * 16 + b);
            if (!value)
                throw Error(400, "bad_url", "NUL in URL");
            out += static_cast<char>(value);
            i += 2;
        } else
            out += source[i] == '+' ? ' ' : source[i];
    }
    return out;
}
bool ascii_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return std::equal(a.begin(), a.end(), b.begin(),
                      [&](unsigned char x, unsigned char y) { return lower(x) == lower(y); });
}
std::string_view bearer_token(std::string_view value) {
    auto space = value.find(' ');
    if (space == std::string_view::npos || !ascii_equal(value.substr(0, space), "Bearer"))
        return {};
    value.remove_prefix(space);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    return value;
}
bool json_content_type(std::string_view value) {
    value = value.substr(0, value.find(';'));
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return ascii_equal(value, "application/json");
}
std::string normalize_gemini_path(std::string_view path) {
    // SillyTavern appends /v1beta even to an already versioned proxy URL.
    // Only normalize the version prefix; resource ids and other routes stay literal.
    auto resource = path;
    while (resource.starts_with("/v1beta/")) {
        resource.remove_prefix(std::string_view("/v1beta").size());
        while (resource.starts_with("//")) resource.remove_prefix(1);
    }
    if (resource != path && (resource == "/models" || resource.starts_with("/models/")))
        return "/v1beta" + std::string(resource);
    return std::string(path);
}
std::string token_from(const http::request<http::string_body> &request) {
    std::string_view target(request.target().data(), request.target().size());
    auto query = target.find('?');
    if (query != std::string_view::npos) {
        auto tail = target.substr(query + 1);
        while (!tail.empty()) {
            auto end = tail.find('&');
            auto pair = tail.substr(0, end);
            auto eq = pair.find('=');
            if (pair.substr(0, eq) == "key" && eq != std::string_view::npos)
                return decode_url(pair.substr(eq + 1));
            if (end == std::string_view::npos)
                break;
            tail.remove_prefix(end + 1);
        }
    }
    for (auto header : {"x-goog-api-key", "x-api-key"}) {
        auto it = request.find(header);
        if (it != request.end())
            return std::string(it->value());
    }
    return std::string(bearer_token(request[http::field::authorization]));
}
net::awaitable<void> send_json(beast::tcp_stream &stream, unsigned status,
                               const json::object &object, std::string_view origin = {}) {
    http::response<http::string_body> response(static_cast<http::status>(status), 11);
    response.set(http::field::server, "ais2api");
    response.set(http::field::content_type, "application/json; charset=utf-8");
    response.set(http::field::cache_control, "no-store");
    if (!origin.empty()) {
        response.set(http::field::access_control_allow_origin, origin);
        response.set(http::field::vary, "Origin");
    }
    response.keep_alive(false);
    response.body() = json::serialize(object);
    response.prepare_payload();
    stream.expires_after(15s);
    co_await http::async_write(stream, response, net::use_awaitable);
}
net::awaitable<void> json_sse(beast::tcp_stream &stream, const json::object &object,
                              std::size_t window, bool named = false) {
    auto write = [&](std::string_view part) -> net::awaitable<void> {
        stream.expires_after(30s);
        co_await net::async_write(stream, http::make_chunk(net::buffer(part.data(), part.size())),
                                  net::use_awaitable);
    };
    // One HTTP chunk per event (more only for events larger than the relay
    // window), so a client never waits on a partial event.
    std::string pending = named ? "event: " + string_or(object, "type") + "\ndata: " : "data: ";
    json::serializer serializer;
    serializer.reset(&object);
    std::vector<char> block(std::max<std::size_t>(1, std::min<std::size_t>(16384, window)));
    while (!serializer.done()) {
        pending.append(serializer.read(block.data(), block.size()));
        if (pending.size() >= block.size()) {
            co_await write(pending);
            pending.clear();
        }
    }
    pending += "\n\n";
    co_await write(pending);
}
net::awaitable<std::optional<json::object>> next_event(const std::shared_ptr<Work> &work,
                                                       unsigned queue_seconds) {
    while (true) {
        // Arm before inspecting the pipe. Producer notifications are posted to
        // this executor, so no notification can be lost before async_wait starts.
        work->wakeup->expires_at(work->pipe->started
                                     ? Clock::time_point::max()
                                     : work->queued + std::chrono::seconds(queue_seconds));
        check_cancel(work->pipe->cancellation.get_token());
        if (auto event = work->pipe->take())
            co_return event;
        if (work->pipe->done()) {
            if (auto error = work->pipe->error())
                throw *error;
            co_return std::nullopt;
        }
        if (!work->pipe->started &&
            Clock::now() - work->queued > std::chrono::seconds(queue_seconds)) {
            work->pipe->cancel();
            throw Error(504, "queue_timeout", "Queue wait exceeded limit");
        }
        boost::system::error_code ec;
        co_await work->wakeup->async_wait(net::redirect_error(net::use_awaitable, ec));
        if (ec && ec != net::error::operation_aborted)
            throw boost::system::system_error(ec);
    }
}
struct ConnectionState {
    bool committed = false;
    bool openai = false;
    bool audit = true;
    unsigned response_status = 200;
    std::string origin, category = "http";
    std::unique_ptr<ResponsesStream> responses_stream;
    std::shared_ptr<RequestUsage> usage;
};
std::map<std::string, std::string> query_parameters(std::string_view target) {
    std::map<std::string, std::string> result;
    auto begin = target.find('?');
    if (begin == std::string_view::npos) return result;
    auto tail = target.substr(begin + 1);
    while (!tail.empty()) {
        auto end = tail.find('&');
        auto pair = tail.substr(0, end);
        auto eq = pair.find('=');
        auto name = decode_url(pair.substr(0, eq));
        if (!result.emplace(name, eq == std::string_view::npos ? "" : decode_url(pair.substr(eq+1))).second)
            throw Error(400, "bad_query", "Duplicate query parameter");
        if (end == std::string_view::npos) break;
        tail.remove_prefix(end+1);
    }
    return result;
}
json::object admin_body(const http::request<http::string_body>& request) {
    if (request.body().empty()) return {};
    auto type = std::string_view(request[http::field::content_type]);
    if (!json_content_type(type))
        throw Error(415, "content_type", "Content-Type must be application/json");
    boost::system::error_code ec;
    auto parsed = json::parse(request.body(), ec);
    if (ec || !parsed.is_object()) throw Error(400, "invalid_json", "Expected a JSON object");
    return std::move(parsed.as_object());
}
std::string admin_token(const http::request<http::string_body>& request) {
    return std::string(bearer_token(request[http::field::authorization]));
}
void admin_origin(const Config& config, const http::request<http::string_body>& request,
                  ConnectionState& state) {
    auto origin = std::string(request[http::field::origin]);
    if (origin.empty()) return;
    const auto host = std::string(request[http::field::host]);
    bool allowed = origin == "http://" + host;
    for (const auto& value : config.admin_origins)
        allowed = allowed || (value.is_string() && value.as_string() == origin);
    if (!allowed) throw Error(403, "admin_origin", "Management origin is not allowed");
    state.origin = std::move(origin);
}
// Admission limits total live request bytes before reading bodies. Browser/JSON
// objects have separate bounds, and the scheduler admits one generation at once.
std::atomic<std::size_t> admitted_bytes{0};
struct Admission {
    std::size_t bytes = 0;
    ~Admission() { admitted_bytes.fetch_sub(bytes); }
    bool reserve(std::size_t n) {
        auto current = admitted_bytes.load();
        do {
            if (n > 256ull * 1024 * 1024 - current)
                return false;
        } while (!admitted_bytes.compare_exchange_weak(current, current + n));
        bytes = n;
        return true;
    }
};
net::awaitable<void> handle_admin(beast::tcp_stream& stream, Service& service,
    ConnectionState& state, const http::request<http::string_body>& request, std::string path) {
    state.category = "management";
    const auto cfg = service.config();
    if (cfg.admin_key.empty()) throw Error(403, "admin_disabled", "Set admin_key to enable management API");
    admin_origin(cfg, request, state);
    if (request.method() == http::verb::options) {
        state.response_status = 204;
        http::response<http::empty_body> response(http::status::no_content, 11);
        if (!state.origin.empty()) response.set(http::field::access_control_allow_origin, state.origin);
        response.set(http::field::vary, "Origin");
        response.set(http::field::access_control_allow_methods, "GET, POST, PATCH, PUT, DELETE, OPTIONS");
        response.set(http::field::access_control_allow_headers, "Authorization, Content-Type, Last-Event-ID");
        response.set(http::field::cache_control, "no-store");
        response.keep_alive(false);
        co_await http::async_write(stream, response, net::use_awaitable);
        co_return;
    }
    const auto token = admin_token(request);
    if (!cfg.authenticate_admin(token)) throw Error(401, "admin_authentication", "A valid management Bearer key is required");
    auto method = request.method();
    const bool get = method == http::verb::get, post = method == http::verb::post;
    if (cfg.admin_key_is_default()) {
        // The default password is public knowledge: accept it only from this
        // machine or a private network, and only to replace it.
        boost::system::error_code peer_error;
        const auto peer = stream.socket().remote_endpoint(peer_error);
        if (peer_error || !local_network_address(peer.address().to_string()))
            throw Error(403, "admin_default_password_remote",
                        "The default management password only works from this machine or a local network");
        if (!(get && path == "/auth/check") && !(post && path == "/auth/password"))
            throw Error(403, "admin_password_change_required",
                        "Change the default management password before using the management API");
    }
    auto query = query_parameters(std::string_view(request.target()));
    auto parameter = [&](const char* key, std::string fallback = {}) {
        auto it = query.find(key); return it == query.end() ? fallback : it->second;
    };
    auto limit_value = parameter("limit", "100");
    std::size_t limit = 0;
    auto parsed = std::from_chars(limit_value.data(), limit_value.data()+limit_value.size(), limit);
    if (parsed.ec != std::errc{} || parsed.ptr != limit_value.data()+limit_value.size() || !limit || limit > 500)
        throw Error(400, "invalid_limit", "limit must be 1..500");
    json::object result, command;
    unsigned status = 200;
    if (get && (path == "/logs" || path == "/logs/stream")) {
        state.audit = false; // Reading logs must never generate an endless stream of read logs.
        auto cursor = parameter("cursor", "0");
        if (!query.contains("cursor") && request.find("Last-Event-ID") != request.end())
            cursor = std::string(request["Last-Event-ID"]);
        auto source = parameter("source");
        auto read = [&] { return source.empty() ? service.journal().read(cursor, limit)
            : read_log_source(cfg.data_dir, source, cursor); };
        auto batch = read(); // Validate cursor/source before committing SSE headers.
        if (path == "/logs") {
            co_await send_json(stream, 200, batch, state.origin);
            co_return;
        }
        http::response<http::empty_body> response(http::status::ok, 11);
        response.set(http::field::content_type, "text/event-stream; charset=utf-8");
        response.set(http::field::cache_control, "no-store");
        response.set("X-Accel-Buffering", "no");
        if (!state.origin.empty()) response.set(http::field::access_control_allow_origin, state.origin);
        response.set(http::field::vary, "Origin");
        response.chunked(true); response.keep_alive(false);
        http::response_serializer<http::empty_body> serializer(response);
        stream.expires_after(15s);
        co_await http::async_write_header(stream, serializer, net::use_awaitable);
        state.committed = true;
        auto executor = co_await net::this_coro::executor;
        net::steady_timer timer(executor);
        auto heartbeat = Clock::now();
        while (!service.stopping()) {
            if (!service.config().authenticate_admin(token)) break;
            const auto next = string_or(batch, "next_cursor");
            const bool data = next != cursor;
            if (data || Clock::now()-heartbeat >= 5s) {
                auto message = data ? "id: " + next + "\nevent: logs\ndata: " + json::serialize(batch) + "\n\n"
                                    : ": heartbeat\n\n";
                stream.expires_after(15s);
                co_await net::async_write(stream, http::make_chunk(net::buffer(message)), net::use_awaitable);
                heartbeat = Clock::now();
            }
            cursor = next;
            if (!bool_or(batch, "has_more")) {
                timer.expires_after(200ms);
                co_await timer.async_wait(net::use_awaitable);
            }
            batch = read();
        }
        co_await net::async_write(stream, http::make_chunk_last(), net::use_awaitable);
        co_return;
    }
    if (get && path == "/auth/check") result = {{"authenticated", true}, {"api_version", "v1"},
        {"password_change_required", cfg.admin_key_is_default()}};
    else if (post && path == "/auth/password") {
        const auto body = admin_body(request);
        if (body.size() != 1 || !body.contains("password") || !body.at("password").is_string())
            throw Error(400, "invalid_password", "Expected only a string password");
        command = {{"command", "admin.password"}, {"password", body.at("password")}};
    }
    else if (get && path == "/version") result = {{"version", "0.1.0"}, {"management_api", "v1"}};
    else if (get && path == "/capabilities") result = {
        {"api_version", "v1"}, {"accounts", true}, {"rotation", true}, {"stats", true},
        {"config", true}, {"keys", "config_and_managed"}, {"diagnostics", true},
        {"logs", json::object{{"history", true}, {"sse", true}, {"resume", true},
            {"retention", "until_explicit_local_deletion"}, {"poll_ms", 200}}}};
    else if (get && path == "/status") command = {{"command", "status"}};
    else if (get && path == "/stats") command = {{"command", "stats"}};
    else if (get && (path == "/usage/requests" || path == "/usage/summary")) {
        if (path == "/usage/summary" && (query.contains("before") || query.contains("snapshot") || query.contains("limit")))
            throw Error(400, "invalid_request_query", "Summary queries do not accept pagination");
        for (auto &[key, value] : query)
            if (key != "from" && key != "to" && key != "before" && key != "snapshot" && key != "limit" &&
                key != "model" && key != "account" && key != "status" && key != "id" && !(path == "/usage/summary" && key == "scope"))
                throw Error(400, "invalid_request_query", "Unknown request history filter");
        auto number = [&](const char *key) -> std::int64_t {
            auto text = parameter(key, "0");
            std::int64_t value = 0;
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0)
                throw Error(400, "invalid_request_query", "Request history timestamps and cursors must be nonnegative integers");
            return value;
        };
        RequestQuery filter;
        auto scope = parameter("scope");
        if (!scope.empty() && scope != "lifetime" && scope != "filtered")
            throw Error(400, "invalid_request_query", "scope must be lifetime or filtered");
        filter.cumulative = scope == "lifetime";
        filter.from = number("from"); filter.to = number("to"); filter.before = number("before"); filter.snapshot = number("snapshot");
        filter.limit = static_cast<unsigned>(limit); filter.model = parameter("model"); filter.account = parameter("account");
        filter.status = parameter("status"); filter.id = parameter("id");
        result = path == "/usage/requests" ? service.store().requests(filter)
            : service.store().stats(query.empty() ? std::nullopt : std::optional<RequestQuery>(filter));
    }
    else if (get && path.starts_with("/usage/requests/")) result = service.store().request(decode_url(path.substr(16)));
    else if (get && path == "/pricing") result = service.prices().status();
    else if (post && path == "/pricing/refresh") {
        if (!admin_body(request).empty()) throw Error(400, "invalid_price_refresh", "Price refresh takes an empty object");
        service.prices().refresh(); result = service.prices().status(); status = 202;
    }
    else if (method == http::verb::delete_ && path == "/stats") command = {{"command", "stats.reset"}};
    else if (get && path == "/accounts") command = {{"command", "accounts.list"}};
    else if (get && path == "/accounts/active") {
        result = service.control({{"command", "rotation.status"}});
        result["active_generation_account_id"] = service.control({{"command", "status"}}).at("active_generation_account_id");
    }
    else if (get && path == "/rotation") command = {{"command", "rotation.status"}};
    else if (post && path == "/rotation/next") command = {{"command", "rotation.next"}};
    else if (post && path == "/rotation/clear-cooldown") {
        command = admin_body(request); command["command"] = "rotation.clear";
    }
    else if (get && path == "/config") command = {{"command", "config.show"}};
    else if (method == http::verb::patch && path == "/config") command = {{"command", "config.patch"}, {"patch", admin_body(request)}};
    else if (post && path == "/config/validate") {
        command = {{"command", "config.validate_patch"}, {"patch", admin_body(request)}};
    }
    else if (post && path == "/config/reload") command = {{"command", "config.reload"}};
    else if (get && path == "/keys") {
        result = service.control({{"command", "keys.list"}});
        auto& keys = result.at("keys").as_array();
        for (auto& key : service.store().managed_keys()) {
            key.as_object()["source"] = "managed"; keys.push_back(std::move(key));
        }
        // Adding object fields can relocate the keys value and invalidate this reference.
        const bool configured = !keys.empty();
        result["source"] = "config_and_managed";
        result["config_key_configured"] = !cfg.api_key.empty();
        result["configured"] = configured;
    }
    else if (post && path == "/keys") {
        const auto body = admin_body(request);
        if (body.size() != 1 || !body.contains("name") || !body.at("name").is_string())
            throw Error(400, "invalid_key", "Expected a string name");
        result = service.store().create_managed_key(string_or(body, "name")); status = 201;
    }
    else if ((method == http::verb::put || method == http::verb::delete_) && path == "/keys/config") {
        auto body = method == http::verb::delete_ ? json::object{{"api_key", ""}} : admin_body(request);
        if (body.size() != 1 || !body.contains("api_key")) throw Error(400, "invalid_key", "Expected only api_key");
        command = {{"command", "config.patch"}, {"patch", body}};
    }
    else if (path.starts_with("/keys/") && (method == http::verb::patch || method == http::verb::delete_)) {
        auto id = decode_url(path.substr(6));
        if (method == http::verb::patch) {
            const auto body = admin_body(request);
            if (body.size()!=1 || !body.contains("name") || !body.at("name").is_string())
                throw Error(400, "invalid_key", "Expected a string name");
            service.store().rename_managed_key(id, string_or(body, "name"));
        } else service.store().revoke_managed_key(id);
        result = {{"ok", true}};
    }
    else if (get && path == "/models") result = service.model_catalog();
    else if (post && path == "/models/refresh") command = {{"command", "models.list"}};
    else if (post && path == "/accounts/probe") { command = {{"command", "accounts.probe"}}; }
    else if (post && path == "/accounts/import") {
        command = admin_body(request); command["command"] = "accounts.import";
    }
    else if (get && path.starts_with("/operations/")) command = {{"command", "operation.status"}, {"id", path.substr(12)}};
    else if (method == http::verb::delete_ && path.starts_with("/operations/")) {
        (void)service.control({{"command", "operation.status"}, {"id", path.substr(12)}});
        service.forget_operation(path.substr(12)); result = {{"ok", true}};
    }
    else if (path.starts_with("/accounts/")) {
        auto tail = path.substr(10); auto slash = tail.find('/');
        auto id = decode_url(tail.substr(0, slash));
        auto action = slash == std::string::npos ? "" : tail.substr(slash+1);
        command = {{"id", id}};
        if (method == http::verb::delete_ && action.empty()) command["command"] = "accounts.remove";
        else if (method == http::verb::patch && action.empty()) {
            command = admin_body(request); command["id"] = id; command["command"] = "accounts.update";
        }
        else if (post && action == "activate") command["command"] = "accounts.select";
        else if (get && action == "fingerprint") command["command"] = "accounts.fingerprint";
        else if (method == http::verb::patch && action == "fingerprint") {
            command = admin_body(request); command["id"] = id; command["command"] = "accounts.fingerprint.update";
        }
        else if (post && action == "fingerprint/regenerate") command["command"] = "accounts.fingerprint.regenerate";
        else if (post && action == "reset") command["command"] = "accounts.reset";
        else if (post && (action == "probe" || action == "detect-email")) command["command"] = "accounts.probe";
        else throw Error(404, "route_not_found", "Unknown management route or method");
    }
    else if (get && path == "/logs/sources") { state.audit = false; result = log_sources(cfg.data_dir); }
    else if ((get && path == "/diagnostics") || (post && path == "/diagnostics/export")) {
        result = {{"version", "0.1.0"}, {"status", service.control({{"command", "status"}})},
            {"browser", service.control({{"command", "browser.diagnostics"}})},
            {"config", service.control({{"command", "config.show"}})},
            {"stats", service.control({{"command", "stats"}})},
            {"logs", log_sources(cfg.data_dir)}, {"google_verified", false}};
    }
    else if (post && path == "/service/stop") {
        state.response_status = 202;
        co_await send_json(stream, 202, {{"status", "stopping"}}, state.origin);
        service.stop(); co_return;
    }
    else throw Error(404, "route_not_found", "Unknown management route or method");
    if (!command.empty()) {
        result = service.control(command);
        if (result.contains("operation_id")) status = 202;
    }
    state.response_status = status;
    co_await send_json(stream, status, result, state.origin);
}
net::awaitable<void> handle(beast::tcp_stream &stream, Service &service, ConnectionState &state) {
    auto cfg = service.config();
    beast::flat_buffer input(64 * 1024);
    http::request_parser<http::string_body> parser;
    parser.header_limit(16384);
    parser.body_limit(cfg.limits.request_bytes);
    stream.expires_after(30s);
    co_await http::async_read_header(stream, input, parser, net::use_awaitable);
    auto claimed = parser.content_length();
    auto reservation = claimed ? *claimed : (parser.chunked() ? cfg.limits.request_bytes : 0);
    if (reservation > cfg.limits.request_bytes)
        throw Error(413, "request_limit", "Request exceeds configured size limit");
    Admission admission;
    if (!admission.reserve(static_cast<std::size_t>(reservation) * 4 + 65536))
        throw Error(429, "memory_budget", "Request admission memory budget is exhausted");
    if (parser.get().find(http::field::content_encoding) != parser.get().end())
        throw Error(415, "content_encoding", "Compressed request bodies are not supported");
    co_await http::async_read(stream, input, parser, net::use_awaitable);
    auto request = parser.release();
    std::string target(request.target());
    auto path = normalize_gemini_path(std::string_view(target).substr(0, target.find('?')));
    state.openai = path.starts_with("/v1/");
    if (request.method() == http::verb::get || request.method() == http::verb::head) {
        if (auto asset = webui::find(path)) {
            http::response<http::string_body> response(http::status::ok, 11);
            response.set(http::field::server, "ais2api");
            response.set(http::field::content_type, asset->content_type);
            response.set(http::field::cache_control, asset->immutable ? "public, max-age=31536000, immutable" : "no-cache");
            response.set("X-Content-Type-Options", "nosniff");
            response.set("X-Frame-Options", "DENY");
            response.set("Referrer-Policy", "no-referrer");
            response.set("Content-Security-Policy",
                "default-src 'self'; connect-src 'self' http: https:; img-src 'self' data:; "
                "style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
            response.keep_alive(false);
            if (request.method() == http::verb::get) response.body().assign(asset->body);
            response.content_length(asset->body.size());
            stream.expires_after(15s);
            co_await http::async_write(stream, response, net::use_awaitable);
            co_return;
        }
    }
    if (path.starts_with("/api/admin/v1/") && path != "/api/admin/v1/accounts/probe-import") {
        co_await handle_admin(stream, service, state, request, path.substr(13));
        co_return;
    }
    if (request.method() == http::verb::get && path == "/userscript/ais2api-cookie.user.js") {
        const auto script = Config::default_directory() / "userscript/ais2api-cookie.user.js";
        if (!fs::is_regular_file(script))
            throw Error(404, "userscript_missing", "Userscript asset is missing; reinstall the runtime package");
        http::response<http::string_body> response(http::status::ok, 11);
        response.set(http::field::content_type, "application/javascript; charset=utf-8");
        response.set(http::field::cache_control, "no-store");
        response.keep_alive(false);
        response.body() = read_file(script, 1024 * 1024);
        response.prepare_payload();
        stream.expires_after(15s);
        co_await http::async_write(stream, response, net::use_awaitable);
        co_return;
    }
    if (path == "/api/admin/v1/accounts/probe-import" && request.method() == http::verb::options) {
        co_await handle_admin(stream, service, state, request, "/accounts/probe-import"); co_return;
    }
    if (request.method() == http::verb::post && (path == "/api/accounts/probe-import" || path == "/api/admin/v1/accounts/probe-import")) {
        auto import_cfg = service.config();
        const bool admin = path.starts_with("/api/admin/");
        if (admin) { admin_origin(import_cfg, request, state); state.category = "management"; }
        if ((admin ? import_cfg.admin_key : import_cfg.cookie_import_key).empty())
            throw Error(403, admin ? "admin_disabled" : "cookie_import_disabled",
                admin ? "Set admin_key in config.json to enable management API" :
                        "Set cookie_import_key in config.json to enable userscript import");
        const auto provided = bearer_token(request[http::field::authorization]);
        const auto principal = admin ? import_cfg.authenticate_admin(provided) : import_cfg.authenticate_cookie_import(provided);
        if (!principal)
            throw Error(401, admin ? "admin_authentication" : "cookie_import_authentication",
                admin ? "A valid management key is required" : "A valid Cookie import key is required");
        if (admin && import_cfg.admin_key_is_default())
            throw Error(403, "admin_password_change_required",
                        "Change the default management password before using the management API");
        const auto origin = request[http::field::origin];
        if (!admin && !origin.empty() && origin != "https://aistudio.google.com")
            throw Error(403, "cookie_import_origin", "Cookie import requires the AI Studio userscript");
        auto content_type = std::string_view(request[http::field::content_type]);
        if (!json_content_type(content_type))
            throw Error(415, "content_type", "Content-Type must be application/json");
        if (request.body().size() > 1024 * 1024)
            throw Error(413, "cookie_import_limit", "Cookie import exceeds one MiB");
        boost::system::error_code parse_error;
        auto value = json::parse(request.body(), parse_error);
        if (parse_error || !value.is_object())
            throw Error(400, "invalid_json", "Cookie import body must be a JSON object");
        std::string().swap(request.body());
        auto job = service.enqueue_cookie_import(std::move(value.as_object()), *principal, admin);
        struct CancelImport {
            std::shared_ptr<CookieImportJob> job;
            ~CancelImport() { job->cancellation.request_stop(); }
        } release{job};
        stream.expires_never();
        stream.socket().async_wait(tcp::socket::wait_read,
            [weak = std::weak_ptr<CookieImportJob>(job)](boost::system::error_code error) {
                if (error != net::error::operation_aborted)
                    if (auto pending = weak.lock()) pending->cancellation.request_stop();
            });
        auto executor = co_await net::this_coro::executor;
        net::steady_timer polling(executor);
        while (job->result.wait_for(0ms) != std::future_status::ready) {
            check_cancel(job->cancellation.get_token());
            const auto started = job->started.load(std::memory_order_acquire);
            const auto limit = started
                ? job->started_at + std::chrono::seconds(job->config.cookie_import_timeout_seconds)
                : job->queued + std::chrono::seconds(job->config.limits.queue_seconds);
            if (Clock::now() >= limit) {
                job->cancellation.request_stop();
                throw Error(504, started ? "cookie_import_timeout" : "queue_timeout",
                            started ? "Cookie identity verification timed out" : "Cookie import queue wait exceeded limit");
            }
            polling.expires_after(50ms);
            co_await polling.async_wait(net::use_awaitable);
        }
        auto result = job->result.get();
        co_await send_json(stream, 200, result, state.origin);
        co_return;
    }
    if (request.method() == http::verb::get && path == "/health") {
        co_await send_json(stream, 200, {{"status", "alive"}});
        co_return;
    }
    if (request.method() == http::verb::get && path == "/ready") {
        auto status = service.control({{"command", "status"}});
        bool ready = bool_or(status, "ready");
        state.response_status = ready ? 200 : 503;
        co_await send_json(
            stream, ready ? 200 : 503,
            {{"ready", ready}, {"scope", "local_prerequisites"}, {"google_verified", false}});
        co_return;
    }
    bool models = request.method() == http::verb::get &&
                  (path == "/v1/models" || path.starts_with("/v1/models/") ||
                   path == "/v1beta/models" || path.starts_with("/v1beta/models/"));
    bool openai = request.method() == http::verb::post && path == "/v1/chat/completions";
    bool responses = request.method() == http::verb::post && path == "/v1/responses";
    bool gemini = request.method() == http::verb::post && path.starts_with("/v1beta/models/") &&
                  (path.ends_with(":generateContent") || path.ends_with(":streamGenerateContent"));
    if (!models && !openai && !responses && !gemini)
        throw Error(404, "route_not_found", "Route is not implemented");
    auto principal = service.authenticate_generation(token_from(request));
    if (!principal)
        throw Error(401, "authentication", "A valid API key is required");
    if (models) {
        json::array list;
        auto available = service.model_catalog().at("models").as_array();
        // Each eligible model is followed by its "<id>-search" variant, which
        // enables Google Search by default; real ids are never shadowed.
        std::set<std::string> listed;
        for (auto &value : available)
            if (value.is_object()) {
                auto id = string_or(value.as_object(), "id", string_or(value.as_object(), "name"));
                listed.insert(id.starts_with("models/") ? id.substr(7) : id);
            }
        json::array expanded;
        for (auto &value : available) {
            expanded.push_back(value);
            if (!value.is_object())
                continue;
            auto object = value.as_object();
            auto id = string_or(object, "id", string_or(object, "name"));
            if (id.starts_with("models/"))
                id.erase(0, 7);
            if (id.empty() || !lists_search_variant(id, object) || listed.contains(id + "-search"))
                continue;
            object.erase("id");
            object["name"] = "models/" + id + "-search";
            auto display = string_or(object, "displayName");
            object["displayName"] = (display.empty() ? id : display) + " (Search)";
            object["source"] = "search_variant";
            expanded.push_back(std::move(object));
        }
        for (auto &value : expanded) {
            if (!value.is_object())
                continue;
            auto object = value.as_object();
            auto id = string_or(object, "id", string_or(object, "name"));
            if (id.starts_with("models/"))
                id.erase(0, 7);
            if (id.empty())
                continue;
            if (state.openai)
                list.push_back(
                    json::object{{"id", id},
                                 {"object", "model"},
                                 {"created", 0},
                                 {"owned_by", "google"},
                                 {"ais2api", {{"source", string_or(object, "source", "cache")}}}});
            else {
                object.erase("id");
                object["name"] = "models/" + id;
                auto display_name = object.if_contains("displayName");
                if (!display_name || display_name->is_null() ||
                    (display_name->is_string() && display_name->as_string().empty()))
                    object["displayName"] = id;
                object["supportedGenerationMethods"] =
                    json::array{"generateContent", "streamGenerateContent"};
                list.push_back(std::move(object));
            }
        }
        auto prefix = state.openai ? std::string("/v1/models/") : std::string("/v1beta/models/");
        if (path.starts_with(prefix)) {
            auto id = decode_url(path.substr(prefix.size()));
            for (auto &item : list) {
                auto &object = item.as_object();
                auto name = string_or(object, state.openai ? "id" : "name");
                if (name == id || name == "models/" + id) {
                    co_await send_json(stream, 200, object);
                    co_return;
                }
            }
            throw Error(404, "model_not_found",
                        "Model not found in discovered or configured models");
        }
        co_await send_json(stream, 200,
                           state.openai
                               ? json::object{{"object", "list"}, {"data", std::move(list)}}
                               : json::object{{"models", std::move(list)}});
        co_return;
    }
    state.usage = std::make_shared<RequestUsage>(service.store(),
        responses ? "resp_" + random_hex(16) : "chatcmpl-" + random_hex(12),
        responses ? "responses" : openai ? "chat_completions" : "gemini");
    auto content_type = request[http::field::content_type];
    if (!json_content_type(content_type))
        throw Error(415, "content_type", "Content-Type must be application/json");
    auto input_bytes = request.body().size();
    boost::system::error_code ec;
    auto value = json::parse(request.body(), ec);
    if (ec || !value.is_object())
        throw Error(400, "invalid_json", "Request body must be a JSON object");
    std::string().swap(request.body());
    auto work = std::make_shared<Work>();
    work->usage = state.usage;
    work->input_bytes = input_bytes;
    work->protocol = (openai || responses) ? Protocol::openai : Protocol::gemini;
    work->responses = responses;
    work->principal = *principal;
    work->body = std::move(value.as_object());
    work->stream =
        (openai || responses) ? bool_or(work->body, "stream") : path.ends_with(":streamGenerateContent");
    work->model =
        (openai || responses) ? string_or(work->body, "model") : decode_url(path.substr(15, path.find(':') - 15));
    work->usage->configure(work->model, work->stream);
    work->pipe = std::make_shared<EventPipe>(cfg.limits.object_bytes);
    auto executor = co_await net::this_coro::executor;
    work->wakeup = std::make_shared<net::steady_timer>(executor);
    work->pipe->set_wakeup([executor, weak = std::weak_ptr<net::steady_timer>(work->wakeup)] {
        net::post(executor, [weak] {
            if (auto timer = weak.lock())
                timer->cancel();
        });
    });
    if (!service.enqueue(work))
        throw Error(429, "queue_full", "Generation queue is full");
    struct Cancel {
        std::shared_ptr<EventPipe> pipe;
        ~Cancel() {
            pipe->disable_wakeup();
            pipe->cancel();
        }
    } cancel{work->pipe};
    // Connections are deliberately not reused: readability after the complete
    // request detects FIN/RST (or unsupported pipelining) and cancels generation.
    stream.socket().async_wait(
        tcp::socket::wait_read,
        [weak = std::weak_ptr<EventPipe>(work->pipe)](boost::system::error_code error) {
            // Windows IOCP reports a reset as an error from wait_read. It is
            // still a disconnected consumer; only local cancellation of this
            // monitor should be ignored.
            if (error != net::error::operation_aborted)
                if (auto pipe = weak.lock())
                    pipe->cancel();
        });
    if (!work->stream) {
        json::object aggregate;
        std::size_t aggregate_bound = 2;
        while (auto event = co_await next_event(work, cfg.limits.queue_seconds)) {
            merge_event(aggregate, *event, cfg.limits.object_bytes, &aggregate_bound);
            work->pipe->acknowledge();
        }
        auto output = responses ? responses_response(aggregate, work->model, work->usage->id(),
                                                     work->response_options, &service.store(), work->principal)
                      : openai ? openai_response(aggregate, work->model, work->usage->id(), &service.store(),
                                               work->principal)
                             : aggregate;
        co_await send_json(stream, 200, output);
        co_return;
    }
    auto event = co_await next_event(work, cfg.limits.queue_seconds);
    if (!event)
        throw Error(502, "empty_upstream", "Upstream returned no content");
    if (responses)
        state.responses_stream = std::make_unique<ResponsesStream>(
            work->model, work->usage->id(), work->response_options, cfg.limits.object_bytes,
            &service.store(), work->principal);
    http::response<http::empty_body> response(http::status::ok, 11);
    response.set(http::field::content_type, "text/event-stream");
    response.set(http::field::cache_control, "no-cache");
    response.set("X-Accel-Buffering", "no");
    response.keep_alive(false);
    response.chunked(true);
    http::response_serializer<http::empty_body> header(response);
    stream.expires_after(15s);
    co_await http::async_write_header(stream, header, net::use_awaitable);
    state.committed = true;
    if (responses) {
        for (const auto &response_event : state.responses_stream->start())
            co_await json_sse(stream, response_event.as_object(), cfg.limits.relay_bytes, true);
        while (event) {
            for (const auto &response_event : state.responses_stream->push(*event))
                co_await json_sse(stream, response_event.as_object(), cfg.limits.relay_bytes, true);
            work->pipe->acknowledge();
            event = co_await next_event(work, cfg.limits.queue_seconds);
        }
        for (const auto &response_event : state.responses_stream->finish())
            co_await json_sse(stream, response_event.as_object(), cfg.limits.relay_bytes, true);
        co_await net::async_write(stream, http::make_chunk_last(), net::use_awaitable);
        co_return;
    }
    const auto created = unix_seconds();
    std::optional<json::value> final_usage;
    std::set<std::int64_t> roles_sent;
    while (event) {
        auto output =
            openai ? openai_delta(*event, work->model, work->usage->id(), &service.store(), work->principal)
                   : *event;
        if (openai) {
            output["created"] = created;
            if (auto usage = output.if_contains("usage"))
                final_usage = *usage;
            output.erase("usage");
            if (work->include_usage)
                output["usage"] = nullptr;
            for (auto &choice : output.at("choices").as_array()) {
                auto &item = choice.as_object();
                auto index = json::value_to<std::int64_t>(item.at("index"));
                if (!roles_sent.insert(index).second)
                    item.at("delta").as_object().erase("role");
            }
        }
        co_await json_sse(stream, output, cfg.limits.relay_bytes);
        work->pipe->acknowledge();
        event = co_await next_event(work, cfg.limits.queue_seconds);
    }
    if (openai && work->include_usage && final_usage) {
        json::object usage_chunk{{"id", work->usage->id()},           {"object", "chat.completion.chunk"},
                                 {"created", created},       {"model", work->model},
                                 {"choices", json::array{}}, {"usage", std::move(*final_usage)}};
        co_await json_sse(stream, usage_chunk, cfg.limits.relay_bytes);
    }
    if (openai)
        co_await net::async_write(stream, http::make_chunk(net::buffer("data: [DONE]\n\n", 14)),
                                  net::use_awaitable);
    co_await net::async_write(stream, http::make_chunk_last(), net::use_awaitable);
}
net::awaitable<void> connection(tcp::socket socket, Service &service,
                                std::shared_ptr<ConnectionSlot> slot) {
    (void)slot;
    beast::tcp_stream stream(std::move(socket));
    ConnectionState state;
    std::optional<Error> error;
    bool delivered = true;
    try {
        co_await handle(stream, service, state);
    } catch (const Error &e) {
        error = e;
    } catch (const boost::system::system_error &e) {
        delivered = false;
        if (e.code() == http::error::body_limit)
            error = Error(413, "request_limit", "Request exceeds configured size limit");
        else if (e.code() == beast::error::timeout)
            error = Error(504, "network_timeout", "HTTP I/O timed out");
        else if (state.usage)
            error = Error(499, "client_disconnected", "Client disconnected");
    } catch (const std::exception &) {
        error = Error(500, "internal", "Request failed internally");
    }
    if (error) {
        try {
            if (state.committed) {
                if (state.responses_stream) {
                    for (const auto &value : state.responses_stream->fail(*error))
                        co_await json_sse(stream, value.as_object(), 16384, true);
                } else
                    co_await json_sse(stream, error_json(*error, state.openai), 16384);
                co_await net::async_write(stream, http::make_chunk_last(), net::use_awaitable);
            } else if (error->status != 499)
                co_await send_json(stream, static_cast<unsigned>(error->status),
                                   error_json(*error, state.openai), state.origin);
        } catch (...) { delivered = false; }
    }
    if (state.usage) {
        try { state.usage->http_done(error && !state.committed ? error->status : state.response_status,
                                   error ? error->code : "", delivered && (!error || error->status != 499)); }
        catch (const std::exception &) { service.usage_failure(state.usage->id()); }
    }
    if (state.audit) {
        try { service.log({{"event", "http_request"}, {"category", state.category},
            {"status", error ? error->status : static_cast<int>(state.response_status)}, {"error", error ? error->code : ""}}); }
        catch (...) {}
    }
    boost::system::error_code ignored;
    stream.socket().cancel(ignored);
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream.socket().close(ignored);
}
net::awaitable<void> accept_loop(tcp::acceptor &acceptor, Service &service) {
    while (!service.stopping()) {
        boost::system::error_code ec;
        auto socket = co_await acceptor.async_accept(net::redirect_error(net::use_awaitable, ec));
        if (ec)
            co_return;
        // Streamed events are small writes; do not let Nagle hold them back
        // waiting for the client's delayed ACK.
        socket.set_option(tcp::no_delay(true), ec);
        if (live_connections.fetch_add(1) >= 32 || service.log_backlogged()) {
            live_connections.fetch_sub(1);
            socket.close(ec);
            continue;
        }
        auto slot = std::make_shared<ConnectionSlot>();
        net::co_spawn(acceptor.get_executor(),
                      connection(std::move(socket), service, std::move(slot)), net::detached);
    }
}
} // namespace
int serve(Config config) { return serve_with_backend(std::move(config), {}); }
int serve_with_backend(Config config, std::unique_ptr<GenerationBackend> backend) {
    config.validate();
    InstanceLock owner(config.data_dir);
    Service service(config, std::move(backend));
    net::io_context io(1);
    tcp::endpoint endpoint(net::ip::make_address(config.bind), config.port);
    tcp::acceptor acceptor(io);
    auto check_listener = [&](std::string_view stage, const boost::system::error_code &ec) {
        if (!ec)
            return;
        const std::string reason = ec == net::error::access_denied ? "permission_denied"
                                   : ec == net::error::address_in_use ? "address_in_use"
                                                                     : "listener_error";
        throw Error(503, "http_" + reason,
                    "HTTP listener " + std::string(stage) + " failed for bind=" + config.bind +
                        " port=" + std::to_string(config.port) + " reason=" + reason +
                        " os_error=" + std::to_string(ec.value()) +
                        "; configure a different available port and restart");
    };
    boost::system::error_code listener_error;
    acceptor.open(endpoint.protocol(), listener_error);
    check_listener("open", listener_error);
    acceptor.set_option(net::socket_base::reuse_address(true), listener_error);
    check_listener("set_option", listener_error);
    acceptor.bind(endpoint, listener_error);
    check_listener("bind", listener_error);
    acceptor.listen(32, listener_error);
    check_listener("listen", listener_error);
    ControlServer control(config,
                          [&](const json::object &command) { return service.control(command); });
    net::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](auto, int) {
        service.stop();
        boost::system::error_code ec;
        acceptor.close(ec);
        io.stop();
    });
    net::steady_timer watcher(io);
    std::function<void()> watch;
    watch = [&] {
        watcher.expires_after(100ms);
        watcher.async_wait([&](boost::system::error_code ec) {
            if (ec)
                return;
            if (service.stopping()) {
                boost::system::error_code ignored;
                acceptor.close(ignored);
                io.stop();
            } else
                watch();
        });
    };
    watch();
    service.log({{"event", "listening"}, {"bind", config.bind}, {"port", config.port}});
    net::co_spawn(io, accept_loop(acceptor, service), net::detached);
    io.run();
    service.shutdown();
    service.log({{"event", "stopped"}});
    return 0;
}
} // namespace ais2api
