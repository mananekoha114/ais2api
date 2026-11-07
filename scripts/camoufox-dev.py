#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Development helpers for the Camoufox prototype. Not installed or packaged.

  python scripts/camoufox-dev.py venv
      Create .tools/camoufox-venv with the pinned camoufox Python package.
  python scripts/camoufox-dev.py fetch
      Download the pinned Windows x64 Camoufox build into .tools/camoufox/<tag>/,
      verify it and print the browser_executable path.
  .tools/camoufox-venv/Scripts/python.exe -I scripts/camoufox-dev.py identities \\
      --executable <camoufox.exe> --out <data_dir>/camoufox/identity-pool.json
      Generate an identity pool with the camoufox package's own generator.

An identity pool describes this machine (its GPU, among other things): keep it
in the data directory and never commit it.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import threading
import urllib.request
import uuid
import zipfile

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / '.tools'
VENV = TOOLS / 'camoufox-venv'
CAMOUFOX_PYTHON = '0.5.9'  # pins TAG in its browser-pin.json
TAG = 'v158.0.1-beta.41'
ASSET = 'camoufox-158.0.1-beta.41-win.x86_64.zip'
ASSET_SHA256 = '2644bd14f1cc345c768f6a70b576bcc87eb3d0bf0df54856d6fe0bbc3027515f'
ASSET_SIZE = 1320075455
URL = f'https://github.com/daijro/camoufox/releases/download/{TAG}/{ASSET}'
RELEASE = 'release.json'
INT32 = (-2**31, 2**31 - 1)


def fail(message):
    print('error: ' + message, file=sys.stderr)
    sys.exit(1)


def venv(_args):
    python = VENV / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
    if not python.exists():
        subprocess.run([sys.executable, '-m', 'venv', str(VENV)], check=True)
    subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check', f'camoufox=={CAMOUFOX_PYTHON}'], check=True)
    print(python)


def safe_member(name):
    path = PurePosixPath(name)
    if '\\' in name or ':' in name or '\x00' in name or path.is_absolute() or '..' in path.parts:
        fail('unsafe path in archive: ' + name)
    return path


