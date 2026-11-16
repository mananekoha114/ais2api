# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Static SpiderMonkey 158.0.1 embedding archive across all four supported
# native targets: windows-x64, linux-x64, linux-arm64 and android-arm64.
#
# Sets AIS2API_SPIDERMONKEY_TARGET, AIS2API_SPIDERMONKEY_VERSION,
# AIS2API_SPIDERMONKEY_LICENSES and AIS2API_RESOLVED_SPIDERMONKEY_DIR.
set(AIS2API_SPIDERMONKEY_RELEASE_REPO "mananekoha114/ais2api-spidermonkey" CACHE STRING
  "GitHub repository providing SpiderMonkey static archives")
set(AIS2API_SPIDERMONKEY_TAG "spidermonkey-158.0.1-r2" CACHE STRING
  "Release tag for SpiderMonkey static archives")
set(AIS2API_SPIDERMONKEY_ARCHIVE_VERSION "158.0.1")
set(AIS2API_SPIDERMONKEY_URL "" CACHE STRING
  "Optional archive URL override for SpiderMonkey; empty uses the pinned GitHub Release")
set(AIS2API_SPIDERMONKEY_SHA256 "" CACHE STRING
  "Optional SHA-256 override for AIS2API_SPIDERMONKEY_URL")

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
  set(spidermonkey_platform "windows-x64")
  set(spidermonkey_default_sha256 "a96e50ff8cb516e5daf902bfda62f1f8999106726916cb33e4c0e2525d37c838")
elseif((ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR DEFINED ENV{TERMUX_VERSION}
        OR "$ENV{PREFIX}" MATCHES "com\\.termux" OR "$ENV{TERMUX__PREFIX}" MATCHES "com\\.termux")
       AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(spidermonkey_platform "android-arm64")
  set(spidermonkey_default_sha256 "beff3f5728b3dd5680f0f0fcf15800f39a2a49d7e86cecfa6932af235bf43e95")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(spidermonkey_platform "linux-x64")
  set(spidermonkey_default_sha256 "2c3acad9eb4197d3689b9ea669fd27e5febe54cd7da04170e99bac87540e84f7")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(spidermonkey_platform "linux-arm64")
  set(spidermonkey_default_sha256 "4fb6ac73d8a472d747e4a7fa15b44634ef0660ae665c4b68ce963fd94f8e1b12")
else()
  message(FATAL_ERROR "AIS2API_SPIDERMONKEY supports windows-x64, linux-x64, linux-arm64 and android-arm64")
endif()

set(spidermonkey_archive "spidermonkey-${AIS2API_SPIDERMONKEY_ARCHIVE_VERSION}-${spidermonkey_platform}.tar.xz")
if(AIS2API_SPIDERMONKEY_SHA256)
  set(spidermonkey_sha256 "${AIS2API_SPIDERMONKEY_SHA256}")
else()
  set(spidermonkey_sha256 "${spidermonkey_default_sha256}")
endif()

set(spidermonkey_override "${AIS2API_SPIDERMONKEY_DIR}")
if(NOT spidermonkey_override AND DEFINED ENV{AIS2API_SPIDERMONKEY_DIR} AND NOT "$ENV{AIS2API_SPIDERMONKEY_DIR}" STREQUAL "")
  set(spidermonkey_override "$ENV{AIS2API_SPIDERMONKEY_DIR}")
endif()

if(spidermonkey_override)
  get_filename_component(spidermonkey_dir "${spidermonkey_override}" ABSOLUTE)
else()
  string(LENGTH "${spidermonkey_sha256}" spidermonkey_sha256_length)
  if(NOT spidermonkey_sha256_length EQUAL 64 OR NOT spidermonkey_sha256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "No release SHA-256 is pinned yet for ${spidermonkey_archive}; set AIS2API_SPIDERMONKEY_DIR to an unpacked ${spidermonkey_platform} SpiderMonkey build")
  endif()
  set(spidermonkey_dir "${CMAKE_BINARY_DIR}/_deps/${AIS2API_SPIDERMONKEY_TAG}-${spidermonkey_platform}")
  set(spidermonkey_marker "${spidermonkey_dir}/.ais2api-unpacked")
  set(unpacked "")
  if(EXISTS "${spidermonkey_marker}")
    file(READ "${spidermonkey_marker}" unpacked)
    string(STRIP "${unpacked}" unpacked)
  endif()
  if(NOT unpacked STREQUAL spidermonkey_sha256)
    set(archive "${CMAKE_BINARY_DIR}/_deps/${spidermonkey_archive}")
    if(AIS2API_SPIDERMONKEY_URL)
      set(url "${AIS2API_SPIDERMONKEY_URL}")
    else()
      set(url "https://github.com/${AIS2API_SPIDERMONKEY_RELEASE_REPO}/releases/download/${AIS2API_SPIDERMONKEY_TAG}/${spidermonkey_archive}")
    endif()
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${spidermonkey_sha256} TLS_VERIFY ON STATUS status)
    list(GET status 0 status_code)
    if(NOT status_code EQUAL 0)
      list(GET status 1 status_text)
      file(REMOVE "${archive}")
      message(FATAL_ERROR "Cannot download ${url}: ${status_text}. Set AIS2API_SPIDERMONKEY_DIR to an unpacked copy instead")
    endif()
    file(REMOVE_RECURSE "${spidermonkey_dir}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${spidermonkey_dir}")
    file(REMOVE "${archive}")
    file(WRITE "${spidermonkey_marker}" "${spidermonkey_sha256}")
  endif()
