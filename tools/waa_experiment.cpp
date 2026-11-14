// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The P3 four-cell experiment: whether AI Studio's refusals follow the WAA
// proof's source (the Camoufox page or the embedded SpiderMonkey) or the
// request's transport (the page's fetch or libcurl).
//
//   cell  proof          transport
//   1     camoufox       page        today's backend
//   2     camoufox       libcurl     the transport alone
//   3     spidermonkey   page        the program and its host alone
//   4     spidermonkey   libcurl     the SpiderMonkey backend
//
// One account and one short prompt; the cells take turns, round by round, so
// that they share the same stretch of time. "same" keeps each gateway's page
// and program between requests, "new" starts both over before each request.
// One JSON line per attempt goes to --out; no prompt, proof or response text.
//
// Stops everything at the first refusal (403), 401, 429 or region or account
// refusal, and a cell after two other failures in a row.
// Creating <out>.stop stops after the current attempt.
//
// Not installed or packaged: -DAIS2API_BUILD_WAA_EXPERIMENT=ON, which needs
// AIS2API_SPIDERMONKEY. Run it with the service stopped: it launches the
// account's Camoufox from the same data directory.
#include "ais2api/camoufox.hpp"
#include "ais2api/config.hpp"
#include "ais2api/protocol.hpp"
#include "ais2api/spidermonkey.hpp"
#include "ais2api/store.hpp"
#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

