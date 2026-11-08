# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# A dev bundle may carry the pinned Camoufox: an install setup made
# (scripts/build-dev.ps1 or build-dev.sh runs it), copied to
# bin/camoufox-runtime/ as is.
set(AIS2API_CAMOUFOX_BUNDLE_DIR "" CACHE PATH "A Camoufox install setup made, carried by a dev bundle")
if(AIS2API_CAMOUFOX_BUNDLE_DIR)
  if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(camoufox_platform windows-x64)
    set(camoufox_program camoufox.exe)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(camoufox_platform linux-x64)
    set(camoufox_program camoufox-bin)
  else()
    message(FATAL_ERROR "Dev bundles carry Camoufox for Windows x64 and Linux x64 only")
  endif()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/runtime-manifest.json" camoufox_manifest)
  string(JSON camoufox_version GET "${camoufox_manifest}" camoufox version)
  string(JSON camoufox_sha256 GET "${camoufox_manifest}" camoufox artifacts ${camoufox_platform} sha256)
  get_filename_component(camoufox_bundle "${AIS2API_CAMOUFOX_BUNDLE_DIR}" ABSOLUTE)
  # Checked when it is installed, since setup may run after configuration.
  install(CODE "set(BUNDLE [[${camoufox_bundle}]])
set(VERSION [[${camoufox_version}]])
set(PLATFORM ${camoufox_platform})
set(PROGRAM ${camoufox_program})
set(SHA256 [[${camoufox_sha256}]])
include([[${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-camoufox-bundle.cmake]])")
  install(DIRECTORY "${camoufox_bundle}/"
    DESTINATION "bin/camoufox-runtime/${camoufox_version}-${camoufox_platform}" USE_SOURCE_PERMISSIONS)
endif()
