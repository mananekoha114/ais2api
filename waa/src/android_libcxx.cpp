// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The android-arm64 SpiderMonkey archive was built to link libc++ statically,
// so its mozglue refers to std::string::push_back(char) with hidden
// visibility. A hidden reference has to be defined in the executable itself,
// which Termux's libc++_shared.so cannot do; this instantiation does.
#include <string>

template void std::basic_string<char>::push_back(char);
