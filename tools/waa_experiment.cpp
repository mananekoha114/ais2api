// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The Firefox 158 comparison experiment: compares Camoufox (cell 1) and
// SpiderMonkey (cell 4) across probe, ListModels, single-turn generation and
// second-turn Rerun, with optional hybrid cells (2 and 3) to isolate transport
// from WAA proof generation.
//
//   cell  proof          transport
//   1     camoufox       page        the Camoufox backend
//   2     camoufox       libcurl     transport alone (adopts Camoufox page UA & context)
//   3     spidermonkey   page        the program and its host alone
//   4     spidermonkey   libcurl     the SpiderMonkey backend
//
// One account and one short prompt; the selected cells take turns round by
// round so they share the same stretch of time. "same" keeps each gateway's
// page and program between rounds; "new" starts both over at the start of each
// cell's round (keeping the page across probe -> models -> generate -> rerun
// within that round so rerun exercises the second-turn Rerun path).
// One JSON line per operation goes to --out; no prompt, proof, response text,
// email, account ID or local path is written.
//
// Stops everything at the first refusal (403), 401, 429 or region or account
// refusal, and a cell after two other failures in a row.
// Creating <out>.stop stops after the current operation.
//
// Not installed or packaged: -DAIS2API_BUILD_WAA_EXPERIMENT=ON, which needs
// AIS2API_SPIDERMONKEY. Run it with the service stopped: it launches the
// account's Camoufox from the same data directory.
#include "ais2api/camoufox.hpp"
#include "ais2api/config.hpp"
#include "ais2api/protocol.hpp"
#include "ais2api/spidermonkey.hpp"
#include "ais2api/store.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