endif()

if(NOT EXISTS "${spidermonkey_dir}/BUILDINFO.json")
  file(GLOB spidermonkey_nested "${spidermonkey_dir}/*/BUILDINFO.json")
  list(LENGTH spidermonkey_nested nested_count)
  if(nested_count EQUAL 1)
    list(GET spidermonkey_nested 0 nested_buildinfo)
    get_filename_component(spidermonkey_dir "${nested_buildinfo}" DIRECTORY)
  else()
    message(FATAL_ERROR "${spidermonkey_dir} holds no BUILDINFO.json")
  endif()
endif()

set(AIS2API_RESOLVED_SPIDERMONKEY_DIR "${spidermonkey_dir}" CACHE INTERNAL "Resolved SpiderMonkey directory")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${spidermonkey_dir}/BUILDINFO.json")
file(READ "${spidermonkey_dir}/BUILDINFO.json" spidermonkey_buildinfo)
string(JSON AIS2API_SPIDERMONKEY_VERSION GET "${spidermonkey_buildinfo}" spidermonkey)
string(JSON spidermonkey_target GET "${spidermonkey_buildinfo}" target)
if(NOT spidermonkey_target STREQUAL spidermonkey_platform)
  message(FATAL_ERROR "${spidermonkey_dir} targets ${spidermonkey_target}, expected ${spidermonkey_platform}")
endif()
# The Windows archive is compiled against the STL of the MSVC toolset its
# BUILDINFO.json names, and MSVC links objects only with a toolset at least as
# new as the newest that compiled any of them; an older one fails on
# unresolved STL symbols such as __std_rotate.
if(MSVC)
  string(JSON spidermonkey_msvc ERROR_VARIABLE spidermonkey_msvc_error
    GET "${spidermonkey_buildinfo}" toolchain msvc)
  if(NOT spidermonkey_msvc_error AND spidermonkey_msvc MATCHES "^14\\.([0-9]+)")
    set(spidermonkey_msvc_minor "${CMAKE_MATCH_1}")
    math(EXPR spidermonkey_msvc_needed "1900 + ${spidermonkey_msvc_minor}")
    if(MSVC_VERSION LESS spidermonkey_msvc_needed)
      message(FATAL_ERROR
        "The ${spidermonkey_platform} SpiderMonkey archive was built with MSVC ${spidermonkey_msvc} and "
        "links only with MSVC toolset 14.${spidermonkey_msvc_minor} or newer; this "
        "compiler is ${CMAKE_CXX_COMPILER_VERSION}. Use a newer toolset, or configure with "
        "-DAIS2API_SPIDERMONKEY=OFF -DAIS2API_CURL_IMPERSONATE=OFF")
    endif()
  endif()
endif()
if(NOT EXISTS "${spidermonkey_dir}/include/mozjs.h")
  message(FATAL_ERROR "${spidermonkey_dir} holds no include/mozjs.h")
endif()