namespace ais2api {
namespace {
using Steady = std::chrono::steady_clock;

struct Cell {
    int number;
    std::string_view proof, transport;
};
constexpr std::array<Cell, 4> all_cells{{{1, "camoufox", "page"}, {2, "camoufox", "libcurl"}, {3, "spidermonkey", "page"}, {4, "spidermonkey", "libcurl"}}};

struct Options {
    fs::path config, out;
    std::string account;
    std::string model = "gemini-3.1-flash-lite";
    std::string prompt = "Reply with the single word: ok";
    unsigned rounds = 10, interval = 20;
    std::vector<std::string> modes{"same", "new"};
    std::string cells = "1234";
};

[[noreturn]] void usage() {
    std::cerr << "usage: ais2api_waa_experiment --config PATH --account ID --out FILE\n"
                 "         [--rounds N] [--interval SECONDS] [--modes same,new] [--cells 1234]\n"
                 "         [--model ID] [--prompt TEXT]\n";
    std::exit(2);
}

unsigned number(std::string_view text) {
    unsigned value = 0;
    if (text.empty() || text.size() > 6) usage();
    for (const char c : text) {
        if (c < '0' || c > '9') usage();
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value;
}

Options parse(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (i + 1 >= argc) usage();
        const std::string value = argv[++i];
        if (name == "--config") options.config = value;
        else if (name == "--account") options.account = value;
        else if (name == "--out") options.out = value;
        else if (name == "--rounds") options.rounds = number(value);
        else if (name == "--interval") options.interval = number(value);
        else if (name == "--model") options.model = value;
        else if (name == "--prompt") options.prompt = value;
        else if (name == "--cells") {
            if (value.empty() || value.find_first_not_of("1234") != std::string::npos) usage();
            options.cells = value;
        } else if (name == "--modes") {
            options.modes.clear();
            for (std::size_t start = 0; start <= value.size();) {
                const auto end = std::min(value.find(',', start), value.size());
                const auto mode = value.substr(start, end - start);
                if (mode != "same" && mode != "new") usage();
                options.modes.push_back(mode);
                start = end + 1;
            }
        } else usage();
    }
    if (options.config.empty() || options.account.empty() || options.out.empty() || !options.rounds) usage();
    return options;
}

long long since(Steady::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now() - start).count();
}

std::string now_utc() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

// The length of the text a chunk event carries, wherever its "text" fields are.
std::size_t text_length(const json::value &value) {
    std::size_t total = 0;
    if (value.is_object()) {
        for (const auto &[key, item] : value.as_object())
            total += key == "text" && item.is_string() ? item.as_string().size() : text_length(item);
    } else if (value.is_array()) {
        for (const auto &item : value.as_array()) total += text_length(item);
    }
    return total;
}

std::int64_t vms_started(SpiderMonkeyGateway &gateway) {
    const auto diagnostics = gateway.diagnostics();
    const auto *value = diagnostics.if_contains("vmsStarted");
    if (!value) return -1;
    if (value->is_int64()) return value->as_int64();
    if (value->is_uint64()) return static_cast<std::int64_t>(value->as_uint64());
    return -1;
}

int run(const Options &options) {
    const auto config = Config::load(options.config);
    Store store(config.data_dir);
    store.account(options.account); // 404 now rather than in the first cell
    CamoufoxGateway camoufox(config, store);
    SpiderMonkeyGateway spidermonkey(config, store);
    auto body = json::parse(R"({"contents":[{"role":"user","parts":[{"text":""}]}]})").as_object();
    body["contents"].as_array()[0].as_object()["parts"].as_array()[0].as_object()["text"] = options.prompt;
    const auto request = normalize_gemini(options.model, body, false, config);

    std::vector<Cell> cells;
    for (const auto &cell : all_cells)
        if (options.cells.find(static_cast<char>('0' + cell.number)) != std::string::npos) cells.push_back(cell);
    std::ofstream out(options.out, std::ios::app | std::ios::binary);
    if (!out) throw Error(500, "io_error", "Cannot open the output file");
    auto stop_file = options.out;
    stop_file += ".stop";

    struct Tally {
        unsigned attempts = 0, passed = 0, refused = 0, failed = 0;
    };
    std::map<std::string, std::map<int, Tally>> tallies;
    std::map<int, unsigned> failures_in_row;
    std::map<int, std::string> stopped;
    std::string stopped_all;
    bool first = true;

    for (const auto &mode : options.modes) {
        for (unsigned round = 1; round <= options.rounds && stopped_all.empty(); ++round) {
            for (const auto &cell : cells) {
                if (!stopped_all.empty()) break;
                if (stopped.contains(cell.number)) continue;
                std::error_code missing;
                if (fs::exists(stop_file, missing)) {
                    stopped_all = "stop_file";
                    break;
                }
                if (!first) std::this_thread::sleep_for(std::chrono::seconds(options.interval));
                first = false;
                if (mode == "new") {
                    camoufox.reset_session();
                    spidermonkey.reset_session();
                }
                const auto account = store.account(options.account);
                const auto vms_before = vms_started(spidermonkey);
                json::object record{{"time", now_utc()}, {"mode", mode}, {"round", round}, {"cell", cell.number},
                                    {"proof", cell.proof}, {"transport", cell.transport}};
                const auto started = Steady::now();
                long long proof_ms = -1, first_chunk_ms = -1, send_ms = -1;
                unsigned chunks = 0;
                std::size_t text = 0, proof_bytes = 0;
                bool passed = false;
                try {
                    const auto proof = cell.proof == "camoufox" ? camoufox.experiment_ticket(request, account)
                                                                : spidermonkey.experiment_proof(request, account);
                    proof_ms = since(started);
                    proof_bytes = proof.size();
                    const auto sent = Steady::now();
                    const EventSink sink = [&](GenerationEvent event) {
                        if (event.kind != GenerationEvent::Kind::chunk) return;
                        if (!chunks) first_chunk_ms = since(sent);
                        ++chunks;
                        text += text_length(event.data);
                    };
                    if (cell.transport == "page") camoufox.experiment_send(request, account, proof, sink);
                    else spidermonkey.experiment_send(request, account, proof, sink);
                    send_ms = since(sent);
                    passed = chunks > 0;
                    if (!passed) record["code"] = "no_chunks";
                } catch (const Error &error) {
                    record["status"] = error.status;
                    record["code"] = error.code;
                    if (error.retry_after) record["retry_after"] = error.retry_after;
                    record["message"] = error.what();
                } catch (const std::exception &error) {
                    record["code"] = "exception";
                    record["message"] = error.what();
                }
                const auto vms_after = vms_started(spidermonkey);
                record["ok"] = passed;
                record["total_ms"] = since(started);
                record["proof_ms"] = proof_ms;
                record["proof_bytes"] = proof_bytes;
                record["first_chunk_ms"] = first_chunk_ms;
                record["send_ms"] = send_ms;
                record["chunks"] = chunks;
                record["text_chars"] = text;
                record["vm_started"] = vms_before >= 0 && vms_after > vms_before;

                auto &tally = tallies[mode][cell.number];
                ++tally.attempts;
                const auto status = record.contains("status") ? record["status"].as_int64() : 0;
                const auto code = record.contains("code") ? std::string(record["code"].as_string()) : std::string();
                if (passed) {
                    ++tally.passed;
                    failures_in_row[cell.number] = 0;
                } else {
                    ++failures_in_row[cell.number];
                    if (status == 403) ++tally.refused;
                    else ++tally.failed;
                    // Any refusal ends the run: the account matters more than the sample.
                    if (status == 401 || status == 403 || status == 429 || code == "region_blocked" || code == "account_unsupported")
                        stopped_all = code.empty() ? "status_" + std::to_string(status) : code;
                    else if (failures_in_row[cell.number] >= 2) stopped[cell.number] = "two_failures";
                }
                if (stopped.contains(cell.number)) record["cell_stopped"] = stopped[cell.number];
                if (!stopped_all.empty()) record["all_stopped"] = stopped_all;
                out << json::serialize(record) << '\n';
                out.flush();
                std::cout << mode << " round " << round << " cell " << cell.number << ": "
                          << (passed ? "ok" : (code.empty() ? std::string("failed") : code)) << " (" << record["total_ms"].as_int64() << " ms"
                          << (record["vm_started"].as_bool() ? ", vm started" : "") << ")" << std::endl;
            }
        }
    }
    camoufox.close();
    spidermonkey.close();

    json::object summary{{"summary", true}, {"time", now_utc()}, {"model", options.model}, {"rounds", options.rounds},
                         {"interval", options.interval}};
    if (!stopped_all.empty()) summary["all_stopped"] = stopped_all;
    json::object by_mode;
    for (const auto &[mode, by_cell] : tallies) {
        json::object cells_json;
        for (const auto &[number, tally] : by_cell) {
            json::object entry{{"attempts", tally.attempts}, {"passed", tally.passed}, {"refused", tally.refused}, {"failed", tally.failed}};
            if (stopped.contains(number)) entry["stopped"] = stopped[number];
            cells_json[std::to_string(number)] = std::move(entry);
            std::cout << mode << " cell " << number << ": " << tally.passed << "/" << tally.attempts << " passed, " << tally.refused
                      << " refused, " << tally.failed << " failed" << std::endl;
        }
        by_mode[mode] = std::move(cells_json);
    }
    summary["cells"] = std::move(by_mode);
    out << json::serialize(summary) << '\n';
    return stopped_all.empty() ? 0 : 3;
}
} // namespace
} // namespace ais2api

int main(int argc, char **argv) {
    try {
        return ais2api::run(ais2api::parse(argc, argv));
    } catch (const ais2api::Error &error) {
        std::cerr << error.code << ": " << error.what() << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
