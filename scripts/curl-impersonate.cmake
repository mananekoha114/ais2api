# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Static curl-impersonate (lexiforest base + Firefox 158 network fingerprint
# patches): libcurl + bssl_-prefixed BoringSSL + nghttp2 + brotli linked
# statically across windows-x64, linux-x64, linux-arm64 and android-arm64.
#
# BoringSSL is compiled with BORINGSSL_PREFIX=bssl_ and no BoringSSL headers
# are placed in include/, so static OpenSSL 3, zlib and zstd link in the same
# executable with zero symbol clashes and zero runtime .dll/.so files.
#
# Sets AIS2API_CURL_TARGET, AIS2API_NGHTTP2_TARGET (when BUILD_TESTING is ON),
# AIS2API_CURL_IMPERSONATE_LICENSES and AIS2API_RESOLVED_CURL_IMPERSONATE_DIR.
set(AIS2API_CURL_IMPERSONATE_VERSION "2.3.2")
set(AIS2API_CURL_IMPERSONATE_RELEASE_REPO "mananekoha114/ais2api-spidermonkey" CACHE STRING
  "GitHub repository providing static curl-impersonate archives")
set(AIS2API_CURL_IMPERSONATE_TAG "curl-impersonate-2.3.2-r2" CACHE STRING
  "Release tag for static curl-impersonate archives")
set(AIS2API_CURL_IMPERSONATE_DIR "" CACHE PATH
  "An unpacked static curl-impersonate archive; empty downloads the pinned one")
set(AIS2API_CURL_IMPERSONATE_URL "" CACHE STRING
  "Optional archive URL override for curl-impersonate; empty uses the pinned GitHub Release")
set(AIS2API_CURL_IMPERSONATE_SHA256 "" CACHE STRING
  "Optional SHA-256 override for AIS2API_CURL_IMPERSONATE_URL")

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
  set(curl_impersonate_platform "windows-x64")
  set(curl_impersonate_default_sha256 "ad708cb957aa6ff2c88708955f6778c40c389451b0abd9e214377f8b671f4c9d")
elseif((ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR DEFINED ENV{TERMUX_VERSION}
        OR "$ENV{PREFIX}" MATCHES "com\\.termux" OR "$ENV{TERMUX__PREFIX}" MATCHES "com\\.termux")
       AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(curl_impersonate_platform "android-arm64")
  set(curl_impersonate_default_sha256 "cbfb6bea88f2648fb85ddf648d6a145a53be83a3787634f67342e0875871820d")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(curl_impersonate_platform "linux-x64")
  set(curl_impersonate_default_sha256 "39030d9722238a7753e8140f866d334ac90db678c369fa42ffb01de949344d87")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(curl_impersonate_platform "linux-arm64")
  set(curl_impersonate_default_sha256 "aa3b8518ec46d38a00f0a1423bf6cc96a00f2c0627a2c50a60b01a788429c6cb")
else()
  message(FATAL_ERROR "AIS2API_CURL_IMPERSONATE supports windows-x64, linux-x64, linux-arm64 and android-arm64")
endif()

set(curl_impersonate_archive "curl-impersonate-${AIS2API_CURL_IMPERSONATE_VERSION}-${curl_impersonate_platform}.tar.gz")
if(AIS2API_CURL_IMPERSONATE_SHA256)
  set(curl_impersonate_sha256 "${AIS2API_CURL_IMPERSONATE_SHA256}")
else()
  set(curl_impersonate_sha256 "${curl_impersonate_default_sha256}")
endif()

set(curl_impersonate_override "${AIS2API_CURL_IMPERSONATE_DIR}")
if(NOT curl_impersonate_override AND DEFINED ENV{AIS2API_CURL_IMPERSONATE_DIR} AND NOT "$ENV{AIS2API_CURL_IMPERSONATE_DIR}" STREQUAL "")
  set(curl_impersonate_override "$ENV{AIS2API_CURL_IMPERSONATE_DIR}")
endif()

if(curl_impersonate_override)
  get_filename_component(curl_impersonate_dir "${curl_impersonate_override}" ABSOLUTE)
else()
  string(LENGTH "${curl_impersonate_sha256}" curl_impersonate_sha256_length)
  if(NOT curl_impersonate_sha256_length EQUAL 64 OR NOT curl_impersonate_sha256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "No release SHA-256 is pinned yet for ${curl_impersonate_archive}; set AIS2API_CURL_IMPERSONATE_DIR to an unpacked ${curl_impersonate_platform} static curl-impersonate build")
  endif()
  set(curl_impersonate_dir "${CMAKE_BINARY_DIR}/_deps/${AIS2API_CURL_IMPERSONATE_TAG}-${curl_impersonate_platform}")
  set(curl_impersonate_marker "${curl_impersonate_dir}/.ais2api-unpacked")
  set(unpacked "")
  if(EXISTS "${curl_impersonate_marker}")
    file(READ "${curl_impersonate_marker}" unpacked)
    string(STRIP "${unpacked}" unpacked)
  endif()
  if(NOT unpacked STREQUAL curl_impersonate_sha256)
    set(archive "${CMAKE_BINARY_DIR}/_deps/${curl_impersonate_archive}")
    if(AIS2API_CURL_IMPERSONATE_URL)
      set(url "${AIS2API_CURL_IMPERSONATE_URL}")
    else()
      set(url "https://github.com/${AIS2API_CURL_IMPERSONATE_RELEASE_REPO}/releases/download/${AIS2API_CURL_IMPERSONATE_TAG}/${curl_impersonate_archive}")
    endif()
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${curl_impersonate_sha256} TLS_VERIFY ON STATUS status)
    list(GET status 0 status_code)
    if(NOT status_code EQUAL 0)
      list(GET status 1 status_text)
      file(REMOVE "${archive}")
      message(FATAL_ERROR "Cannot download ${url}: ${status_text}. Set AIS2API_CURL_IMPERSONATE_DIR to an unpacked copy instead")
    endif()
    file(REMOVE_RECURSE "${curl_impersonate_dir}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${curl_impersonate_dir}")
    file(REMOVE "${archive}")
    file(WRITE "${curl_impersonate_marker}" "${curl_impersonate_sha256}")
  endif()
