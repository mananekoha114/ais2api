# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# The Camoufox a dev bundle carries must be the pinned build as setup
# installed it: BUNDLE holds its marker and the browser.
set(marker_file "${BUNDLE}/.ais2api-runtime.json")
if(NOT EXISTS "${marker_file}")
  message(FATAL_ERROR "${BUNDLE} is not a Camoufox install setup made; scripts/build-dev.ps1 runs setup to make it")
endif()
file(READ "${marker_file}" marker)
foreach(key browser browser_version platform sha256)
  string(JSON marker_${key} ERROR_VARIABLE marker_error GET "${marker}" ${key})
endforeach()
if(NOT marker_browser STREQUAL "camoufox" OR NOT marker_browser_version STREQUAL "${VERSION}" OR
   NOT marker_platform STREQUAL "${PLATFORM}" OR NOT marker_sha256 STREQUAL "${SHA256}")
  message(FATAL_ERROR "${BUNDLE} is not Camoufox ${VERSION} for ${PLATFORM} as runtime-manifest.json pins it; rerun setup")
endif()
if(NOT EXISTS "${BUNDLE}/camoufox.exe")
  message(FATAL_ERROR "${BUNDLE} holds no camoufox.exe")
endif()
