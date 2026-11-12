#!/bin/sh
# Copyright (c) 2026 ais2api contributors
# SPDX-License-Identifier: MPL-2.0
# Runs only in the disposable Termux CI container; never installs phone packages.
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
# The Termux app preloads termux-exec so shebangs such as #!/usr/bin/env node
# (npm package binaries) resolve inside the prefix; the container does not.
export LD_PRELOAD="$PREFIX/lib/libtermux-exec-ld-preload.so"
test -f "$LD_PRELOAD"
# A fresh container must have a coherent package set from the rolling repository.
apt-get update
apt-get upgrade -y -o Dpkg::Options::=--force-confnew
pkg install -y clang cmake ninja git curl pkg-config boost boost-headers libcurl openssl libarchive libsqlite zstd python tar nodejs npm
python -m unittest discover -s tests -p test_portable_smoke.py
# libc++ supplies syncstream declarations through its experimental feature gate.
CC=clang CXX=clang++ CXXFLAGS=-fexperimental-library sh scripts/build.sh termux --webui --jobs 2
python tests/cli_smoke.py --exe build/termux/install/bin/ais2api
python tests/portable_smoke.py --exe build/termux/install/bin/ais2api --require-webui
# The archive is packed from the install tree, so the launcher runs in a copy.
launcher_smoke=$(mktemp -d)
cp -R build/termux/install "$launcher_smoke/ais2api"
# The stand-in executable is a CloakBrowser one; a new configuration starts on Camoufox.
AIS2API_BROWSER_BACKEND=cloakbrowser sh "$launcher_smoke/ais2api/start.sh" --background --port 18460 --browser "$PREFIX/bin/true" < /dev/null
"$launcher_smoke/ais2api/bin/ais2api" stop
rm -rf "$launcher_smoke"

python - "$SOURCE_COMMIT" "$TERMUX_CI_IMAGE" <<'PY'
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

spec = importlib.util.spec_from_file_location('package_check', 'scripts/check-package.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
commit, image = sys.argv[1:]
if not re.fullmatch(r'[0-9a-f]{40}', commit) or not re.fullmatch(r'termux/termux-docker@sha256:[0-9a-f]{64}', image):
    raise ValueError('invalid_build_provenance')
prefix = Path(checker.TERMUX_PREFIX)
stage = Path('build/termux/install')
packages = {}
listing = subprocess.check_output(['dpkg-query', '-W', '-f', '${Package}\t${Version}\t${Architecture}\t${Status}\n'], text=True)
for line in listing.splitlines():
    name, version, architecture, status = line.split('\t', 3)
    if status == 'install ok installed':
        packages[name] = {'version': version, 'architecture': architecture}

def owner(path):
    result = subprocess.check_output(['dpkg-query', '-S', str(path)], text=True).splitlines()
    if len(result) != 1:
        raise ValueError('ambiguous_dependency_owner')
    name = result[0].split(': ', 1)[0]
    if name not in packages:
        raise ValueError('unknown_dependency_owner')
    return name

executable = checker.elf_runtime((stage / 'bin/ais2api').read_bytes())
libraries = {}
pending = list(executable['needed'])
while pending:
    name = checker.termux_soname(pending.pop())
    if name in libraries:
        continue
    if name in checker.ANDROID_LIBRARIES:
        libraries[name] = {'provider': 'android-system'}
        continue
    library = prefix / 'lib' / name
    package = owner(library)
    needed = checker.elf_runtime(library.read_bytes())['needed']
    libraries[name] = {'provider': 'termux', 'package': package,
                       'needed': [checker.termux_soname(item) for item in needed]}
    pending.extend(needed)

experimental = prefix / 'lib/libc++experimental.a'
static_components = {'libc++experimental.a': {
    'package': owner(experimental),
    'sha256': hashlib.sha256(experimental.read_bytes()).hexdigest(),
}}
# Ship notices only for code inside the executable and for the packages providing
# its runtime libraries; check-package.py requires this same set. Boost's notice
# also covers its headers. Build-only tools in the container are not distributed.
notice_packages = set(checker.TERMUX_EMBEDDED_NOTICES)
notice_packages.add(static_components['libc++experimental.a']['package'])
notice_packages.update(item['package'] for item in libraries.values() if item['provider'] == 'termux')
notices = {}
for package in sorted(notice_packages):
    source = prefix / 'share/doc' / package / 'copyright'
    if source.is_file():
        destination = stage / 'share/licenses/ais2api-dependencies' / package / 'copyright'
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        notices[package] = 'included'
    else:
        notices[package] = 'external-system-package'
metadata = {
    'schema_version': 1, 'target': 'termux-aarch64',
    'source_commit': commit, 'container_image': image,
    'compiler': subprocess.check_output(['clang++', '--version'], text=True).strip(),
    'packages': packages, 'executable': executable,
    'runtime_libraries': libraries, 'static_components': static_components,
    'dependency_notices': notices,
}
destination = stage / 'share/ais2api/termux-build.json'
destination.write_text(json.dumps(metadata, indent=2, sort_keys=True) + '\n', encoding='utf-8')
archive = Path('/out') / f'ais2api-termux-aarch64-{commit}.tar.gz'
if archive.exists() or Path(str(archive) + '.sha256').exists():
    raise ValueError('refusing_to_overwrite_existing_artifact')
with tarfile.open(archive, 'w:gz') as output:
    output.add(stage, arcname='ais2api-termux-aarch64')
with archive.open('rb') as source:
    digest = hashlib.file_digest(source, 'sha256').hexdigest()
Path(str(archive) + '.sha256').write_text(f'{digest}  {archive.name}\n', encoding='ascii')
checker.check(archive, target='termux-aarch64')
print('Termux runtime and dependency metadata staged; no accounts or browser bundled.')
PY
