# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# The WAA runtime. Every build compiles the parts that need no JavaScript
# engine, so that they are tested on every platform; the virtual machine itself
# is compiled with AIS2API_SPIDERMONKEY.
add_library(ais2api_waa STATIC src/challenge.cpp src/loop.cpp src/text.cpp)
target_include_directories(ais2api_waa PUBLIC include)
target_link_libraries(ais2api_waa PUBLIC ais2api_common)
if(NOT MSVC)
  target_compile_options(ais2api_waa PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NOT AIS2API_SPIDERMONKEY)
  return()
endif()

if(NOT AIS2API_SPIDERMONKEY_DIR)
  message(FATAL_ERROR "AIS2API_SPIDERMONKEY needs AIS2API_SPIDERMONKEY_DIR: the artifact tools/spidermonkey builds")
endif()
get_filename_component(spidermonkey_dir "${AIS2API_SPIDERMONKEY_DIR}" ABSOLUTE)
set(third_party "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aistudio2api")

# The artifact's shim must be the one whose header this tree compiles against.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${spidermonkey_dir}/BUILDINFO.json")
if(NOT EXISTS "${spidermonkey_dir}/BUILDINFO.json")
  message(FATAL_ERROR "${spidermonkey_dir} holds no BUILDINFO.json")
endif()
file(READ "${spidermonkey_dir}/BUILDINFO.json" spidermonkey_buildinfo)
string(JSON spidermonkey_version GET "${spidermonkey_buildinfo}" spidermonkey)
foreach(name mozjs.cpp mozjs.h)
  string(JSON built GET "${spidermonkey_buildinfo}" shim "${name}")
  file(SHA256 "${third_party}/mozjs/${name}" present)
  if(NOT built STREQUAL present)
    message(FATAL_ERROR "${spidermonkey_dir} was built from another ${name}; rebuild it with tools/spidermonkey")
  endif()
endforeach()
string(JSON spidermonkey_library_count LENGTH "${spidermonkey_buildinfo}" link)
if(spidermonkey_library_count EQUAL 0)
  message(FATAL_ERROR "${spidermonkey_dir}/BUILDINFO.json lists no libraries to link")
endif()
math(EXPR spidermonkey_library_last "${spidermonkey_library_count} - 1")
set(spidermonkey_libraries "")
foreach(index RANGE ${spidermonkey_library_last})
  string(JSON name GET "${spidermonkey_buildinfo}" link ${index})
  if(name MATCHES "^lib/")
    # The artifact's own libraries; the rest are the system's.
    list(APPEND spidermonkey_libraries "${spidermonkey_dir}/${name}")
  else()
    list(APPEND spidermonkey_libraries "${name}")
  endif()
endforeach()

find_package(ada CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
set(waa_host_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/waa_host.inc")
add_custom_command(OUTPUT "${waa_host_inc}"
  COMMAND "${CMAKE_COMMAND}" "-DDOM=${third_party}/dom.js" "-DHOST=${third_party}/smhost.js"
    "-DSHAPE=${third_party}/firefox152.json.gz" "-DOUTPUT=${waa_host_inc}"
    -P "${PROJECT_SOURCE_DIR}/scripts/embed-waa.cmake"
  DEPENDS "${third_party}/dom.js" "${third_party}/smhost.js" "${third_party}/firefox152.json.gz"
    "${PROJECT_SOURCE_DIR}/scripts/embed-waa.cmake" VERBATIM)
target_sources(ais2api_waa PRIVATE src/js.cpp src/url.cpp src/host.cpp src/runtime.cpp "${waa_host_inc}")
target_include_directories(ais2api_waa PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${third_party}/mozjs")
target_compile_definitions(ais2api_waa PUBLIC AIS2API_HAS_SPIDERMONKEY
  PRIVATE AIS2API_SPIDERMONKEY_VERSION="${spidermonkey_version}")
target_link_libraries(ais2api_waa PRIVATE ada::ada ZLIB::ZLIB PUBLIC ${spidermonkey_libraries})
message(STATUS "SpiderMonkey ${spidermonkey_version} from ${spidermonkey_dir}")
