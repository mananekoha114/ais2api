#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Native builds only: no browser setup or account requests.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
usage() {
  cat <<'EOF'
Usage: sh scripts/build.sh [linux|termux] [options]
  --install-deps       install Termux host packages (changes shared packages)
  --package            create desktop CPack TGZ after tests
  --browser-bundle PATH include a verified local browser (desktop only)
  --webui              build and embed the WebUI (requires Node.js 20.19+ or 22.12+)
  --sanitizers         enable desktop Linux ASan/UBSan
  --fuzzer             build Clang libFuzzer target (also enables sanitizers)
  --skip-tests         omit test targets (Termux also skips Catch2 preparation)
  --build-dir PATH     default build/linux or build/termux
  --jobs N             default 2; Termux default 1
Environment: CC, CXX, VCPKG_ROOT (existing pinned checkout), BUILD_TYPE.
New dependencies go under build/deps; existing .tools is untouched.
EOF
}
target=linux
case "${1:-}" in linux|termux) target=$1; shift ;; esac
packages=0; package=0; sanitize=OFF; fuzz=OFF; testing=ON; webui=OFF; jobs=2; build_dir=; browser_bundle=
[ "$target" != termux ] || jobs=1
while [ "$#" -gt 0 ]; do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --install-deps) packages=1; shift ;;
    --package) package=1; shift ;;
    --sanitizers) sanitize=ON; shift ;;
    --fuzzer) fuzz=ON; sanitize=ON; shift ;;
    --skip-tests) testing=OFF; shift ;;
    --webui) webui=ON; shift ;;
    --build-dir|--jobs|--browser-bundle)
      [ "$#" -ge 2 ] || { echo "Missing argument: $1" >&2; exit 2; }
      case "$1" in --build-dir) build_dir=$2 ;; --jobs) jobs=$2 ;; --browser-bundle) browser_bundle=$2 ;; esac
      shift 2 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done
