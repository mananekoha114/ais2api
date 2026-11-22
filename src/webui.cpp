// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
#include "ais2api/webui.hpp"
#include <algorithm>
#include <array>

namespace ais2api::webui {
namespace {
#ifdef AIS2API_BUILD_WEBUI
#include "webui_assets.inc"
#else
const std::array<Asset, 0> assets{};
#endif
}
bool enabled() { return !std::empty(assets); }

std::optional<Asset> find(std::string_view path) {
    // Keep API errors and arbitrary/missing files out of the SPA fallback.
    constexpr std::array routes{"/", "/login", "/change-password", "/accounts", "/models", "/keys",
                                "/stats", "/logs", "/config", "/diagnostics", "/settings"};
    if (std::find(routes.begin(), routes.end(), path) != routes.end()) path = "/index.html";
    for (const auto& asset : assets)
        if (asset.path == path) return asset;
    return std::nullopt;
}
} // namespace ais2api::webui
