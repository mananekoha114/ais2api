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
      Download the Windows x64 Camoufox build runtime-manifest.json pins into
      .tools/camoufox/<tag>/, verify it and print its executable. Unlike the
      install setup makes, it keeps the bundled fonts: the hidden
      [.camoufox-fonts] test compares the two.
  .tools/camoufox-venv/Scripts/python.exe -I scripts/camoufox-dev.py catalog
      Copy the identity catalogues ais2api embeds (fonts, voices, media devices,
      launcher constants) from the pinned camoufox package into browser/camoufox/.
  .tools/camoufox-venv/Scripts/python.exe -I scripts/camoufox-dev.py check-identities \\
      --executable <camoufox.exe> --identities <identities.jsonl>
      Check identities ais2api generated (the hidden [.fpgen-model] test writes
      them to AIS2API_TEST_FPGEN_OUTPUT) with the camoufox package's own
      validate_config and coherence rules, and compare their user agents,
      screens, GPUs and cores with as many identities the package generates.

Identities describe this machine (its GPU, among other things): keep them out
of the repository.
"""
from __future__ import annotations
import argparse
import base64
from collections import Counter
import contextlib
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
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
# The Camoufox build setup installs, as runtime-manifest.json pins it.
MANIFEST = json.loads((REPO / 'runtime-manifest.json').read_text(encoding='utf-8'))
TAG = 'v' + MANIFEST['camoufox']['version']
URL = MANIFEST['camoufox']['artifacts']['windows-x64']['url']
ASSET = URL.rsplit('/', 1)[1]
ASSET_SHA256 = MANIFEST['camoufox']['artifacts']['windows-x64']['sha256']
ASSET_SIZE = MANIFEST['camoufox']['artifacts']['windows-x64']['size']
RELEASE = 'release.json'
INT32 = (-2**31, 2**31 - 1)
CATALOG = REPO / 'browser' / 'camoufox'
# The launcher's own data files that the native identity generator reads. Its
# macOS-only voice URI table is left out: macOS identities are not generated.
CATALOG_FILES = ('essential-fonts.json', 'font-bases.json', 'font-groups.json', 'fonts.json',
                 'launcher-constants.json', 'media-devices.json', 'voice-manifests.json')


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


def check_identities(args):
    try:
        from importlib.metadata import version
        from camoufox import coherence
        from camoufox.addons import DefaultAddons
        from camoufox.host_rendering import has_canvas_placeholder
        from camoufox.utils import launch_options, validate_config
    except ImportError:
        fail(f'run this with the camoufox package installed: {VENV} (scripts/camoufox-dev.py venv)')
    if version('camoufox') != CAMOUFOX_PYTHON:
        fail(f'camoufox {version("camoufox")} is installed; this script is pinned to {CAMOUFOX_PYTHON}')
    executable = Path(args.executable).resolve()
    if executable.stem.lower() != 'camoufox' or not executable.is_file():
        fail(f'{executable} is not a camoufox executable')
    native = [json.loads(line) for line in Path(args.identities).read_text(encoding='utf-8').splitlines() if line.strip()]
    if not native:
        fail(f'{args.identities} holds no identities')
    systems = {identity.get('os') for identity in native}
    if len(systems) != 1 or not systems <= {'windows', 'linux'}:
        fail(f'expected identities of one OS, windows or linux: {sorted(map(str, systems))}')
    system = systems.pop()

    problems = []
    for index, identity in enumerate(native):
        config, prefs = identity.get('config', {}), identity.get('prefs', {})
        # validate_config reports a key the build does not read on stdout,
        # and raises for a value of the wrong type.
        report = io.StringIO()
        try:
            with contextlib.redirect_stdout(report):
                validate_config(config, executable)
        except Exception as error:  # noqa: BLE001 - every failure is reported
            problems.append(f'identity {index}: {error}')
        problems += [f'identity {index}: {line}' for line in report.getvalue().splitlines()]
        problems += [f'identity {index}: {v.rule}: {v.detail}' for v in coherence.validate(config, system[:3])]
        for name, value in prefs.items():
            if not (isinstance(value, (bool, str)) or (isinstance(value, int) and INT32[0] <= value <= INT32[1])):
                problems.append(f'identity {index}: preference {name} has a value user.js cannot hold: {value!r}')

    count = args.count or len(native)
    reference = []
    for index in range(count):
        # The launcher's own identity for this machine, uBlock Origin left out
        # the way the launcher does it.
        options = launch_options(executable_path=str(executable), os=system, headless=True,
                                 exclude_addons=[DefaultAddons.UBO], i_know_what_im_doing=True)
        env = options['env']
        pieces = sorted((int(name.rsplit('_', 1)[1]), value) for name, value in env.items() if name.startswith('CAMOU_CONFIG_'))
        config = json.loads(''.join(value for _, value in pieces))
        reference.append({'config': config, 'canvasCovered': has_canvas_placeholder(options['firefox_user_prefs'])})
        if (index + 1) % 50 == 0 or index + 1 == count:
            print(f'reference identity {index + 1}/{count}', file=sys.stderr)

    def screen(identity):
        config = identity['config']
        return f'{config.get("screen.width")}x{config.get("screen.height")}'

    features = {
        'user agent': lambda identity: identity['config'].get('navigator.userAgent'),
        'screen': screen,
        'GPU': lambda identity: identity['config'].get('webGl:renderer'),
        'cores': lambda identity: identity['config'].get('navigator.hardwareConcurrency'),
        'touch points': lambda identity: identity['config'].get('navigator.maxTouchPoints', 0),
        'fonts': lambda identity: len(identity['config'].get('fonts', [])),
        'canvas covered': lambda identity: bool(identity.get('canvasCovered')),
    }
    print(f'{len(native)} ais2api identities, {len(reference)} from camoufox {CAMOUFOX_PYTHON} ({system})')
    for name, feature in features.items():
        ours, theirs = Counter(map(feature, native)), Counter(map(feature, reference))
        # Total variation distance: half the summed difference of the shares.
        distance = sum(abs(ours[value] / len(native) - theirs[value] / len(reference)) for value in ours | theirs) / 2
        print(f'\n{name}: total variation {distance:.3f}   ais2api  camoufox')
        for value, _ in (ours + theirs).most_common(args.top):
            print(f'  {ours[value] / len(native):6.1%} {theirs[value] / len(reference):8.1%}  {value}')
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        fail(f'{len(problems)} problems in the ais2api identities')
    print('\nno validation or coherence problems')


def catalog(_args):
    from importlib.metadata import PackageNotFoundError, distribution
    try:
        package = distribution('camoufox')
    except PackageNotFoundError:
        fail(f'run this with the camoufox package installed: {VENV} (scripts/camoufox-dev.py venv)')
    if package.version != CAMOUFOX_PYTHON:
        fail(f'camoufox {package.version} is installed; this script is pinned to {CAMOUFOX_PYTHON}')
    records = {str(entry).replace('\\', '/'): entry for entry in package.files or []}

    def recorded(name):
        # The wheel's RECORD names each file's sha256 (urlsafe base64, unpadded).
        entry = records.get('camoufox/' + name)
        if entry is None or entry.hash is None or entry.hash.mode != 'sha256':
            fail(f'camoufox/{name} is not listed with a sha256 in the package RECORD')
        data = Path(package.locate_file(entry)).read_bytes()
        digest = hashlib.sha256(data)
        if base64.urlsafe_b64encode(digest.digest()).rstrip(b'=').decode() != entry.hash.value:
            fail(f'camoufox/{name} does not match the camoufox {CAMOUFOX_PYTHON} RECORD')
        return data, digest.hexdigest()

    staged, files = {}, {}
    for name in CATALOG_FILES:
        staged[name], files[name] = recorded(name)
    pin = json.loads(recorded('browser-pin.json')[0])
    if pin.get('tag') != TAG:
        fail(f'camoufox {CAMOUFOX_PYTHON} pins {pin.get("tag")}, not {TAG} (runtime-manifest.json)')
    # runtime-manifest.json pins the fpgen model the launcher was released with.
    model = json.loads(recorded('fpgen-model.json')[0])
    manifest = MANIFEST.get('fpgen_model', {})
    for key in ('tag', 'url', 'size', 'sha256', 'file_sha256'):
        if manifest.get(key) != model.get(key):
            print(f'warning: runtime-manifest.json fpgen_model.{key} differs from the launcher pin: {model.get(key)!r}', file=sys.stderr)
    CATALOG.mkdir(parents=True, exist_ok=True)
    for name, data in staged.items():
        (CATALOG / name).write_bytes(data)
    document = {'camoufox_python': CAMOUFOX_PYTHON, 'browser': TAG, 'files': files}
    # LF on every OS: the build embeds this file's digest.
    (CATALOG / 'catalog.json').write_bytes((json.dumps(document, indent=1) + '\n').encode('utf-8'))
    print(CATALOG)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('venv', help='create the camoufox Python environment').set_defaults(run=venv)
    command = commands.add_parser('fetch', help='download and unpack the pinned Camoufox build')
    command.add_argument('--dir', default=str(TOOLS / 'camoufox'))
    command.add_argument('--archive', help='use an already downloaded archive')
    command.add_argument('--keep-archive', action='store_true')
    command.set_defaults(run=fetch)
    commands.add_parser('catalog', help='copy the embedded identity catalogues from the camoufox package').set_defaults(run=catalog)
    command = commands.add_parser('check-identities', help='check identities ais2api generated against the camoufox package')
    command.add_argument('--executable', required=True)
    command.add_argument('--identities', required=True, help='JSON lines, one stored identity each')
    command.add_argument('--count', type=int, default=0, help='reference identities to draw (default: as many)')
    command.add_argument('--top', type=int, default=8, help='values listed per feature')
    command.set_defaults(run=check_identities)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
