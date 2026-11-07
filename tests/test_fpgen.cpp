// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "fpgen_fixture.hpp"
#include <catch2/catch_approx.hpp>

using namespace ais2api;
using namespace ais2api::fpgen;
using Catch::Approx;

namespace {
using namespace fpgen_fixture;
double probability(const std::vector<Weighted> &distribution, ValueId value) {
    for (const auto &entry : distribution)
        if (entry.value == value) return entry.probability;
    return 0;
}

// A -> B -> C, with D on its own:
//   A: a0 .6, a1 .4
//   B | a0: b0 .5, b1 .5     B | a1: b1 .25, b2 .75
//   C | b0: c0 1             C | b1: c0 .5, c1 .5     C | b2: (missing, so uniform)
//   D: d0 .9, d1 .1, d2 0
struct Chain : Fixture {
    ValueId a0 = add(R"("a0")"), a1 = add(R"("a1")"), b0 = add(R"("b0")"), b1 = add(R"("b1")"), b2 = add(R"("b2")"),
            c0 = add(R"("c0")"), c1 = add(R"("c1")"), d0 = add(R"("d0")"), d1 = add(R"({"d":1})"),
            d2 = add(R"("d2")");
    Model model = load({
        node("A", {}, codes({a0, a1}), table({{a0, .6}, {a1, .4}})),
        node("B", {"A"}, codes({b0, b1, b2}),
             {{code(a0), table({{b0, .5}, {b1, .5}})}, {code(a1), table({{b1, .25}, {b2, .75}})}}),
        node("C", {"B"}, codes({c0, c1}), {{code(b0), table({{c0, 1}})}, {code(b1), table({{c0, .5}, {c1, .5}})}}),
        node("D", {}, codes({d0, d1, d2}), table({{d0, .9}, {d1, .1}, {d2, 0}})),
    });
    std::size_t A = *model.node("A"), B = *model.node("B"), C = *model.node("C"), D = *model.node("D");
};
} // namespace

TEST_CASE("fpgen value indices decode as Python's b85decode does", "[fpgen]") {
    CHECK(decode_index("0") == 0);
    CHECK(decode_index("!") == 0);
    CHECK(decode_index("00") == 0);
    CHECK(decode_index("1^") == 6);
    CHECK(decode_index("jQ") == 141);
    CHECK(decode_index("0~") == 3);
    CHECK(decode_index("0<i") == 689);
    CHECK(decode_index("7eW") == 5954);
    CHECK(decode_index("0000~") == 84);
    for (ValueId value : {0u, 1u, 6u, 141u, 255u, 256u, 689u, 5954u, 65535u, 65536u})
        CHECK(decode_index(code(value)) == value);
    for (const auto text : {"~", "~~~", "~~~~", "0 ", "\"0"}) CHECK_THROWS_AS(decode_index(text), Error);
}

TEST_CASE("fpgen's random numbers are xoshiro256** seeded by SplitMix64", "[fpgen]") {
    Random rng(42);
    CHECK(rng.next() == 0x15780b2e0c2ec716ull);
    CHECK(rng.next() == 0x6104d9866d113a7eull);
    CHECK(rng.next() == 0xae17533239e499a1ull);
    CHECK(Random(42).uniform() == 0.08386297105988216);
    Random bounded(7);
    for (int i = 0; i < 1000; ++i) {
        const auto value = bounded.uniform();
        CHECK((value >= 0 && value < 1));
        CHECK(bounded.below(13) < 13);
    }
    CHECK_THROWS(bounded.below(0));
}

TEST_CASE("fpgen traces conditional distributions over the evidence's ancestors", "[fpgen]") {
    Chain chain;
    const auto &model = chain.model;
    CHECK(model.possible_values(chain.B) == std::vector<ValueId>{chain.b0, chain.b1, chain.b2});
    CHECK_FALSE(model.node("E"));

    const auto c = model.trace(chain.C, {});
    REQUIRE(c.size() == 2);
    CHECK(c[0].value == chain.c0);  // first seen first
    CHECK(c[0].probability == Approx(.65));
    CHECK(c[1].probability == Approx(.35));

    // Evidence below the target conditions it too.
    const auto a = model.trace(chain.A, {{chain.C, {chain.c1}}});
    REQUIRE(a.size() == 2);
    CHECK(a[0].value == chain.a0);
    CHECK(a[0].probability == Approx(3.0 / 7));
    CHECK(a[1].probability == Approx(4.0 / 7));

    // Several allowed values, and an unrelated node that changes nothing.
    const auto b = model.trace(chain.B, {{chain.A, {chain.a0, chain.a1}}, {chain.D, {chain.d1}}});
    CHECK(probability(b, chain.b0) == Approx(.3));
    CHECK(probability(b, chain.b1) == Approx(.4));
    CHECK(probability(b, chain.b2) == Approx(.3));

    CHECK(model.trace(chain.C, {{chain.B, {chain.b0}}, {chain.C, {chain.c1}}}).empty());
    CHECK(model.trace(chain.C, {{chain.B, {chain.b2}}}) ==
          std::vector<Weighted>{{chain.c0, .5}, {chain.c1, .5}});
}