# The artifact's shim must match the mozjs.cpp and mozjs.h in this tree.
set(spidermonkey_shim_dir "${PROJECT_SOURCE_DIR}/waa/third_party/aistudio2api/mozjs")
foreach(name mozjs.cpp mozjs.h)
  string(JSON built GET "${spidermonkey_buildinfo}" shim "${name}")
  file(SHA256 "${spidermonkey_shim_dir}/${name}" present)
  if(NOT built STREQUAL present)
    message(FATAL_ERROR "${spidermonkey_dir} was built from another ${name}; rebuild it with tools/spidermonkey")
  endif()
endforeach()

string(JSON spidermonkey_library_count LENGTH "${spidermonkey_buildinfo}" link)
if(spidermonkey_library_count EQUAL 0)
  message(FATAL_ERROR "${spidermonkey_dir}/BUILDINFO.json lists no libraries to link")
endif()
math(EXPR spidermonkey_library_last "${spidermonkey_library_count} - 1")
set(spidermonkey_archive_targets "")
set(spidermonkey_system_libraries "")
foreach(index RANGE ${spidermonkey_library_last})
  string(JSON name GET "${spidermonkey_buildinfo}" link ${index})
  if(name MATCHES "\\.(dll|so|dylib)(\\.[0-9]+)*$")
    message(FATAL_ERROR "${spidermonkey_dir}/BUILDINFO.json lists dynamic library ${name}; only static archives are supported")
  endif()
  if(name MATCHES "^lib/")
    set(full_path "${spidermonkey_dir}/${name}")
    if(NOT EXISTS "${full_path}")
      message(FATAL_ERROR "${spidermonkey_dir} is missing static archive ${name}")
    endif()
    set(imported_target "ais2api_spidermonkey_lib_${index}")
    add_library(${imported_target} STATIC IMPORTED GLOBAL)
    set_target_properties(${imported_target} PROPERTIES IMPORTED_LOCATION "${full_path}")
    list(APPEND spidermonkey_archive_targets "${imported_target}")
  elseif(name MATCHES "^(z|zlib|zlib\\.lib)$")
    find_package(ZLIB REQUIRED)
    list(APPEND spidermonkey_system_libraries ZLIB::ZLIB)
  elseif(name STREQUAL "pthread")
    list(APPEND spidermonkey_system_libraries Threads::Threads)
  else()
    list(APPEND spidermonkey_system_libraries "${name}")
  endif()
endforeach()

set(AIS2API_SPIDERMONKEY_LICENSES "")
file(GLOB spidermonkey_license_candidates "${spidermonkey_dir}/licenses/*")
foreach(candidate IN LISTS spidermonkey_license_candidates)
  if(NOT IS_DIRECTORY "${candidate}")
    list(APPEND AIS2API_SPIDERMONKEY_LICENSES "${candidate}")
  endif()
endforeach()
if(NOT AIS2API_SPIDERMONKEY_LICENSES)
  file(GLOB spidermonkey_fallback_candidates "${PROJECT_SOURCE_DIR}/licenses/spidermonkey/*")
  foreach(candidate IN LISTS spidermonkey_fallback_candidates)
    if(NOT IS_DIRECTORY "${candidate}")
      list(APPEND AIS2API_SPIDERMONKEY_LICENSES "${candidate}")
    endif()
  endforeach()
endif()
if(NOT AIS2API_SPIDERMONKEY_LICENSES)
  message(FATAL_ERROR "No SpiderMonkey license files found in ${spidermonkey_dir}/licenses or licenses/spidermonkey")
endif()

add_library(ais2api_spidermonkey INTERFACE IMPORTED GLOBAL)
target_include_directories(ais2api_spidermonkey INTERFACE "${spidermonkey_dir}/include")
if(UNIX AND NOT APPLE AND spidermonkey_archive_targets)
  string(JOIN "," spidermonkey_rescan_group ${spidermonkey_archive_targets})
  target_link_libraries(ais2api_spidermonkey INTERFACE
    "$<LINK_GROUP:RESCAN,${spidermonkey_rescan_group}>"
    ${spidermonkey_system_libraries})
else()
  target_link_libraries(ais2api_spidermonkey INTERFACE
    ${spidermonkey_archive_targets}
    ${spidermonkey_system_libraries})
endif()
set(AIS2API_SPIDERMONKEY_TARGET ais2api_spidermonkey)
message(STATUS "SpiderMonkey ${AIS2API_SPIDERMONKEY_VERSION} (${spidermonkey_platform}) from ${spidermonkey_dir}")
