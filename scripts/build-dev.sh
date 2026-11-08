#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Builds a Linux x64 dev bundle for local use: the fpgen model embedded and the
# pinned Camoufox carried beside the executable. Release archives come from
# build.sh.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
usage() {
  cat <<'EOF'
Usage: sh scripts/build-dev.sh [options]
  --build-dir PATH   default build/linux-dev
  --jobs N           default 2
  --proxy URL        proxy for the downloads and setup
  --yes              let setup install the system packages Camoufox needs (sudo apt-get)
  --skip-tests       omit test targets
  --skip-webui       do not build and embed the WebUI
Downloads the pinned fpgen model and, through the built setup, the pinned
Camoufox (about 1.3 GB, kept for later runs), then writes packages/*-Dev.tar.gz.
Requires Git, CMake, Ninja, curl, Python 3 and, unless --skip-webui, Node.js.
Environment: VCPKG_ROOT (existing pinned checkout), BUILD_TYPE.
EOF
}
build_dir=; jobs=2; proxy=; approve=; testing=ON; webui=ON
while [ "$#" -gt 0 ]; do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --yes) approve=--yes; shift ;;
    --skip-tests) testing=OFF; shift ;;
    --skip-webui) webui=OFF; shift ;;
    --build-dir|--jobs|--proxy)
      [ "$#" -ge 2 ] || { echo "Missing argument: $1" >&2; exit 2; }
      case "$1" in --build-dir) build_dir=$2 ;; --jobs) jobs=$2 ;; --proxy) proxy=$2 ;; esac
      shift 2 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done
case "$jobs" in ''|*[!0-9]*|0) echo '--jobs must be positive.' >&2; exit 2 ;; esac
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] || { echo 'Dev bundles for Linux target x86_64.' >&2; exit 1; }
for tool in cmake ninja git curl python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing build tool: $tool" >&2; exit 1; }
done
cd "$project"
[ -n "$build_dir" ] || build_dir="$project/build/linux-dev"
case "$build_dir" in /*) ;; *) build_dir="$project/$build_dir" ;; esac
if [ -f "$build_dir/CMakeCache.txt" ] && ! grep -qx 'AIS2API_DEV_BUNDLE:BOOL=ON' "$build_dir/CMakeCache.txt"; then
  echo "$build_dir holds a release build; use another --build-dir." >&2
  exit 1
fi
build_type=${BUILD_TYPE:-Release}
platform=linux-x64
manifest="$project/runtime-manifest.json"
value() { python3 -I -c 'import json, sys
value = json.load(open(sys.argv[1], encoding="utf-8"))
for key in sys.argv[2:]:
    value = value[key]
print(value)' "$manifest" "$@"; }
version=$(value camoufox version) || { echo "runtime-manifest.json pins no Camoufox for $platform." >&2; exit 1; }
value camoufox artifacts "$platform" sha256 >/dev/null || { echo "runtime-manifest.json pins no Camoufox for $platform." >&2; exit 1; }
bundle="$build_dir/dev-bundle"
data="$bundle/data"
camoufox="$data/runtime/camoufox/$version-$platform"
mkdir -p "$bundle"

# The fpgen model the build embeds, checked file by file.
model_dir="$bundle/fpgen/$(value fpgen_model tag | tr / -)"
check_model() { python3 -I -c 'import hashlib, json, pathlib, sys
model = json.load(open(sys.argv[1], encoding="utf-8"))["fpgen_model"]
directory = pathlib.Path(sys.argv[2])
sys.exit(0 if all((directory / name).is_file() and hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest
                  for name, digest in model["file_sha256"].items()) else 1)' "$manifest" "$model_dir"; }
if ! check_model; then
  archive="$bundle/fpgen-model.zip"
  set -- --fail --location --silent --show-error --retry 2 --connect-timeout 20 --output "$archive"
  [ -z "$proxy" ] || set -- "$@" --proxy "$proxy"
  curl "$@" "$(value fpgen_model url)"
  python3 -I -c 'import hashlib, json, pathlib, shutil, sys, zipfile
model = json.load(open(sys.argv[1], encoding="utf-8"))["fpgen_model"]
archive, directory = pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
data = archive.read_bytes()
if len(data) != model["size"] or hashlib.sha256(data).hexdigest() != model["sha256"]:
    sys.exit("The downloaded fpgen model does not match runtime-manifest.json.")
shutil.rmtree(directory, ignore_errors=True)
directory.mkdir(parents=True)
with zipfile.ZipFile(archive) as source:
    for name in model["file_sha256"]:
        (directory / name).write_bytes(source.read(name))
archive.unlink()' "$manifest" "$archive" "$model_dir"
  check_model || { echo 'The fpgen model archive does not hold the pinned files.' >&2; exit 1; }
fi

# The pinned vcpkg checkout, as build.sh prepares it.
baseline=$(cmake "-DAIS2API_MANIFEST=$project/vcpkg.json" -P "$project/scripts/read-baseline.cmake")
vcpkg_root=${VCPKG_ROOT:-"$project/build/deps/vcpkg"}
if [ ! -e "$vcpkg_root" ]; then
  mkdir -p "$(dirname -- "$vcpkg_root")"
  git clone --filter=blob:none --no-checkout https://github.com/microsoft/vcpkg.git "$vcpkg_root"
  git -C "$vcpkg_root" checkout --detach "$baseline"
fi
[ "$(git -C "$vcpkg_root" rev-parse HEAD)" = "$baseline" ] || { echo 'VCPKG_ROOT must match the manifest baseline; existing checkout was left unchanged.' >&2; exit 1; }
[ -f "$vcpkg_root/vcpkg" ] || "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
cmake -S "$project" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_TESTING="$testing" \
  -DCMAKE_TOOLCHAIN_FILE="$vcpkg_root/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DCMAKE_INSTALL_PREFIX="$build_dir/install" -DAIS2API_DEV_BUNDLE=ON -DAIS2API_FPGEN_MODEL_DIR="$model_dir" \
  -DAIS2API_CAMOUFOX_BUNDLE_DIR="$camoufox" -DAIS2API_BUILD_WEBUI="$webui"
cmake --build "$build_dir" --parallel "$jobs"

# Runs the program with only the given ais2api variables set.
program() { env -u AIS2API_DATA_DIR -u AIS2API_BROWSER_BACKEND -u AIS2API_BROWSER_EXECUTABLE -u AIS2API_PROXY "$@"; }
# setup installs the pinned Camoufox, trimmed as every install is, and
# verifies it on a local page. A finished install is only verified again.
set -- AIS2API_DATA_DIR="$data" AIS2API_BROWSER_BACKEND=camoufox
[ -z "$proxy" ] || set -- "$@" AIS2API_PROXY="$proxy"
program "$@" "$build_dir/ais2api" setup --config "$bundle/config.json" $approve >/dev/null

if [ "$testing" = ON ]; then ctest --test-dir "$build_dir" --output-on-failure; fi
rm -rf "$build_dir/install"
cmake --install "$build_dir"

# A fresh configuration of the installed bundle needs no setup. doctor
# writes nothing there.
smoke="$bundle/smoke"
report=$(program AIS2API_DATA_DIR="$smoke/data" "$build_dir/install/bin/ais2api" doctor --config "$smoke/config.json")
printf '%s' "$report" | python3 -I -c 'import json, sys
doctor = json.load(sys.stdin)
ready = (doctor.get("build_mode") == "dev" and doctor.get("browser_backend") == "camoufox"
         and doctor.get("camoufox", {}).get("source") == "bundle"
         and doctor.get("fpgen_model", {}).get("source") == "embedded" and doctor.get("browser_exists") is True)
sys.exit(0 if ready else "The installed dev bundle is not ready to run: " + json.dumps(doctor))'

cmake --build "$build_dir" --target package --parallel "$jobs"
package=$(ls -t "$build_dir"/packages/*-Dev.tar.gz 2>/dev/null | head -n 1)
[ -n "$package" ] || { echo 'CPack wrote no dev package.' >&2; exit 1; }
python3 "$project/scripts/check-package.py" --dev "$package"
echo "Dev bundle: $package"
