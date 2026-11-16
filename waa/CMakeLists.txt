# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# The WAA runtime. Every build compiles the parts that need no JavaScript
# engine, so that they are tested on every platform; the virtual machine itself
# is compiled with AIS2API_SPIDERMONKEY (scripts/spidermonkey.cmake).
add_library(ais2api_waa STATIC src/challenge.cpp src/loop.cpp src/pointer.cpp src/text.cpp)
target_include_directories(ais2api_waa PUBLIC include)
target_link_libraries(ais2api_waa PUBLIC ais2api_common)
if(NOT MSVC)
  target_compile_options(ais2api_waa PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NOT AIS2API_SPIDERMONKEY)
  return()
endif()

set(third_party "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aistudio2api")
find_package(ada CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
set(waa_host_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/waa_host.inc")
# The shape tables collected with tools/shape: Windows's on Windows, Linux's
# everywhere else, Android included until it has a table of its own.
if(WIN32)
  set(waa_shape "${CMAKE_CURRENT_SOURCE_DIR}/shapes/firefox158-windows.json.gz")
else()
  set(waa_shape "${CMAKE_CURRENT_SOURCE_DIR}/shapes/firefox158-linux.json.gz")
endif()
add_custom_command(OUTPUT "${waa_host_inc}"
  COMMAND "${CMAKE_COMMAND}" "-DDOM=${third_party}/dom.js" "-DHOST=${third_party}/smhost.js"
    "-DSHAPE=${waa_shape}" "-DOUTPUT=${waa_host_inc}"
    -P "${PROJECT_SOURCE_DIR}/scripts/embed-waa.cmake"
  DEPENDS "${third_party}/dom.js" "${third_party}/smhost.js" "${waa_shape}"
    "${PROJECT_SOURCE_DIR}/scripts/embed-waa.cmake" VERBATIM)
target_sources(ais2api_waa PRIVATE src/js.cpp src/url.cpp src/host.cpp src/runtime.cpp "${waa_host_inc}")
target_include_directories(ais2api_waa PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${third_party}/mozjs")
target_compile_definitions(ais2api_waa PUBLIC AIS2API_HAS_SPIDERMONKEY
  PRIVATE AIS2API_SPIDERMONKEY_VERSION="${AIS2API_SPIDERMONKEY_VERSION}")
target_link_libraries(ais2api_waa PRIVATE ada::ada ZLIB::ZLIB PUBLIC ${AIS2API_SPIDERMONKEY_TARGET})
if(AIS2API_SPIDERMONKEY_PLATFORM STREQUAL "android-arm64")
  target_sources(ais2api_waa PRIVATE src/android_libcxx.cpp)
endif()
