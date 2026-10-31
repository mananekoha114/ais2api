#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
"""Check runtime archives without extraction or service launch."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys
import tarfile
import zipfile

spec = importlib.util.spec_from_file_location('browser_bundle', Path(__file__).with_name('browser-bundle.py'))
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)

TERMUX_PREFIX = '/data/data/com.termux/files/usr'
ANDROID_LIBRARIES = {'libc.so', 'libdl.so', 'libm.so', 'liblog.so', 'libandroid.so'}
# Termux packages whose header/static code is compiled into the executable.
TERMUX_EMBEDDED_NOTICES = frozenset({'boost', 'libc++'})


def termux_soname(name: str) -> str:
    # SQLite's Termux build can record its full prefix path in DT_NEEDED.
    name = name.removeprefix(TERMUX_PREFIX + '/lib/')
    if not re.fullmatch(r'[A-Za-z0-9_+.-]+\.so(?:\.[0-9]+)*', name):
        raise ValueError('invalid_termux_library_name')
    return name


def elf_runtime(data: bytes) -> dict:
    """Read the ARM64 ELF loader contract without executing or extracting it."""
    if len(data) < 64 or len(data) > 100_000_000 or data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected_little_endian_ELF64')
    if struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('expected_AArch64_ELF')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', data, 54)
    if phsize != 56 or not 0 < phcount <= 1024 or phoff + phsize * phcount > len(data):
        raise ValueError('invalid_ELF_program_headers')
    loads, dynamic, interpreter = [], None, None
    for index in range(phcount):
        kind, _, offset, address, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + index * phsize)
        if offset + size > len(data):
            raise ValueError('invalid_ELF_segment')
        if kind == 1:
            loads.append((offset, address, size))
        elif kind == 2:
            if dynamic is not None or size % 16:
                raise ValueError('invalid_ELF_dynamic_segment')
            dynamic = (offset, size)
        elif kind == 3:
            value = data[offset:offset + size]
            if interpreter is not None or not value.endswith(b'\0') or b'\0' in value[:-1]:
                raise ValueError('invalid_ELF_interpreter')
            interpreter = value[:-1].decode('ascii')
    if dynamic is None:
        raise ValueError('missing_ELF_dynamic_segment')
    tags, needed_offsets = {}, []
    for offset in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        tag, value = struct.unpack_from('<qQ', data, offset)
        if tag == 0:
            break
        if tag == 1:
            needed_offsets.append(value)
        else:
            tags[tag] = value
    else:
        raise ValueError('unterminated_ELF_dynamic_segment')
    address, size = tags.get(5, -1), tags.get(10, 0)
    tables = [data[offset + address - base:offset + address - base + size]
              for offset, base, length in loads if base <= address and address + size <= base + length]
    if len(tables) != 1 or not size:
        raise ValueError('invalid_ELF_string_table')
    table, needed = tables[0], []
    for offset in needed_offsets:
        end = table.find(b'\0', offset)
        if not 0 <= offset < len(table) or end < 0:
            raise ValueError('invalid_ELF_needed_string')
        needed.append(table[offset:end].decode('ascii'))
    return {'machine': 'AArch64', 'interpreter': interpreter, 'needed': needed}


def check_termux(relative, read) -> None:
    metadata = 'share/ais2api/termux-build.json'
    if metadata not in relative:
        raise ValueError('missing_termux_build_metadata')
    info = json.loads(read(metadata, 1_000_000))
    if not isinstance(info, dict) or info.get('schema_version') != 1 or info.get('target') != 'termux-aarch64':
        raise ValueError('invalid_termux_build_metadata')
    if not re.fullmatch(r'[0-9a-f]{40}', info.get('source_commit', '')):
        raise ValueError('invalid_source_commit')
    if not re.fullmatch(r'termux/termux-docker@sha256:[0-9a-f]{64}', info.get('container_image', '')):
        raise ValueError('unpinned_termux_container')
    actual = elf_runtime(read('bin/ais2api', 100_000_001))
    if actual['interpreter'] != '/system/bin/linker64':
        raise ValueError('wrong_termux_interpreter')
    if actual != info.get('executable'):
        raise ValueError('termux_executable_metadata_mismatch')
    if any(re.search(r'\.so(?:\.[0-9]+)*$', name) for name in relative):
        raise ValueError('unexpected_bundled_termux_library')
    packages, libraries = info.get('packages'), info.get('runtime_libraries')
    if not isinstance(packages, dict) or not packages or not isinstance(libraries, dict) or not libraries:
        raise ValueError('missing_termux_dependencies')
    for package in packages.values():
        if not isinstance(package, dict) or not package.get('version') or package.get('architecture') not in {'aarch64', 'all'}:
            raise ValueError('invalid_termux_package')
    # Header/static code is distributed inside the executable. Shared libraries
    # stay in Termux's package manager and must declare their notice availability.
    required_licenses, runtime_packages = set(TERMUX_EMBEDDED_NOTICES), set()
    required_libraries = {termux_soname(name) for name in actual['needed']}
    for name, library in libraries.items():
        if termux_soname(name) != name or not isinstance(library, dict):
            raise ValueError('invalid_termux_library')
        provider = library.get('provider')
        if provider == 'android-system':
            if name not in ANDROID_LIBRARIES:
                raise ValueError('unexpected_android_system_library')
        elif provider == 'termux':
            package = library.get('package')
            if package not in packages or not isinstance(library.get('needed'), list):
                raise ValueError('unreported_termux_library_package')
            runtime_packages.add(package)
            required_libraries.update(termux_soname(item) for item in library['needed'])
        else:
            raise ValueError('invalid_termux_library_provider')
    if not required_libraries.issubset(libraries):
        raise ValueError('unreported_termux_runtime_library')
    component = info.get('static_components', {}).get('libc++experimental.a', {})
    if component.get('package') not in packages or not re.fullmatch(r'[0-9a-f]{64}', component.get('sha256', '')):
        raise ValueError('missing_experimental_library_provenance')
    required_licenses.add(component['package'])
    notices = info.get('dependency_notices')
    if not isinstance(notices, dict):
        raise ValueError('missing_termux_dependency_notices')
    for package in required_licenses | runtime_packages:
        name = f'share/licenses/ais2api-dependencies/{package}/copyright'
        declaration = notices.get(package)
        if declaration == 'included':
            if name not in relative or not read(name, 100000).strip():
                raise ValueError('missing_termux_dependency_license')
        elif declaration == 'external-system-package' and package not in required_licenses:
            if name in relative:
                raise ValueError('termux_notice_metadata_mismatch')
        else:
            raise ValueError('missing_termux_dependency_license')


def check(path: Path, with_browser=False, target='desktop') -> int:
    if target not in {'desktop', 'termux-aarch64'}:
        raise ValueError('invalid_package_target')
    if target == 'termux-aarch64' and (with_browser or not path.name.endswith('.tar.gz')):
        raise ValueError('termux_requires_backend_tar_gz')
    archive = zipfile.ZipFile(path) if path.suffix == '.zip' else tarfile.open(path, 'r:gz')
    with archive:
        names = {}
        entries = archive.infolist() if isinstance(archive, zipfile.ZipFile) else archive.getmembers()
        for entry in entries:
            entry_name = entry.filename if isinstance(archive, zipfile.ZipFile) else entry.name
            entry_path = PurePosixPath(entry_name)
            if '\\' in entry_name or ':' in entry_name or entry_path.is_absolute() or '..' in entry_path.parts:
                raise ValueError('unsafe_archive_entry')
            if isinstance(archive, zipfile.ZipFile):
                mode = entry.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise ValueError('archive_contains_links')
                if entry.is_dir():
                    continue
                name = entry.filename
            else:
                if not entry.isfile() and not entry.isdir():
                    raise ValueError('archive_contains_special_files')
                if entry.isdir():
                    continue
                name = entry.name
            if name in names:
                raise ValueError('duplicate_archive_member')
            names[name] = entry
        def read(name, limit=None):
            source = archive.open(names[name]) if isinstance(archive, zipfile.ZipFile) else archive.extractfile(names[name])
            with source:
                return source.read() if limit is None else source.read(limit)
        licenses = [name for name in names if name.endswith('/share/doc/ais2api/LICENSE')]
        if len(licenses) != 1 or not read(licenses[0], 100000).startswith(b'Mozilla Public License Version 2.0'):
            raise ValueError('project_license_is_not_MPL_2_0')
        forbidden = {'.tools', '.analysis', '.git', 'vcpkg_installed', 'data', 'dumps', 'server-logs',
                     '.env', 'installation.json', 'config.yaml', 'config.json', 'config.local.json',
                     'credentials', 'secrets', 'cookies.json', 'cookies.txt', 'storage_state.json',
                     'storage-state.json', 'service.log', 'state.sqlite3', 'usage.sqlite3'}
        relative, roots, original = set(), set(), {}
        for name in names:
            member = PurePosixPath(name)
            if '\\' in name or ':' in name or member.is_absolute() or '..' in member.parts or len(member.parts) < 2:
                raise ValueError('unsafe_or_unrooted_archive_entry')
            if forbidden.intersection(member.parts):
                raise ValueError('private_or_build_files_in_archive')
            roots.add(member.parts[0])
            item = PurePosixPath(*member.parts[1:]).as_posix()
            if item in relative:
                raise ValueError('duplicate_normalized_archive_member')
            relative.add(item)
            original[item] = name
        if len(roots) != 1:
            raise ValueError('archive_must_have_one_root')
        executable = 'bin/ais2api.exe' if path.suffix == '.zip' else 'bin/ais2api'
        launcher = 'start.cmd' if path.suffix == '.zip' else 'start.sh'
        required = {executable, launcher, *(['share/ais2api/start-messages.txt'] if launcher == 'start.cmd' else []), 'bin/browser/bridge.js', 'bin/runtime-manifest.json', 'bin/userscript/ais2api-cookie.user.js', 'share/doc/ais2api/LICENSE', 'share/doc/ais2api/platforms.md', 'share/doc/ais2api/NOTICE', 'share/doc/ais2api/THIRD_PARTY_NOTICES.md'}
        if not required.issubset(relative):
            raise ValueError('missing_runtime_assets')
        if launcher == 'start.sh' and not names[original[launcher]].mode & 0o100:
            raise ValueError('launcher_is_not_executable')
        if launcher == 'start.cmd':
            # cmd misreads batch files holding multi-byte text under code page
            # 65001 and mishandles labels in LF-only files; text lives in
            # start-messages.txt instead.
            script = read(original[launcher], 1_000_000)
            if any(byte > 0x7f for byte in script):
                raise ValueError('launcher_must_be_ascii')
            if script.count(b'\n') != script.count(b'\r\n'):
                raise ValueError('launcher_requires_crlf')
        if not any(name.startswith('share/ais2api/schemas/') for name in relative):
            raise ValueError('missing_schemas')
        if not any(name.startswith('share/licenses/ais2api-dependencies/') and name.endswith('/copyright') for name in relative):
            raise ValueError('missing_dependency_licenses')
        if target == 'termux-aarch64':
            check_termux(relative, lambda name, limit=None: read(original[name], limit))
        browser_files = {name for name in relative if name.startswith('bin/browser-runtime/')}
        if with_browser:
            platform = 'windows-x64' if path.suffix == '.zip' else 'linux-x64'
            prefix = 'bin/browser-runtime/' + platform + '/'
            manifest = json.loads(read(original['bin/runtime-manifest.json'], 100000))
            marker_name = prefix + bundle.MARKER
            if marker_name not in relative:
                raise ValueError('missing_browser_bundle_marker')
            marker = json.loads(read(original[marker_name], 4000000))
            expected = {'platform': platform, 'browser_version': manifest['browser_version'], 'sha256': manifest['artifacts'][platform]['sha256']}
            if any(marker.get(key) != value for key, value in expected.items()):
                raise ValueError('browser_bundle_manifest_mismatch')
            actual = {}
            # Compressed tar seeks backwards by replaying decompression. Hash in
            # archive order so a large browser is read sequentially.
            ordered_browser_files = sorted(browser_files, key=lambda name:
                names[original[name]].header_offset if isinstance(archive, zipfile.ZipFile)
                else names[original[name]].offset_data)
            for name in ordered_browser_files:
                if not name.startswith(prefix):
                    raise ValueError('unexpected_browser_platform')
                local = bundle.safe_name(name[len(prefix):])
                if local == bundle.MARKER:
                    continue
                source = archive.open(names[original[name]]) if isinstance(archive, zipfile.ZipFile) else archive.extractfile(names[original[name]])
                with source:
                    actual[local] = bundle.digest(source)
            if 'BINARY-LICENSE.md' not in actual or marker.get('usage') != 'local-internal':
                raise ValueError('missing_browser_binary_license_or_local_usage')
            if not actual or marker.get('files') != actual:
                raise ValueError('browser_bundle_inventory_mismatch')
            if ('chrome.exe' if platform == 'windows-x64' else 'chrome') not in actual:
                raise ValueError('missing_browser_executable')
        elif browser_files:
            raise ValueError('unexpected_browser_bundle_use_with_browser')
    expected = Path(str(path) + '.sha256').read_text(encoding='utf-8').split()[0].lower()
    with path.open('rb') as source:
        digest = bundle.digest(source)
    if digest != expected:
        raise ValueError('archive_checksum_mismatch')
    print(f'Runtime archive verified: {len(names)} files, SHA-256 matches' + (', browser inventory matches' if with_browser else ''))
    return 0

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--with-browser', action='store_true')
    parser.add_argument('--target', choices=['desktop', 'termux-aarch64'], default='desktop')
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    try:
        sys.exit(check(args.archive, args.with_browser, args.target))
    except Exception as error:
        print(f'Runtime archive validation failed: {type(error).__name__}: {error}', file=sys.stderr)
        sys.exit(1)
