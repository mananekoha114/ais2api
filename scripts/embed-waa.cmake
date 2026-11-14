# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# Embeds the WAA host files as bytes: dom.js, smhost.js and the gzip-compressed
# Firefox shape table, each with its SHA-256 for diagnostics.
set(generated "// Generated; do not edit.\n")
foreach(entry "dom=${DOM}" "host=${HOST}" "shape=${SHAPE}")
  string(REGEX MATCH "^([a-z]+)=(.*)$" matched "${entry}")
  set(name "${CMAKE_MATCH_1}")
  set(source "${CMAKE_MATCH_2}")
  file(READ "${source}" hex HEX)
  string(LENGTH "${hex}" hex_length)
  if(hex_length EQUAL 0 OR hex_length GREATER 1048576)
    message(FATAL_ERROR "${source} must contain 1..524288 bytes")
  endif()
  file(SHA256 "${source}" sha256)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(APPEND generated
    "static constexpr unsigned char embedded_waa_${name}[] = {${bytes}};\n"
    "static constexpr std::string_view embedded_waa_${name}_sha256 = \"${sha256}\";\n")
endforeach()
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}" "${generated}")
