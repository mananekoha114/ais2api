// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A port of fpgen's Bayesian network sampler (scrapfly/fingerprint-generator
// 1.3.0, Apache License 2.0; see licenses/fpgen-LICENSE.txt). Only the parts
// the Firefox identity generator uses: conditional distributions by beam
// search, and sampling nodes in the network's own order.

#pragma once
#include "ais2api/common.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace ais2api::fpgen {
// Seeded xoshiro256** (state from SplitMix64) with its own conversions, so a
// seed draws the same values with every compiler and standard library.
class Random {
  public:
    explicit Random(std::uint64_t seed);
    std::uint64_t next();
    // [0, 1) with 53 random bits.
    double uniform();
    // [0, bound), without modulo bias. `bound` must be positive.
    std::uint64_t below(std::uint64_t bound);

  private:
    std::uint64_t state_[4];
};
// A value is its index in values.json; every node names its values that way.
using ValueId = std::uint32_t;
// The values each constrained node may take, by node index (sorted, unique).
using Evidence = std::map<std::size_t, std::vector<ValueId>>;
struct Weighted {
    ValueId value;
    double probability;
    bool operator==(const Weighted &) const = default;
};
// fpgen's base85 value index ("jQ" -> 141).
ValueId decode_index(std::string_view text);
// The first value whose cumulative probability passes one uniform draw, as
// fpgen's sample_value_from_distribution picks; the first value otherwise.
ValueId choose(const std::vector<Weighted> &distribution, Random &rng);
// fpgen's model: fingerprint-network.json.zst, values.json.zst and
// values.dat.zst from one release.
class Model {
  public:
    // Reads the three files from `directory`, each checked against its sha256
    // in `file_sha256` first.
    static Model load(const fs::path &directory, const std::map<std::string, std::string> &file_sha256);
    Model(Model &&) noexcept;
    Model &operator=(Model &&) noexcept;
    ~Model();
    std::optional<std::size_t> node(std::string_view name) const;
    const std::vector<ValueId> &possible_values(std::size_t node) const;
    // The distribution of `target` given `evidence`, in fpgen's order: beam
    // search over the target's and the evidence's ancestors, the beam cut to
    // the 1000 likeliest assignments. Empty when the evidence is impossible.
    std::vector<Weighted> trace(std::size_t target, const Evidence &evidence) const;
    // Values for `targets` and every node they or the evidence depend on, each
    // drawn in sampling order from its distribution given what was drawn
    // before it, as fpgen's generate_consistent_sample does for the whole
    // network. A node with evidence draws among its allowed values.
    std::map<std::size_t, ValueId> sample(const std::vector<std::size_t> &targets, const Evidence &evidence,
                                          Random &rng) const;
    // The JSON text of each value, read in one streaming pass over values.dat.
    std::unordered_map<ValueId, std::string> values(std::vector<ValueId> ids) const;

  private:
    struct Impl;
    explicit Model(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace ais2api::fpgen
