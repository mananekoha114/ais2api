# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# curl-impersonate (lexiforest): a libcurl whose BoringSSL and nghttp2 can be
# told a browser's TLS ClientHello and HTTP/2 framing. The SpiderMonkey
# backend reaches Google through it (src/firefox_http.cpp sets Firefox's
# values), and with it the whole program does: one libcurl per build.
#
# Only the release's DLL is linked. Its static libraries carry BoringSSL under
# OpenSSL's names, unprefixed, and would clash with the OpenSSL this program
# links statically; the DLL exports curl's functions and BoringSSL's webcrypto
# helpers, nothing named like OpenSSL. It links the static CRT (/MT) and
# depends on system DLLs only.
#
# Sets AIS2API_CURL_TARGET, AIS2API_CURL_IMPERSONATE_DLL and
# AIS2API_CURL_IMPERSONATE_LICENSES.
set(AIS2API_CURL_IMPERSONATE_VERSION 2.3.2)
set(AIS2API_CURL_IMPERSONATE_DIR "" CACHE PATH "An unpacked libcurl-impersonate release; empty downloads the pinned one")

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
  # The Linux and Android releases are shared objects too, but whether they
  # keep BoringSSL's symbols to themselves is not checked yet.
  message(FATAL_ERROR "AIS2API_CURL_IMPERSONATE supports Windows x64 only for now")
endif()
set(curl_impersonate_archive "libcurl-impersonate-v${AIS2API_CURL_IMPERSONATE_VERSION}.x86_64-win32.tar.gz")
set(curl_impersonate_sha256 4c416b5e50716199c4f6541b660fdada9a049cd0bdb72e92bdee4d4da27d39db)

if(AIS2API_CURL_IMPERSONATE_DIR)
  get_filename_component(curl_impersonate_dir "${AIS2API_CURL_IMPERSONATE_DIR}" ABSOLUTE)
else()
  set(curl_impersonate_dir "${CMAKE_BINARY_DIR}/_deps/curl-impersonate-${AIS2API_CURL_IMPERSONATE_VERSION}")
  # The marker holds the digest of the archive the directory was unpacked
  # from, so a changed pin unpacks again.
  set(curl_impersonate_marker "${curl_impersonate_dir}/.ais2api-unpacked")
  set(unpacked "")
  if(EXISTS "${curl_impersonate_marker}")
    file(READ "${curl_impersonate_marker}" unpacked)
  endif()
  if(NOT unpacked STREQUAL curl_impersonate_sha256)
    set(archive "${CMAKE_BINARY_DIR}/_deps/${curl_impersonate_archive}")
    set(url "https://github.com/lexiforest/curl-impersonate/releases/download/v${AIS2API_CURL_IMPERSONATE_VERSION}/${curl_impersonate_archive}")
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

set(AIS2API_CURL_IMPERSONATE_DLL "${curl_impersonate_dir}/lib/libcurl-impersonate.dll")
foreach(required include/curl/curl.h lib/libcurl-impersonate.dll lib/libcurl-impersonate_imp.lib LICENSE)
  if(NOT EXISTS "${curl_impersonate_dir}/${required}")
    message(FATAL_ERROR "${curl_impersonate_dir} is not a libcurl-impersonate release: it has no ${required}")
  endif()
endforeach()
file(GLOB AIS2API_CURL_IMPERSONATE_LICENSES "${curl_impersonate_dir}/LICENSE*")

add_library(ais2api_curl_impersonate SHARED IMPORTED GLOBAL)
set_target_properties(ais2api_curl_impersonate PROPERTIES
  IMPORTED_LOCATION "${AIS2API_CURL_IMPERSONATE_DLL}"
  IMPORTED_IMPLIB "${curl_impersonate_dir}/lib/libcurl-impersonate_imp.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${curl_impersonate_dir}/include"
  INTERFACE_COMPILE_DEFINITIONS AIS2API_CURL_IMPERSONATE)
set(AIS2API_CURL_TARGET ais2api_curl_impersonate)
message(STATUS "curl-impersonate ${AIS2API_CURL_IMPERSONATE_VERSION} from ${curl_impersonate_dir}")

# Puts the DLL beside a target's executable after each build.
function(ais2api_copy_curl_impersonate target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AIS2API_CURL_IMPERSONATE_DLL}" "$<TARGET_FILE_DIR:${target}>"
    VERBATIM)
endfunction()
