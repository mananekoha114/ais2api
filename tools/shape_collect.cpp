// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Collects a shape table (tools/shape/collect.js) with an account's Camoufox
// identity, for tools/shape/merge.py.
//
//   ais2api_shape_collect --config PATH --account ID --script tools/shape/collect.js --out FILE
//                         [--local URL] [--settle SECONDS]
//
// Without --local it loads AI Studio's new chat page once, with the
// account's cookies and nothing preloaded, and generates nothing. With
// --local it opens that address (tools/shape/serve.py's collect.html?manual)
// with the identity alone and no cookies. Either way it waits for the page's
// first frame to load and for --settle seconds (default 10), then evaluates
// the script and writes the table it returns to --out.
//
// Not installed or packaged: -DAIS2API_BUILD_SHAPE_COLLECTOR=ON. Run it with
// the service stopped: it launches the account's Camoufox from the same data
// directory.
#include "ais2api/camoufox.hpp"
#include "ais2api/config.hpp"
#include "ais2api/store.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

namespace ais2api {
namespace {
struct Options {
    fs::path config, script, out;
    std::string account, local;
    unsigned settle = 10;
};

[[noreturn]] void usage() {
    std::cerr << "usage: ais2api_shape_collect --config PATH --account ID --script collect.js --out FILE\n"
                 "         [--local URL] [--settle SECONDS]\n";
    std::exit(2);
}

Options parse(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (i + 1 >= argc) usage();
        const std::string value = argv[++i];
        if (name == "--config") options.config = value;
        else if (name == "--account") options.account = value;
        else if (name == "--script") options.script = value;
        else if (name == "--out") options.out = value;
        else if (name == "--local") options.local = value;
        else if (name == "--settle") {
            if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != std::string::npos) usage();
            options.settle = static_cast<unsigned>(std::stoul(value));
        } else usage();
    }
    if (options.config.empty() || options.account.empty() || options.script.empty() || options.out.empty()) usage();
    if (!options.local.empty() && !options.local.starts_with("http://127.0.0.1:")) {
        std::cerr << "--local takes a loopback address (http://127.0.0.1:PORT/...)\n";
        std::exit(2);
    }
    return options;
}

int run(const Options &options) {
    std::ifstream file(options.script, std::ios::binary);
    if (!file) throw Error(500, "io_error", "Cannot read the collection script");
    const std::string script{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto config = Config::load(options.config);
    Store store(config.data_dir);
    const auto account = store.account(options.account);
    CamoufoxGateway camoufox(config, store);
    const bool live = options.local.empty();
    const auto url = live ? "https://aistudio.google.com/prompts/new_chat?authuser=" + account.auth_user : options.local;
    const auto table = camoufox.experiment_collect(account, url, script, live, std::chrono::seconds(options.settle));
    std::ofstream out(options.out, std::ios::binary | std::ios::trunc);
    if (!out || !(out << table)) throw Error(500, "io_error", "Cannot write the output file");
    std::cout << (live ? "live page" : "local page") << ": " << table.size() << " bytes" << std::endl;
    return 0;
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