def download(target, segments=8):
    # Parallel ranges, each resumed after an early end: a single long download
    # from GitHub's storage can crawl or end without an error.
    partial = target.with_name(target.name + '.part')
    with open(partial, 'wb') as output:
        output.truncate(ASSET_SIZE)
    bounds = [(i * ASSET_SIZE // segments, (i + 1) * ASSET_SIZE // segments) for i in range(segments)]
    received = [0] * segments
    lock = threading.Lock()
    shown = [0]

    def segment(index):
        start, end = bounds[index]
        with open(partial, 'r+b') as output:
            for _ in range(20):
                position = start + received[index]
                if position >= end:
                    return
                request = urllib.request.Request(URL, headers={'Range': f'bytes={position}-{end - 1}'})
                try:
                    with urllib.request.urlopen(request, timeout=60) as response:
                        if response.status != 206:
                            raise OSError(f'HTTP {response.status} instead of a range')
                        output.seek(position)
                        while block := response.read(1024 * 1024):
                            block = block[:end - start - received[index]]
                            output.write(block)
                            with lock:
                                received[index] += len(block)
                                step = sum(received) * 20 // ASSET_SIZE
                                if step > shown[0]:
                                    shown[0] = step
                                    print(f'{step * 5}% of {ASSET_SIZE // (1024 * 1024)} MiB', file=sys.stderr)
                except OSError as error:
                    print(f'segment {index} interrupted at {start + received[index]}: {error}', file=sys.stderr)

    with ThreadPoolExecutor(segments) as pool:
        list(pool.map(segment, range(segments)))
    if sum(received) != ASSET_SIZE or not verify(partial):
        partial.unlink()
        fail(f'{ASSET} did not download completely or does not match the pinned SHA-256 {ASSET_SHA256}')
    partial.replace(target)


def verify(archive):
    digest = hashlib.sha256()
    with open(archive, 'rb') as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest() == ASSET_SHA256


def fetch(args):
    root = Path(args.dir).resolve() / TAG
    browser = root / 'browser'
    marker = root / RELEASE
    if browser.is_dir() and marker.exists():
        release = json.loads(marker.read_text(encoding='utf-8'))
        if release.get('sha256') == ASSET_SHA256:
            print(browser / release['executable'])
            return
    root.mkdir(parents=True, exist_ok=True)
    archive = Path(args.archive).resolve() if args.archive else root / ASSET
    if not archive.exists():
        download(archive)
    elif not verify(archive):
        fail(f'{archive} does not match the pinned SHA-256 {ASSET_SHA256}')
    # Extract into a fresh directory and move it into place once complete.
    staging = root / f'browser.{uuid.uuid4().hex[:8]}'
    staging.mkdir()
    try:
        with zipfile.ZipFile(archive) as source:
            for member in source.infolist():
                safe_member(member.filename)
            source.extractall(staging)
        executables = sorted(staging.rglob('camoufox.exe'), key=lambda path: len(path.parts))
        if not executables:
            fail('camoufox.exe not found in ' + ASSET)
        if browser.exists():
            shutil.rmtree(browser)
        staging.replace(browser)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    executable = executables[0].relative_to(staging)
    marker.write_text(json.dumps({'tag': TAG, 'asset': ASSET, 'sha256': ASSET_SHA256, 'executable': executable.as_posix()}, indent=2) + '\n', encoding='utf-8')
    if not args.archive and not args.keep_archive:
        archive.unlink()
    print(browser / executable)


def release_tag(executable):
    for parent in executable.parents:
        if (parent / RELEASE).exists():
            return json.loads((parent / RELEASE).read_text(encoding='utf-8')).get('tag')
    return None


def check_prefs(prefs):
    # ais2api writes these to user.js, which holds strings, booleans and
    # 32-bit integers only.
    for name, value in prefs.items():
        if isinstance(value, bool):
            continue
        if isinstance(value, int) and INT32[0] <= value <= INT32[1]:
            continue
        if isinstance(value, str) and not any(ord(c) < 32 for c in value):
            continue
        fail(f'preference {name} has a value user.js cannot hold: {value!r}')


def tracked(path):
    # Refuse to write a pool where git could pick it up.
    try:
        path.relative_to(REPO)
    except ValueError:
        return False
    return subprocess.run(['git', '-C', str(REPO), 'check-ignore', '-q', str(path)]).returncode != 0


def identities(args):
    try:
        from importlib.metadata import version
        from camoufox.addons import DefaultAddons
        from camoufox.utils import launch_options, resolve_verstr
    except ImportError:
        fail(f'run this with the camoufox package installed: {VENV} (scripts/camoufox-dev.py venv)')
    if version('camoufox') != CAMOUFOX_PYTHON:
        fail(f'camoufox {version("camoufox")} is installed; this script is pinned to {CAMOUFOX_PYTHON}')
    executable = Path(args.executable).resolve()
    if executable.stem.lower() != 'camoufox' or not executable.is_file():
        fail(f'{executable} is not a camoufox executable')
    out = Path(args.out).resolve()
    if tracked(out):
        fail(f'{out} is inside the repository and not ignored by git')
    if args.count < 1:
        fail('--count must be at least 1')
    pool = []
    for index in range(args.count):
        # uBlock Origin is excluded the way the official launcher does it: the
        # addon list travels in the config's `addons` key.
        options = launch_options(executable_path=str(executable), os='windows', headless=True,
                                 exclude_addons=[DefaultAddons.UBO])
        env = options['env']
        pieces = sorted((int(name.rsplit('_', 1)[1]), value) for name, value in env.items() if name.startswith('CAMOU_CONFIG_'))
        config = json.loads(''.join(value for _, value in pieces))
        config.pop('addons', None)
        prefs = dict(options['firefox_user_prefs'])
        check_prefs(prefs)
        pool.append({'config': config, 'prefs': prefs})
        print(f'identity {index + 1}/{args.count}: {config.get("navigator.userAgent", "?")}', file=sys.stderr)
    document = {
        'schema': 1,
        'camoufox': {'tag': release_tag(executable), 'firefox': resolve_verstr(executable)},
        'generator': {'camoufox_python': CAMOUFOX_PYTHON},
        'created_at': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'identities': pool,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    partial = out.with_name(out.name + '.part')
    partial.write_text(json.dumps(document, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    partial.replace(out)
    print(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('venv', help='create the camoufox Python environment').set_defaults(run=venv)
    command = commands.add_parser('fetch', help='download and unpack the pinned Camoufox build')
    command.add_argument('--dir', default=str(TOOLS / 'camoufox'))
    command.add_argument('--archive', help='use an already downloaded archive')
    command.add_argument('--keep-archive', action='store_true')
    command.set_defaults(run=fetch)
    command = commands.add_parser('identities', help='generate an identity pool')
    command.add_argument('--executable', required=True)
    command.add_argument('--count', type=int, default=16)
    command.add_argument('--out', required=True)
    command.set_defaults(run=identities)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
