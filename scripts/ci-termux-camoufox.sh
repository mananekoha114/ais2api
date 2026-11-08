#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Runs only in the disposable Termux CI container: builds ais2api, lets setup
# create the proot container and install Camoufox in it, and runs the hidden
# Camoufox runtime tests through proot. Only local pages are opened: no
# account, cookie or Google request.
set -eu
[ "${AIS2API_TERMUX_CI:-}" = 1 ] && [ -f /input/source.tar ] && [ -d /out ] || {
  echo 'Run this entry point inside the isolated Termux CI container.' >&2
  exit 2
}
[ "${PREFIX:-}" = /data/data/com.termux/files/usr ] && [ "$(uname -m)" = aarch64 ] || {
  echo 'Expected the standard ARM64 Termux prefix.' >&2
  exit 2
}
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project"
export DEBIAN_FRONTEND=noninteractive
# The Termux app preloads termux-exec; the container does not.
export LD_PRELOAD="$PREFIX/lib/libtermux-exec-ld-preload.so"
test -f "$LD_PRELOAD"
apt-get update
apt-get upgrade -y -o Dpkg::Options::=--force-confnew
pkg install -y clang cmake ninja git curl pkg-config boost boost-headers libcurl openssl libarchive libsqlite zstd python tar
# libc++ supplies syncstream declarations through its experimental feature gate.
CC=clang CXX=clang++ CXXFLAGS=-fexperimental-library sh scripts/build.sh termux --jobs "$(nproc)"

data="$HOME/camoufox-data"
config="$HOME/camoufox-config.json"
run() { AIS2API_DATA_DIR="$data" AIS2API_BROWSER_BACKEND=camoufox build/termux/ais2api "$@" --config "$config"; }
keep_logs() {
  mkdir -p /out/logs
  cp -R "$data/logs/." /out/logs/ 2>/dev/null || true
}
trap keep_logs EXIT
# Without the container, setup asks before it creates anything.
if run setup > /out/refused.json 2>&1; then
  echo 'setup created the container without --yes' >&2
  exit 1
fi
grep -q system_confirmation_required /out/refused.json
run setup --yes > /out/setup.json
run doctor > /out/doctor.json
python - /out/setup.json /out/doctor.json > "$HOME/camoufox-paths" <<'EOF'
import json, sys
setup, doctor = (json.load(open(path, encoding='utf-8')) for path in sys.argv[1:3])
camoufox = setup['camoufox']
print(json.dumps({'system_packages': setup.get('system_packages'), 'version': camoufox['version'],
                  'page': camoufox['browser_verification']['page']}, indent=1), file=sys.stderr)
assert setup['system_packages']['status'] == 'installed', setup['system_packages']
assert doctor['browser_exists'] is True and doctor['camoufox']['source'] == 'setup', doctor
print(doctor['camoufox']['executable'])
print(doctor['fpgen_model']['directory'])
EOF
# Ready now: a second setup asks for nothing and installs nothing.
run setup > /out/again.json
grep -q '"system_packages":{"status":"ready"' /out/again.json
grep -q '"installed_now":false' /out/again.json
AIS2API_TEST_CAMOUFOX=$(sed -n 1p "$HOME/camoufox-paths") AIS2API_TEST_FPGEN_MODEL=$(sed -n 2p "$HOME/camoufox-paths") \
  build/termux/ais2api_tests '[.camoufox-runtime]'
