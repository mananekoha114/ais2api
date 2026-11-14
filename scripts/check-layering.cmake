# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# ais2api_common, ais2api_bidi and ais2api_waa are libraries the service is
# built on. Their sources may include ais2api/common.hpp, their own headers
# and third-party ones, and nothing else of this project.
file(GLOB_RECURSE waa_files "${SOURCE_DIR}/waa/src/*.cpp" "${SOURCE_DIR}/waa/src/*.hpp" "${SOURCE_DIR}/waa/include/*.hpp")
set(files ${waa_files} "${SOURCE_DIR}/src/common.cpp" "${SOURCE_DIR}/include/ais2api/common.hpp"
  "${SOURCE_DIR}/browser/bidi.hpp" "${SOURCE_DIR}/browser/json_text.hpp")
list(LENGTH waa_files waa_count)
if(waa_count EQUAL 0)
  message(FATAL_ERROR "No WAA sources under ${SOURCE_DIR}/waa")
endif()
set(violations "")
foreach(file IN LISTS files)
  file(STRINGS "${file}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
  foreach(line IN LISTS includes)
    string(REGEX REPLACE "^[^\"]*\"([^\"]*)\".*$" "\\1" header "${line}")
    if(header MATCHES "^ais2api/" AND NOT header STREQUAL "ais2api/common.hpp" AND NOT header MATCHES "^ais2api/waa/")
      list(APPEND violations "${file}: ${header}")
    elseif(header MATCHES "\\.\\./")
      list(APPEND violations "${file}: ${header}")
    endif()
  endforeach()
endforeach()
if(violations)
  list(JOIN violations "\n  " text)
  message(FATAL_ERROR "These includes reach outside the library layer:\n  ${text}")
endif()
