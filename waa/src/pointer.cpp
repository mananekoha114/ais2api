// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The page's trusted pointer. After AIStudio2API's internal/waa/runtime.go
// (MIT).
#include "ais2api/waa/runtime.hpp"

namespace ais2api::waa {
namespace {
using std::chrono::milliseconds;
using Action = PointerStep::Action;
} // namespace

PointerEvents Pointer::step(const PointerStep &step) {
    std::lock_guard lock(mutex_);
    PointerEvents events{at_, at_, focus_, focus_, rounds_, {}};
    switch (step.action) {
    case Action::move:
        events.to = step.to;
        if (events.from == events.to) events.types = {"pointermove", "mousemove"};
        else if (events.from.empty()) events.types = {"pointerover", "pointermove", "mouseover", "mousemove"};
        else events.types = {"pointerout", "pointerover", "pointermove", "mouseout", "mouseover", "mousemove"};
        break;
    case Action::press:
        events.types = {"pointerdown", "mousedown"};
        events.focused = events.from;
        if (events.focused != events.blurred) {
            if (!events.blurred.empty()) events.types.emplace_back("focusout");
            events.types.emplace_back("focusin");
        }
        break;
    case Action::release:
        events.types = {"pointerup", "mouseup", "click"};
        clicked_ = true;
        break;
    case Action::replace:
        if (events.from == "rerun") {
            events.to = "content";
            if (events.focused == "rerun") events.focused.clear();
        } else if (events.from != "run") {
            return events;
        }
        events.types = {"pointerover", "mouseover"};
        break;
    }
    at_ = events.to;
    focus_ = events.focused;
    return events;
}

void Pointer::set_rounds(int rounds) {
    std::lock_guard lock(mutex_);
    rounds_ = rounds;
}

bool Pointer::clicked() const {
    std::lock_guard lock(mutex_);
    return clicked_;
}

// The Camoufox backend moved the pointer to the Run button's centre in 120 ms,
// pressed, waited 60 ms and released; Firefox dispatched the groups of events
// at these intervals.
const std::vector<PointerStep> &run_click() {
    static const std::vector<PointerStep> steps{
        {milliseconds(0), Action::move, "run"}, {milliseconds(5), Action::press, {}}, {milliseconds(65), Action::release, {}}};
    return steps;
}

// The same for Rerun, after the pointer rested on the last turn until its
// Rerun button appeared.
const std::vector<PointerStep> &rerun_click() {
    static const std::vector<PointerStep> steps{{milliseconds(0), Action::move, "turn"},
                                                {milliseconds(1000), Action::move, "rerun"},
                                                {milliseconds(5), Action::press, {}},
                                                {milliseconds(65), Action::release, {}}};
    return steps;
}
} // namespace ais2api::waa