TEST_CASE("fpgen keeps the 1000 likeliest assignments, ties in order", "[fpgen]") {
    // R: r0 .5, r1 .5; S | r0: 1000 values at .001 each; S | r1: s0 1. The
    // 1001 assignments of S lose the last one of r0's.
    Fixture fixture;
    const auto r0 = fixture.add("0"), r1 = fixture.add("1");
    std::vector<ValueId> s;
    for (int i = 0; i < 1000; ++i) s.push_back(fixture.add(std::to_string(100 + i)));
    json::array s_codes;
    json::object uniform;
    for (const auto value : s) {
        s_codes.push_back(json::value(code(value)));
        uniform[code(value)] = .001;
    }
    const auto model = fixture.load({
        node("R", {}, codes({r0, r1}), table({{r0, .5}, {r1, .5}})),
        node("S", {"R"}, s_codes, {{code(r0), uniform}, {code(r1), table({{s[0], 1}})}}),
    });
    const auto distribution = model.trace(*model.node("S"), {});
    REQUIRE(distribution.size() == 999);
    CHECK(distribution.front().value == s[0]);
    CHECK(distribution.front().probability == Approx(.5005 / .9995));
    CHECK(distribution.back().value == s[998]);
    CHECK(probability(distribution, s[999]) == 0);
}

TEST_CASE("fpgen samples the targets, the evidence and their ancestors", "[fpgen]") {
    Chain chain;
    const auto &model = chain.model;
    Random rng(1);
    const auto draw = model.sample({chain.C}, {{chain.A, {chain.a1}}}, rng);
    CHECK(draw.size() == 3);
    CHECK(draw.at(chain.A) == chain.a1);
    CHECK_FALSE(draw.contains(chain.D));

    // Same seed, same draw.
    Random first(99), second(99);
    for (int i = 0; i < 20; ++i)
        CHECK(model.sample({chain.C, chain.D}, {}, first) == model.sample({chain.C, chain.D}, {}, second));

    // The frequencies follow the conditional distributions.
    const int draws = 20000;
    int c0 = 0, a1 = 0, b2 = 0;
    for (int i = 0; i < draws; ++i) {
        c0 += model.sample({chain.C}, {{chain.A, {chain.a1}}}, rng).at(chain.C) == chain.c0;
        a1 += model.sample({chain.A}, {{chain.C, {chain.c1}}}, rng).at(chain.A) == chain.a1;
        b2 += model.sample({chain.B}, {{chain.B, {chain.b0, chain.b2}}}, rng).at(chain.B) == chain.b2;
    }
    CHECK(c0 / double(draws) == Approx(.5).margin(.02));      // .25 * .5 + .75 * .5
    CHECK(a1 / double(draws) == Approx(4.0 / 7).margin(.02));
    CHECK(b2 / double(draws) == Approx(.5).margin(.02));      // .3 against .3

    // Evidence the network gives no weight falls back to uniform on its own
    // node, and leaves nothing to draw for an ancestor.
    CHECK(model.sample({chain.D}, {{chain.D, {chain.d2}}}, rng).at(chain.D) == chain.d2);
    CHECK_THROWS_AS(model.sample({chain.C}, {{chain.B, {chain.b0}}, {chain.C, {chain.c1}}}, rng), Error);
}

TEST_CASE("fpgen reads values in one pass over values.dat", "[fpgen]") {
    Fixture fixture;
    // The filler pushes `straddling` across the first decompressed block.
    const auto filler = fixture.add('"' + std::string(131068, 'x') + '"');
    const auto straddling = fixture.add(R"({"screen":{"width":1920}})");
    const auto last = fixture.add(R"("Windows")");
    const auto model = fixture.load({node("V", {}, codes({filler, straddling, last}), table({{straddling, 1}}))});
    const auto values = model.values({last, straddling, last});
    REQUIRE(values.size() == 2);
    CHECK(values.at(straddling) == R"({"screen":{"width":1920}})");
    CHECK(values.at(last) == R"("Windows")");
    CHECK(model.values({filler}).at(filler).size() == 131070);
    CHECK(model.values({}).empty());
    CHECK_THROWS(model.values({3}));
}

TEST_CASE("fpgen refuses missing, altered and malformed models", "[fpgen]") {
    Fixture fixture;
    const auto v = fixture.add("1");
    const json::array nodes{node("V", {}, codes({v}), table({{v, 1}}))};
    REQUIRE_NOTHROW(fixture.load(nodes));
    auto pins = fixture.sha256s;
    pins["values.dat.zst"] = std::string(64, '0');
    try {
        Model::load(fixture.directory.path, pins);
        FAIL("an altered file loaded");
    } catch (const Error &error) {
        CHECK(error.status == 503);
        CHECK(error.code == "browser_not_configured");
    }
    Directory empty;
    CHECK_THROWS_AS(Model::load(empty.path, fixture.sha256s), Error);

    // A parent after its child, a duplicate name, a value outside values.json.
    CHECK_THROWS_AS(fixture.load({node("C", {"P"}, codes({v}), {{code(v), table({{v, 1}})}}),
                                  node("P", {}, codes({v}), table({{v, 1}}))}),
                    Error);
    CHECK_THROWS_AS(fixture.load({nodes[0], nodes[0]}), Error);
    CHECK_THROWS_AS(fixture.load({node("V", {}, codes({7}), table({{7, 1}}))}), Error);
}
