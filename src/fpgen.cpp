// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Ported from fpgen's bayesian_network.py and unpacker.py (Apache License 2.0,
// Copyright the fingerprint-generator authors; see licenses/fpgen-LICENSE.txt).

#include "ais2api/fpgen.hpp"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <unordered_map>
namespace ais2api::fpgen {
namespace {
[[noreturn]] void invalid(const std::string &detail) {
    throw Error(503, "browser_not_configured", "fpgen model is invalid (" + detail + "): rerun ais2api setup");
}
std::uint64_t splitmix(std::uint64_t &state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
// Search beams keep this many assignments, as fpgen's BEAM_WIDTH.
constexpr std::size_t beam_width = 1000;
// The model files are small; this only stops a corrupt frame header from
// asking for an absurd buffer.
constexpr std::size_t json_limit = 64 * 1024 * 1024;

std::string decompress(std::string_view compressed, std::string_view name) {
    const auto size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > json_limit)
        invalid(std::string(name) + " has no usable zstd frame size");
    std::string output(static_cast<std::size_t>(size), '\0');
    const auto written = ZSTD_decompress(output.data(), output.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(written) || written != output.size()) invalid(std::string(name) + " does not decompress");
    return output;
}
std::string checked(const fs::path &directory, const std::map<std::string, std::string> &file_sha256,
                    const std::string &name) {
    const auto pin = file_sha256.find(name);
    if (pin == file_sha256.end()) invalid("no sha256 is pinned for " + name);
    const auto path = directory / name;
    std::error_code error;
    if (!fs::is_regular_file(path, error))
        throw Error(503, "browser_not_configured", "fpgen model is not installed: run ais2api setup");
    auto data = read_file(path, json_limit);
    if (sha256(data) != pin->second) invalid(name + " does not match its pinned sha256");
    return data;
}
} // namespace

Random::Random(std::uint64_t seed) {
    for (auto &word : state_) word = splitmix(seed);
}
std::uint64_t Random::next() {
    const auto result = rotl(state_[1] * 5, 7) * 9;
    const auto t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
}
double Random::uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
std::uint64_t Random::below(std::uint64_t bound) {
    if (!bound) throw std::invalid_argument("Random::below needs a positive bound");
    // Reject the tail that would make low values likelier.
    const auto limit = UINT64_MAX - UINT64_MAX % bound;
    for (;;)
        if (const auto value = next(); value < limit) return value % bound;
}

ValueId decode_index(std::string_view text) {
    // base64.b85decode, then the bytes as a big-endian integer.
    static constexpr std::string_view alphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";
    const auto padding = (5 - text.size() % 5) % 5;
    std::string bytes;
    for (std::size_t start = 0; start < text.size(); start += 5) {
        std::uint64_t hunk = 0;
        for (std::size_t i = start; i < start + 5; ++i) {
            const char c = i < text.size() ? text[i] : '~';
            const auto digit = alphabet.find(c);
            if (digit == std::string_view::npos) invalid("value index has a character outside base85");
            hunk = hunk * 85 + digit;
        }
        if (hunk > 0xffffffffull) invalid("value index overflows a base85 hunk");
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>((hunk >> shift) & 0xff));
    }
    bytes.resize(bytes.size() - padding);
    if (bytes.size() > sizeof(ValueId)) invalid("value index is too large");
    ValueId value = 0;
    for (const auto byte : bytes) value = (value << 8) | static_cast<unsigned char>(byte);
    return value;
}

ValueId choose(const std::vector<Weighted> &distribution, Random &rng) {
    if (distribution.empty()) throw Error(500, "fpgen_sampling", "Cannot draw from an empty distribution");
    const auto anchor = rng.uniform();
    double cumulative = 0;
    for (const auto &entry : distribution) {
        cumulative += entry.probability;
        if (anchor < cumulative) return entry.value;
    }
    return distribution.front().value;
}