endif()

if(NOT EXISTS "${curl_impersonate_dir}/BUILDINFO.json")
  file(GLOB curl_impersonate_nested "${curl_impersonate_dir}/*/BUILDINFO.json")
  list(LENGTH curl_impersonate_nested nested_count)
  if(nested_count EQUAL 1)
    list(GET curl_impersonate_nested 0 nested_buildinfo)
    get_filename_component(curl_impersonate_dir "${nested_buildinfo}" DIRECTORY)
  else()
    message(FATAL_ERROR "${curl_impersonate_dir} holds no BUILDINFO.json (expected a static curl-impersonate archive)")
  endif()
endif()

set(AIS2API_RESOLVED_CURL_IMPERSONATE_DIR "${curl_impersonate_dir}" CACHE INTERNAL "Resolved curl-impersonate directory")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${curl_impersonate_dir}/BUILDINFO.json")
if(NOT EXISTS "${curl_impersonate_dir}/include/curl/curl.h")
  message(FATAL_ERROR "${curl_impersonate_dir} holds no include/curl/curl.h")
endif()
if(EXISTS "${curl_impersonate_dir}/include/openssl")
  message(FATAL_ERROR "${curl_impersonate_dir}/include/openssl would shadow OpenSSL 3; omit BoringSSL headers from include/")
endif()
file(GLOB curl_impersonate_shared_libs
  "${curl_impersonate_dir}/lib/*.dll"
  "${curl_impersonate_dir}/lib/*.so"
  "${curl_impersonate_dir}/lib/*.so.*"
  "${curl_impersonate_dir}/lib/*.dylib")
if(curl_impersonate_shared_libs)
  message(FATAL_ERROR "${curl_impersonate_dir}/lib contains dynamic libraries (${curl_impersonate_shared_libs}); only static archives are allowed")
endif()

file(READ "${curl_impersonate_dir}/BUILDINFO.json" curl_impersonate_buildinfo)
string(JSON curl_impersonate_built_version GET "${curl_impersonate_buildinfo}" version)
string(JSON curl_impersonate_target GET "${curl_impersonate_buildinfo}" target)
if(NOT curl_impersonate_target STREQUAL curl_impersonate_platform)
  message(FATAL_ERROR "${curl_impersonate_dir} targets ${curl_impersonate_target}, expected ${curl_impersonate_platform}")
endif()
string(JSON curl_impersonate_bssl_prefix GET "${curl_impersonate_buildinfo}" boringssl_prefix)
if(NOT curl_impersonate_bssl_prefix STREQUAL "bssl_")
  message(FATAL_ERROR "${curl_impersonate_dir} has boringssl_prefix='${curl_impersonate_bssl_prefix}', expected 'bssl_'")
endif()

string(JSON curl_impersonate_library_count LENGTH "${curl_impersonate_buildinfo}" link)
if(curl_impersonate_library_count EQUAL 0)
  message(FATAL_ERROR "${curl_impersonate_dir}/BUILDINFO.json lists no libraries to link")
endif()
math(EXPR curl_impersonate_library_last "${curl_impersonate_library_count} - 1")
set(curl_impersonate_archive_targets "")
set(curl_impersonate_system_libraries "")
foreach(index RANGE ${curl_impersonate_library_last})
  string(JSON name GET "${curl_impersonate_buildinfo}" link ${index})
  if(name MATCHES "\\.(dll|so|dylib)(\\.[0-9]+)*$" OR name MATCHES "_imp\\.lib$")
    message(FATAL_ERROR "${curl_impersonate_dir}/BUILDINFO.json lists dynamic/import library ${name}; only static archives are supported")
  endif()
  if(name MATCHES "^lib/")
    set(full_path "${curl_impersonate_dir}/${name}")
    if(NOT EXISTS "${full_path}")
      message(FATAL_ERROR "${curl_impersonate_dir} is missing static archive ${name}")
    endif()
    set(imported_target "ais2api_curl_impersonate_lib_${index}")
    add_library(${imported_target} STATIC IMPORTED GLOBAL)
    set_target_properties(${imported_target} PROPERTIES IMPORTED_LOCATION "${full_path}")
    list(APPEND curl_impersonate_archive_targets "${imported_target}")
  elseif(name MATCHES "^(z|zlib|zlib\\.lib|zstd|libzstd|zstd\\.lib|zstd_static\\.lib|pthread)$")
    # Linked portably via ZLIB::ZLIB, AIS2API_ZSTD_TARGET and Threads::Threads below.
  else()
    list(APPEND curl_impersonate_system_libraries "${name}")
  endif()