namespace ais2api {
namespace experiment_detail {
json::object captured_page_session(const CamoufoxGateway &gateway);
void stage_page_session(SpiderMonkeyGateway &gateway, json::object session);
} // namespace experiment_detail

namespace {
using Steady = std::chrono::steady_clock;

struct Cell {
    int number;
    std::string_view proof, transport;
};
constexpr std::array<Cell, 4> all_cells{{{1, "camoufox", "page"},
                                         {2, "camoufox", "libcurl"},
                                         {3, "spidermonkey", "page"},
                                         {4, "spidermonkey", "libcurl"}}};

struct Options {
    fs::path config, out;
    std::string account;
    std::string model = "gemini-3.1-flash-lite";
    std::string prompt = "Reply with the single word: ok";
    unsigned rounds = 10, interval = 20;
    std::vector<std::string> modes{"same", "new"};
    std::vector<std::string> ops{"probe", "models", "generate", "rerun"};
    std::string cells = "14";
};

[[noreturn]] void usage() {
    std::cerr << "usage: ais2api_waa_experiment --config PATH --account ID --out FILE\n"
                 "         [--rounds N] [--interval SECONDS] [--modes same,new] [--cells 14]\n"
                 "         [--ops probe,models,generate,rerun] [--model ID] [--prompt TEXT]\n";
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

std::vector<std::string> split_csv(std::string_view value, std::initializer_list<std::string_view> allowed) {
    if (value.empty()) usage();
    std::vector<std::string> items;
    for (std::size_t start = 0; start <= value.size();) {
        const auto end = std::min(value.find(',', start), value.size());
        const auto item = value.substr(start, end - start);
        if (std::find(allowed.begin(), allowed.end(), item) == allowed.end()) usage();
        items.emplace_back(item);
        if (end == value.size()) break;
        start = end + 1;
    }
    if (items.empty()) usage();
    return items;
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
            options.cells.clear();
            for (const char c : value)
                if (options.cells.find(c) == std::string::npos) options.cells += c;
        } else if (name == "--modes") {
            options.modes = split_csv(value, {"same", "new"});
        } else if (name == "--ops") {
            options.ops = split_csv(value, {"probe", "models", "generate", "rerun"});
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

std::string diagnostic_user_agent(GenerationBackend &gateway) {
    const auto diagnostics = gateway.diagnostics();
    const auto *identity = diagnostics.if_contains("identity");
    if (!identity || !identity->is_object()) return {};
    return string_or(identity->as_object(), "userAgent");
}

int run(const Options &options) {
    const auto config = Config::load(options.config);
    Store store(config.data_dir);
    store.account(options.account); // 404 now rather than in the first cell
    CamoufoxGateway camoufox(config, store);
    SpiderMonkeyGateway spidermonkey(config, store);
    // Dedicated transport session for hybrid cell 2 so adopting Camoufox's live
    // page context never disturbs cell 4's resident SpiderMonkey page and VM.
    SpiderMonkeyGateway hybrid_transport(config, store);
    auto body = json::parse(R"({"contents":[{"role":"user","parts":[{"text":""}]}]})").as_object();
    body["contents"].as_array()[0].as_object()["parts"].as_array()[0].as_object()["text"] = options.prompt;
    const auto request = normalize_gemini(options.model, body, false, config);

    std::vector<Cell> cells;
    for (const char c : options.cells)
        for (const auto &cell : all_cells)
            if (cell.number == c - '0') cells.push_back(cell);
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
        if (mode == "new") {
            camoufox.reset_session();
            spidermonkey.reset_session();
            hybrid_transport.reset_session();
        }
        for (unsigned round = 1; round <= options.rounds && stopped_all.empty(); ++round) {
            for (const auto &cell : cells) {
                if (!stopped_all.empty()) break;
                if (stopped.contains(cell.number)) continue;
                if (mode == "new") {
                    if (cell.proof == "camoufox" || cell.transport == "page") camoufox.reset_session();
                    if (cell.number == 2) hybrid_transport.reset_session();
                    if (cell.proof == "spidermonkey" || cell.number == 4) spidermonkey.reset_session();
                }
                for (const auto &op : options.ops) {
                    if (!stopped_all.empty() || stopped.contains(cell.number)) break;
                    // Probe and ListModels belong to the self-contained backends;
                    // hybrid cells 2 and 3 only split proof and generation send.
                    if ((op == "probe" || op == "models") && (cell.number == 2 || cell.number == 3)) continue;
                    std::error_code missing;
                    if (fs::exists(stop_file, missing)) {
                        stopped_all = "stop_file";
                        break;
                    }
                    if (!first && options.interval) std::this_thread::sleep_for(std::chrono::seconds(options.interval));
                    first = false;

                    const auto account = store.account(options.account);
                    const auto vms_before = vms_started(spidermonkey);
                    json::object record{{"time", now_utc()},
                                        {"mode", mode},
                                        {"round", round},
                                        {"cell", cell.number},
                                        {"op", op},
                                        {"proof", cell.proof},
                                        {"transport", cell.transport}};
                    const auto started = Steady::now();
                    long long proof_ms = -1, first_chunk_ms = -1, send_ms = -1;
                    unsigned chunks = 0;
                    std::size_t text = 0, proof_bytes = 0;
                    bool passed = false;
                    try {
                        if (op == "probe") {
                            const auto result = cell.number == 1 ? camoufox.probe(account) : spidermonkey.probe(account);
                            const bool ready = bool_or(result, "identityReady") && !string_or(result, "email").empty();
                            record["identity_ready"] = ready;
                            passed = ready;
                            if (!passed) record["code"] = "identity_unverified";
                        } else if (op == "models") {
                            const auto discovered = cell.number == 1 ? camoufox.models(account) : spidermonkey.models(account);
                            record["models_count"] = static_cast<std::int64_t>(discovered.size());
                            passed = !discovered.empty();
                            if (!passed) record["code"] = "no_models";
                        } else {
                            const auto proof = cell.proof == "camoufox" ? camoufox.experiment_ticket(request, account)
                                                                        : spidermonkey.experiment_proof(request, account);
                            proof_ms = since(started);
                            proof_bytes = proof.size();
                            if (cell.number == 2)
                                experiment_detail::stage_page_session(hybrid_transport,
                                                                      experiment_detail::captured_page_session(camoufox));
                            const auto sent = Steady::now();
                            const EventSink sink = [&](GenerationEvent event) {
                                if (event.kind != GenerationEvent::Kind::chunk) return;
                                if (!chunks) first_chunk_ms = since(sent);
                                ++chunks;
                                text += text_length(event.data);
                            };
                            if (cell.transport == "page") camoufox.experiment_send(request, account, proof, sink);
                            else if (cell.number == 2) hybrid_transport.experiment_send(request, account, proof, sink);
                            else spidermonkey.experiment_send(request, account, proof, sink);
                            send_ms = since(sent);
                            if (cell.number == 2) {
                                const auto page_ua = diagnostic_user_agent(camoufox);
                                const auto transport_ua = diagnostic_user_agent(hybrid_transport);
                                record["ua_matched"] = !page_ua.empty() && page_ua == transport_ua;
                            }
                            passed = chunks > 0;
                            if (!passed) record["code"] = "no_chunks";
                        }
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
                    if (op == "generate" || op == "rerun") {
                        record["proof_ms"] = proof_ms;
                        record["proof_bytes"] = static_cast<std::int64_t>(proof_bytes);
                        record["first_chunk_ms"] = first_chunk_ms;
                        record["send_ms"] = send_ms;
                        record["chunks"] = chunks;
                        record["text_chars"] = static_cast<std::int64_t>(text);
                    }
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
                    std::cout << mode << " round " << round << " cell " << cell.number << " " << op << ": "
                              << (passed ? "ok" : (code.empty() ? std::string("failed") : code)) << " ("
                              << record["total_ms"].as_int64() << " ms" << (record["vm_started"].as_bool() ? ", vm started" : "") << ")"
                              << std::endl;
                }
            }
        }
    }
    const auto sm_diagnostics = spidermonkey.diagnostics();
    camoufox.close();
    hybrid_transport.close();
    spidermonkey.close();

    json::array ops_json;
    for (const auto &op : options.ops) ops_json.emplace_back(op);
    json::object summary{{"summary", true},
                         {"time", now_utc()},
                         {"model", options.model},
                         {"rounds", options.rounds},
                         {"interval", options.interval},
                         {"selected_cells", options.cells},
                         {"ops", std::move(ops_json)}};
    for (const auto *key : {"claimedFirefox", "network", "networkStack", "host"})
        if (const auto *value = sm_diagnostics.if_contains(key)) summary[key] = *value;
    if (!stopped_all.empty()) summary["all_stopped"] = stopped_all;
    json::object by_mode;
    for (const auto &[mode, by_cell] : tallies) {
        json::object cells_json;
        for (const auto &[number, tally] : by_cell) {
            json::object entry{{"attempts", tally.attempts},
                               {"passed", tally.passed},
                               {"refused", tally.refused},
                               {"failed", tally.failed}};
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