namespace {
// A conditional probability table: one level per parent, in parent order,
// then the node's own values with their probabilities in file order.
struct Table {
    std::vector<ValueId> keys;  // sorted
    std::vector<Table> children;
    std::vector<Weighted> leaf;
};
struct Node {
    std::string name;
    std::vector<std::size_t> parents;
    std::vector<ValueId> values;  // file order
    std::vector<char> ancestor;   // by node index
    Table table;
};
bool allows(const std::vector<ValueId> &allowed, ValueId value) {
    return std::binary_search(allowed.begin(), allowed.end(), value);
}
void parse_table(const json::object &object, std::size_t depth, std::size_t parents, Table &table) {
    if (depth == parents) {
        table.leaf.reserve(object.size());
        for (const auto &item : object) {
            if (!item.value().is_number()) invalid("a probability is not a number");
            table.leaf.push_back({decode_index(item.key()), item.value().to_number<double>()});
        }
        return;
    }
    std::vector<std::pair<ValueId, const json::object *>> entries;
    for (const auto &item : object) {
        if (!item.value().is_object()) invalid("a conditional table level is not an object");
        entries.emplace_back(decode_index(item.key()), &item.value().as_object());
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (std::adjacent_find(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.first == b.first; }) != entries.end())
        invalid("a conditional table names one value twice");
    table.keys.reserve(entries.size());
    table.children.resize(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        table.keys.push_back(entries[i].first);
        parse_table(*entries[i].second, depth + 1, parents, table.children[i]);
    }
}
} // namespace

struct Model::Impl {
    std::vector<Node> nodes;  // sampling order
    std::unordered_map<std::string, std::size_t> by_name;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> spans;  // by ValueId: offset, length
    std::string values_dat;  // compressed

    // The probabilities fpgen's get_probabilities_given_known_values returns,
    // or nothing for a parent combination the table does not have.
    const std::vector<Weighted> *probabilities(const Node &node, const std::vector<ValueId> &parent_values) const {
        const Table *table = &node.table;
        for (const auto value : parent_values) {
            const auto at = std::lower_bound(table->keys.begin(), table->keys.end(), value);
            if (at == table->keys.end() || *at != value) return nullptr;
            table = &table->children[static_cast<std::size_t>(at - table->keys.begin())];
        }
        return &table->leaf;
    }
    std::vector<Weighted> trace(std::size_t target, const Evidence &evidence) const {
        std::vector<char> relevant = nodes[target].ancestor;
        relevant[target] = 1;
        for (const auto &[node, _] : evidence) {
            relevant[node] = 1;
            for (std::size_t i = 0; i < nodes.size(); ++i) relevant[i] |= nodes[node].ancestor[i];
        }
        std::vector<std::size_t> ordered, position(nodes.size(), SIZE_MAX);
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (relevant[i]) {
                position[i] = ordered.size();
                ordered.push_back(i);
            }
        struct Assignment {
            std::vector<ValueId> values;  // by position in `ordered`
            double probability;
        };
        struct Candidate {
            std::size_t assignment;
            ValueId value;
            double probability;
        };
        std::vector<Assignment> beam{{std::vector<ValueId>(ordered.size()), 1.0}}, next;
        std::vector<Candidate> candidates;
        std::vector<Weighted> uniform;
        std::vector<ValueId> parent_values;
        for (std::size_t step = 0; step < ordered.size(); ++step) {
            const auto &node = nodes[ordered[step]];
            const auto restriction = evidence.find(ordered[step]);
            const auto *allowed = restriction == evidence.end() ? nullptr : &restriction->second;
            candidates.clear();
            for (std::size_t a = 0; a < beam.size(); ++a) {
                parent_values.clear();
                for (const auto parent : node.parents) parent_values.push_back(beam[a].values[position[parent]]);
                const auto *table = probabilities(node, parent_values);
                // A missing or empty row is uniform over the node's values.
                if ((!table || table->empty()) && !node.values.empty()) {
                    uniform.clear();
                    for (const auto value : node.values) uniform.push_back({value, 1.0 / static_cast<double>(node.values.size())});
                    table = &uniform;
                }
                if (!table) continue;
                for (const auto &[value, probability] : *table) {
                    if (probability <= 0 || (allowed && !allows(*allowed, value))) continue;
                    candidates.push_back({a, value, beam[a].probability * probability});
                }
            }
            if (candidates.empty()) return {};
            if (candidates.size() > beam_width) {
                // heapq.nlargest: the likeliest, ties in their original order.
                std::stable_sort(candidates.begin(), candidates.end(),
                                 [](const auto &a, const auto &b) { return a.probability > b.probability; });
                candidates.resize(beam_width);
            }
            next.clear();
            next.reserve(candidates.size());
            for (const auto &candidate : candidates) {
                next.push_back({beam[candidate.assignment].values, candidate.probability});
                next.back().values[step] = candidate.value;
            }
            beam.swap(next);
        }
        std::vector<Weighted> result;
        std::unordered_map<ValueId, std::size_t> index;
        double total = 0;
        const auto at = position[target];
        for (const auto &assignment : beam) {
            const auto value = assignment.values[at];
            const auto [slot, inserted] = index.try_emplace(value, result.size());
            if (inserted) result.push_back({value, 0});
            result[slot->second].probability += assignment.probability;
            total += assignment.probability;
        }
        if (total <= 0) return {};
        for (auto &entry : result) entry.probability /= total;
        return result;
    }
};