endforeach()

set(AIS2API_CURL_IMPERSONATE_LICENSES "")
file(GLOB curl_impersonate_license_candidates "${curl_impersonate_dir}/licenses/*")
if(NOT curl_impersonate_license_candidates)
  file(GLOB curl_impersonate_license_candidates "${curl_impersonate_dir}/LICENSE*")
endif()
foreach(candidate IN LISTS curl_impersonate_license_candidates)
  if(NOT IS_DIRECTORY "${candidate}")
    list(APPEND AIS2API_CURL_IMPERSONATE_LICENSES "${candidate}")
  endif()
endforeach()
if(NOT AIS2API_CURL_IMPERSONATE_LICENSES)
  message(FATAL_ERROR "${curl_impersonate_dir} holds no license files under licenses/ or LICENSE*")
endif()

# Prepend include/ at directory scope as well so Termux's $PREFIX/include/curl
# (pulled in by Boost/OpenSSL include paths) cannot shadow curl-impersonate.
include_directories(BEFORE "${curl_impersonate_dir}/include")
add_library(ais2api_curl_impersonate INTERFACE IMPORTED GLOBAL)
target_include_directories(ais2api_curl_impersonate INTERFACE "${curl_impersonate_dir}/include")
target_compile_definitions(ais2api_curl_impersonate INTERFACE AIS2API_CURL_IMPERSONATE CURL_STATICLIB)
if(UNIX AND NOT APPLE AND curl_impersonate_archive_targets)
  string(JOIN "," curl_impersonate_rescan_group ${curl_impersonate_archive_targets})
  target_link_libraries(ais2api_curl_impersonate INTERFACE
    "$<LINK_GROUP:RESCAN,${curl_impersonate_rescan_group}>"
    ${AIS2API_ZSTD_TARGET}
    ZLIB::ZLIB
    Threads::Threads
    ${curl_impersonate_system_libraries})
else()
  target_link_libraries(ais2api_curl_impersonate INTERFACE
    ${curl_impersonate_archive_targets}
    ${AIS2API_ZSTD_TARGET}
    ZLIB::ZLIB
    Threads::Threads
    ${curl_impersonate_system_libraries})
endif()
if(WIN32)
  target_link_libraries(ais2api_curl_impersonate INTERFACE
    ws2_32 crypt32 advapi32 bcrypt normaliz iphlpapi secur32)
endif()
set(AIS2API_CURL_TARGET ais2api_curl_impersonate)

if(BUILD_TESTING)
  # Expose nghttp2 for tests/test_firefox_fingerprint.cpp HPACK inflation,
  # preferring the headers and static library already carried in the archive so
  # MSVC never sees duplicate nghttp2 symbols alongside ais2api_core.
  add_library(ais2api_nghttp2 INTERFACE IMPORTED GLOBAL)
  if(EXISTS "${curl_impersonate_dir}/include/nghttp2/nghttp2.h")
    target_include_directories(ais2api_nghttp2 INTERFACE "${curl_impersonate_dir}/include")
    target_compile_definitions(ais2api_nghttp2 INTERFACE NGHTTP2_STATICLIB)
    target_link_libraries(ais2api_nghttp2 INTERFACE ais2api_curl_impersonate)
  else()
    find_path(AIS2API_NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h REQUIRED)
    target_include_directories(ais2api_nghttp2 INTERFACE "${AIS2API_NGHTTP2_INCLUDE_DIR}")
    if(EXISTS "${curl_impersonate_dir}/lib/nghttp2.lib" OR EXISTS "${curl_impersonate_dir}/lib/libnghttp2.a")
      target_compile_definitions(ais2api_nghttp2 INTERFACE NGHTTP2_STATICLIB)
      target_link_libraries(ais2api_nghttp2 INTERFACE ais2api_curl_impersonate)
    else()
      find_library(AIS2API_NGHTTP2_LIBRARY NAMES nghttp2 REQUIRED)
      target_link_libraries(ais2api_nghttp2 INTERFACE "${AIS2API_NGHTTP2_LIBRARY}")
      if(AIS2API_NGHTTP2_LIBRARY MATCHES "\\.(lib|a)$")
        target_compile_definitions(ais2api_nghttp2 INTERFACE NGHTTP2_STATICLIB)
      endif()
    endif()
  endif()
  set(AIS2API_NGHTTP2_TARGET ais2api_nghttp2)
endif()
message(STATUS "curl-impersonate ${curl_impersonate_built_version} (${curl_impersonate_platform}) from ${curl_impersonate_dir}")