case "$jobs" in ''|*[!0-9]*|0) echo '--jobs must be positive.' >&2; exit 2 ;; esac
cd "$project"
[ -n "$build_dir" ] || build_dir="$project/build/$target"
case "$build_dir" in /*) ;; *) build_dir="$project/$build_dir" ;; esac
# Release archives never come from a dev build (scripts/build-dev.ps1).
if [ -f "$build_dir/CMakeCache.txt" ] && grep -qx 'AIS2API_DEV_BUNDLE:BOOL=ON' "$build_dir/CMakeCache.txt"; then
  echo "$build_dir holds a dev build; use another --build-dir." >&2
  exit 1
fi
build_type=${BUILD_TYPE:-Release}
need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing build tool: $1" >&2; exit 1; }; }
# GitHub occasionally times out (HTTP 408) on large partial-clone fetches.
retry() {
  attempt=1
  until "$@"; do
    [ "$attempt" -lt 3 ] || return 1
    echo "Command failed, retrying ($attempt/3): $*" >&2
    sleep $((attempt * 15))
    attempt=$((attempt + 1))
  done
}
if [ "$target" = termux ]; then
  [ -n "${PREFIX:-}" ] && command -v pkg >/dev/null 2>&1 || { echo 'Run in the Termux host, outside proot.' >&2; exit 1; }
  [ "$(uname -m)" = aarch64 ] || { echo 'Supported Termux target: ARM64 (aarch64).' >&2; exit 1; }
  [ "$sanitize" = OFF ] && [ "$fuzz" = OFF ] && [ "$package" -eq 0 ] && [ -z "$browser_bundle" ] || { echo 'Desktop sanitizer/fuzzer/archive targets are separate from Termux device acceptance.' >&2; exit 2; }
  if [ "$packages" -eq 1 ]; then
    pkg install -y clang cmake ninja git curl pkg-config boost libcurl openssl libarchive libsqlite zstd
  fi
  for tool in cmake ninja clang++ curl tar sha512sum; do need "$tool"; done
  prefix_path=$PREFIX
  if [ "$testing" = ON ]; then
    # Same version and archive checksum as the pinned vcpkg Catch2 port.
    catch_version=3.16.0
    catch_hash=b17a1d03bd206ec2a006bfd2cb6a40cb641dd6c3ee192f4e4830ecc2c7fe60ffb2d98397adbb7c039b065c891ebc5dcd0e02331dff472ced7411f8263e1d7b29
    deps="$project/build/deps"
    archive="$deps/Catch2-$catch_version.tar.gz"
    source="$deps/Catch2-$catch_version"
    mkdir -p "$deps"
    if [ ! -f "$archive" ]; then
      curl --fail --location --retry 2 --connect-timeout 20 --output "$archive.part" "https://github.com/catchorg/Catch2/archive/v$catch_version.tar.gz"
      printf '%s  %s\n' "$catch_hash" "$archive.part" | sha512sum --check --status
      mv -- "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$catch_hash" "$archive" | sha512sum --check --status
    if [ ! -d "$source" ]; then
      tar -xzf "$archive" -C "$deps"
      printf '%s\n' "$catch_hash" > "$source/.ais2api-archive"
    else
      [ -f "$source/.ais2api-archive" ] && [ "$(cat "$source/.ais2api-archive")" = "$catch_hash" ] || { echo "Unrecognized Catch2 directory: $source" >&2; exit 1; }
    fi
    cmake -S "$source" -B "$deps/catch2-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps/catch2-install" -DCATCH_BUILD_TESTING=OFF -DCATCH_INSTALL_DOCS=OFF
    cmake --build "$deps/catch2-build" --parallel "$jobs"
    cmake --install "$deps/catch2-build"
    prefix_path="$deps/catch2-install;$PREFIX"
  fi
  cmake -S "$project" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_TESTING="$testing" -DAIS2API_BUILD_WEBUI="$webui" -DAIS2API_DEV_BUNDLE=OFF -DCMAKE_PREFIX_PATH="$prefix_path" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DCMAKE_INSTALL_PREFIX="$build_dir/install"
else
  [ "$(uname -m)" = x86_64 ] || { echo 'This desktop preset targets Linux x86_64.' >&2; exit 1; }
  [ "$packages" -eq 0 ] || { echo 'Install Linux build prerequisites using your OS package manager; see docs/platforms.md.' >&2; exit 2; }
  for tool in cmake ninja git; do need "$tool"; done
  baseline=$(cmake "-DAIS2API_MANIFEST=$project/vcpkg.json" -P "$project/scripts/read-baseline.cmake")
  vcpkg_root=${VCPKG_ROOT:-"$project/build/deps/vcpkg"}
  if [ ! -e "$vcpkg_root" ]; then
    mkdir -p "$(dirname -- "$vcpkg_root")"
    clone_vcpkg() { rm -rf "$vcpkg_root" && git clone --filter=blob:none --no-checkout https://github.com/microsoft/vcpkg.git "$vcpkg_root"; }
    retry clone_vcpkg
    retry git -C "$vcpkg_root" checkout --detach "$baseline"
  fi
  actual=$(git -C "$vcpkg_root" rev-parse HEAD)
  [ "$actual" = "$baseline" ] || { echo 'VCPKG_ROOT must match the manifest baseline; existing checkout was left unchanged.' >&2; exit 1; }
  [ -f "$vcpkg_root/vcpkg" ] || "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
  cmake -S "$project" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_TESTING="$testing" -DAIS2API_SANITIZE="$sanitize" -DAIS2API_BUILD_FUZZER="$fuzz" -DAIS2API_BUILD_WEBUI="$webui" -DAIS2API_DEV_BUNDLE=OFF -DCMAKE_TOOLCHAIN_FILE="$vcpkg_root/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-linux -DCMAKE_INSTALL_PREFIX="$build_dir/install" -DAIS2API_BROWSER_BUNDLE_DIR="$browser_bundle"
fi
cmake --build "$build_dir" --parallel "$jobs"
if [ "$testing" = ON ]; then ctest --test-dir "$build_dir" --output-on-failure; fi
cmake --install "$build_dir"
if [ "$package" -eq 1 ]; then cmake --build "$build_dir" --target package --parallel "$jobs"; fi