Model::Model(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Model::Model(Model &&) noexcept = default;
Model &Model::operator=(Model &&) noexcept = default;
Model::~Model() = default;

Model Model::load(const fs::path &directory, const std::map<std::string, std::string> &file_sha256) {
    auto impl = std::make_unique<Impl>();
    const auto network_text = decompress(checked(directory, file_sha256, "fingerprint-network.json.zst"), "fingerprint-network.json.zst");
    const auto values_text = decompress(checked(directory, file_sha256, "values.json.zst"), "values.json.zst");
    impl->values_dat = checked(directory, file_sha256, "values.dat.zst");

    // values.json maps each value's hexadecimal offset in values.dat to its
    // length, in value index order.
    json::value values;
    try { values = json::parse(values_text); } catch (const std::exception &) { invalid("values.json is not JSON"); }
    if (!values.is_object()) invalid("values.json is not an object");
    impl->spans.reserve(values.as_object().size());
    for (const auto &item : values.as_object()) {
        std::uint64_t offset = 0;
        const auto key = std::string_view(item.key());
        const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), offset, 16);
        if (error != std::errc{} || end != key.data() + key.size() || !item.value().is_int64() ||
            item.value().as_int64() < 0 || item.value().as_int64() > UINT32_MAX)
            invalid("values.json has a malformed entry");
        impl->spans.emplace_back(offset, static_cast<std::uint32_t>(item.value().as_int64()));
    }

    json::value network;
    // Probabilities parse to the nearest double, as Python's json does.
    json::parse_options precise;
    precise.numbers = json::number_precision::precise;
    try { network = json::parse(network_text, {}, precise); } catch (const std::exception &) { invalid("the network is not JSON"); }
    const auto *list = network.is_object() ? network.as_object().if_contains("nodes") : nullptr;
    if (!list || !list->is_array() || list->as_array().empty()) invalid("the network has no nodes");
    const auto &definitions = list->as_array();
    impl->nodes.resize(definitions.size());
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (!definitions[i].is_object()) invalid("a node is not an object");
        const auto &definition = definitions[i].as_object();
        const auto name = string_or(definition, "name");
        if (name.empty() || !impl->by_name.emplace(name, i).second) invalid("a node has no name or a duplicate one");
        impl->nodes[i].name = name;
    }
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto &definition = definitions[i].as_object();
        auto &node = impl->nodes[i];
        node.ancestor.assign(definitions.size(), 0);
        const auto *parents = definition.if_contains("parentNames");
        const auto *possible = definition.if_contains("possibleValues");
        const auto *table = definition.if_contains("conditionalProbabilities");
        if (!parents || !parents->is_array() || !possible || !possible->is_array() || !table || !table->is_object())
            invalid("node " + node.name + " is incomplete");
        for (const auto &parent : parents->as_array()) {
            const auto found = parent.is_string() ? impl->by_name.find(std::string(parent.as_string())) : impl->by_name.end();
            // Sampling order is topological: parents come first.
            if (found == impl->by_name.end() || found->second >= i) invalid("node " + node.name + " has an unknown or later parent");
            node.parents.push_back(found->second);
            node.ancestor[found->second] = 1;
            for (std::size_t a = 0; a < i; ++a) node.ancestor[a] |= impl->nodes[found->second].ancestor[a];
        }
        for (const auto &value : possible->as_array()) {
            if (!value.is_string()) invalid("node " + node.name + " names a value that is not a string");
            const auto id = decode_index(value.as_string());
            if (id >= impl->spans.size()) invalid("node " + node.name + " names a value outside values.json");
            node.values.push_back(id);
        }
        auto sorted = node.values;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) invalid("node " + node.name + " names one value twice");
        parse_table(table->as_object(), 0, node.parents.size(), node.table);
    }
    return Model(std::move(impl));
}

