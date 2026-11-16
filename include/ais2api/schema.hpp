// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "common.hpp"

namespace ais2api {
struct UnsupportedSchema : Error {
    explicit UnsupportedSchema(std::string message)
        : Error(400, "invalid_argument", "schema: " + std::move(message)) {}
};

// Lower client JSON Schema into the verified AI Studio Schema subset.
// Unsupported constraints are reported by keyword, never by caller values.
// A nested schema without a type accepts any JSON value; it becomes a string
// the model fills with JSON text. `json_text` receives the paths of such
// values reachable through properties (a string key) and items (null), so
// decode_json_text_args can restore the values in function call arguments.
json::object prepare_schema(const json::object &schema, json::array *ignored = nullptr,
                            std::string_view location = "schema", bool ignore_unsupported = false,
                            json::array *json_text = nullptr);
}
