# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# Embed bytes rather than a string literal to avoid compiler string-size limits.
# NAME prefixes the generated identifiers (default: bridge).
if(NOT DEFINED NAME)
  set(NAME bridge)
endif()
file(READ "${SOURCE}" bridge_hex HEX)
string(LENGTH "${bridge_hex}" bridge_hex_length)
if(bridge_hex_length EQUAL 0 OR bridge_hex_length GREATER 524288)
  message(FATAL_ERROR "Browser script must contain 1..262144 bytes")
endif()
file(SHA256 "${SOURCE}" bridge_sha256)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bridge_bytes "${bridge_hex}")
get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT}" "// Generated; do not edit.\nstatic constexpr unsigned char embedded_${NAME}_bytes[] = {${bridge_bytes}};\nstatic constexpr std::string_view embedded_${NAME}_digest = \"${bridge_sha256}\";\n")