std::optional<std::size_t> Model::node(std::string_view name) const {
    const auto found = impl_->by_name.find(std::string(name));
    if (found == impl_->by_name.end()) return std::nullopt;
    return found->second;
}
const std::vector<ValueId> &Model::possible_values(std::size_t node) const { return impl_->nodes.at(node).values; }
std::vector<Weighted> Model::trace(std::size_t target, const Evidence &evidence) const {
    if (target >= impl_->nodes.size()) throw std::out_of_range("fpgen node");
    return impl_->trace(target, evidence);
}

std::map<std::size_t, ValueId> Model::sample(const std::vector<std::size_t> &targets, const Evidence &evidence,
                                             Random &rng) const {
    const auto &nodes = impl_->nodes;
    // A node is independent of the nodes it does not descend from once its
    // parents are drawn, so drawing only the targets, the evidence and their
    // ancestors yields the distribution a whole-network draw gives them.
    std::vector<char> needed(nodes.size(), 0);
    auto need = [&](std::size_t node) {
        needed.at(node) = 1;
        for (std::size_t i = 0; i < nodes.size(); ++i) needed[i] |= nodes[node].ancestor[i];
    };
    for (const auto target : targets) need(target);
    for (const auto &[node, _] : evidence) need(node);
    Evidence current = evidence;
    std::map<std::size_t, ValueId> result;
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!needed[node]) continue;
        std::vector<Weighted> distribution;
        if (const auto own = current.find(node); own != current.end()) {
            const auto allowed = own->second;
            Evidence others = current;
            others.erase(node);
            double total = 0;
            for (const auto &entry : impl_->trace(node, others))
                if (allows(allowed, entry.value)) {
                    distribution.push_back(entry);
                    total += entry.probability;
                }
            if (distribution.empty() || total <= 0) {
                distribution.clear();
                for (const auto value : allowed) distribution.push_back({value, 1.0 / static_cast<double>(allowed.size())});
            } else {
                for (auto &entry : distribution) entry.probability /= total;
            }
        } else {
            distribution = impl_->trace(node, current);
            if (distribution.empty())
                throw Error(500, "fpgen_sampling", "fpgen cannot draw " + nodes[node].name + " under these conditions");
        }
        const auto value = choose(distribution, rng);
        result[node] = value;
        current[node] = {value};
    }
    return result;
}

std::unordered_map<ValueId, std::string> Model::values(std::vector<ValueId> ids) const {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const auto id : ids)
        if (id >= impl_->spans.size()) throw std::out_of_range("fpgen value");
    std::sort(ids.begin(), ids.end(), [&](ValueId a, ValueId b) { return impl_->spans[a].first < impl_->spans[b].first; });
    std::unordered_map<ValueId, std::string> result;
    if (ids.empty()) return result;
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), &ZSTD_freeDCtx);
    if (!context) throw std::bad_alloc();
    ZSTD_inBuffer input{impl_->values_dat.data(), impl_->values_dat.size(), 0};
    std::string chunk(ZSTD_DStreamOutSize(), '\0');
    std::uint64_t position = 0;  // decompressed bytes before `chunk`
    std::size_t first = 0;       // the first value not yet complete
    while (first < ids.size()) {
        ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
        const auto status = ZSTD_decompressStream(context.get(), &output, &input);
        if (ZSTD_isError(status)) invalid("values.dat does not decompress");
        const auto end = position + output.pos;
        for (auto i = first; i < ids.size() && impl_->spans[ids[i]].first < end; ++i) {
            const auto [offset, length] = impl_->spans[ids[i]];
            const auto from = std::max(offset, position), to = std::min(offset + length, end);
            if (from < to) result[ids[i]].append(chunk.data() + (from - position), static_cast<std::size_t>(to - from));
        }
        while (first < ids.size() && impl_->spans[ids[first]].first + impl_->spans[ids[first]].second <= end) {
            result.try_emplace(ids[first]);  // zero-length values
            ++first;
        }
        position = end;
        if (output.pos == 0 && input.pos == input.size) break;
    }
    if (first < ids.size()) invalid("values.dat ends before a value it lists");
    return result;
}
} // namespace ais2api::fpgen
