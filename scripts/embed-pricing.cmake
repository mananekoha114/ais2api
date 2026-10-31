# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# Embeds the price catalog snapshot shipped with the source as bytes.
file(READ "${SOURCE}" pricing_hex HEX)
string(LENGTH "${pricing_hex}" pricing_hex_length)
if(pricing_hex_length EQUAL 0 OR pricing_hex_length GREATER 2097152)
  message(FATAL_ERROR "Price catalog snapshot must contain 1..1048576 bytes")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," pricing_bytes "${pricing_hex}")
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}" "// Generated; do not edit.\nstatic constexpr unsigned char embedded_pricing_bytes[] = {${pricing_bytes}};\n")
