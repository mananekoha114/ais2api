#!/usr/bin/env python3
# Copyright (c) 2026 ais2api contributors
# SPDX-License-Identifier: MPL-2.0
"""Runtime package contracts, including negative credential/ABI/license cases."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('package_check', ROOT / 'scripts/check-package.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def executable(*, machine=183, interpreter='/system/bin/linker64', needed=('libc.so', 'libc++_shared.so'),
               mode=b'release'):
    """A small ELF fixture with real program/dynamic headers, never executed."""
    data = bytearray(1024)
    data[:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<HHIQQQIHHHHHH', data, 16, 3, machine, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    base, interp, dynamic, strings = 0x400000, 0x180, 0x200, 0x300
    loader = interpreter.encode() + b'\0'
    table = b'\0'
    offsets = []
    for name in needed:
        offsets.append(len(table))
        table += name.encode() + b'\0'
    entries = [(1, offset) for offset in offsets] + [(5, base + strings), (10, len(table)), (0, 0)]
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, base, base, len(data), len(data), 4096)
    struct.pack_into('<IIQQQQQQ', data, 120, 3, 4, interp, base + interp, 0, len(loader), len(loader), 1)
    struct.pack_into('<IIQQQQQQ', data, 176, 2, 6, dynamic, base + dynamic, 0, 16 * len(entries), 16 * len(entries), 8)
    data[interp:interp + len(loader)] = loader
    data[strings:strings + len(table)] = table
    for index, entry in enumerate(entries):
        struct.pack_into('<qQ', data, dynamic + 16 * index, *entry)
    # The build mode tag, as src/common.cpp leaves it in the executable.
    return bytes(data) + (checker.BUILD_TAG + mode if mode else b'')


class RuntimeArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.files = {
            'bin/ais2api': executable(),
            'start.sh': b'#!/bin/sh\n',
            'bin/browser/bridge.js': b'// fixture',
            'bin/runtime-manifest.json': b'{}',
            'bin/userscript/ais2api-cookie.user.js': b'// fixture',
            'share/ais2api/schemas/config.schema.json': b'{}',
            'share/doc/ais2api/LICENSE': (ROOT / 'LICENSE').read_bytes(),
            'share/doc/ais2api/platforms.md': b'fixture',
            'share/doc/ais2api/NOTICE': b'fixture',
            'share/doc/ais2api/THIRD_PARTY_NOTICES.md': b'fixture',
        }
        self.executables = {'bin/ais2api', 'start.sh'}
        self.batch = b'@echo off\r\necho ready\r\n'
        for package in ['boost', 'libc++', 'ndk-sysroot']:
            self.files[f'share/licenses/ais2api-dependencies/{package}/copyright'] = b'fixture license'
        self.metadata = {
            'schema_version': 1, 'target': 'termux-aarch64',
            'source_commit': 'a' * 40,
            'container_image': 'termux/termux-docker@sha256:' + 'b' * 64,
            'executable': checker.elf_runtime(self.files['bin/ais2api']),
            'packages': {name: {'version': '29', 'architecture': 'aarch64'}
                         for name in ['boost', 'libc++', 'ndk-sysroot']},
            'runtime_libraries': {
                'libc.so': {'provider': 'android-system'},
                'libc++_shared.so': {'provider': 'termux', 'package': 'libc++', 'needed': ['libc.so']},
            },
            'static_components': {'libc++experimental.a': {'package': 'ndk-sysroot', 'sha256': 'c' * 64}},
            'dependency_notices': {name: 'included' for name in ['boost', 'libc++', 'ndk-sysroot']},
        }

    def archive(self, *, metadata=True, windows=False):
        files = dict(self.files)
        if metadata:
            files['share/ais2api/termux-build.json'] = json.dumps(self.metadata).encode()
        if windows:
            files['bin/ais2api.exe'] = files.pop('bin/ais2api')
            if 'start.sh' in files:
                files['start.cmd'] = files.pop('start.sh')
                files['start.cmd'] = self.batch
            files['share/ais2api/start-messages.txt'] = '启动|中文\n'.encode()
        path = self.directory / ('runtime.zip' if windows else 'runtime.tar.gz')
        if windows:
            with zipfile.ZipFile(path, 'w') as output:
                for name, content in files.items():
                    output.writestr('runtime/' + name, content)
        else:
            with tarfile.open(path, 'w:gz') as output:
                for name, content in files.items():
                    entry = tarfile.TarInfo('runtime/' + name)
                    entry.size = len(content)
                    entry.mode = 0o755 if name in self.executables else 0o644
                    output.addfile(entry, io.BytesIO(content))
        Path(str(path) + '.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest() + '\n')
        return path

    def verify(self, path=None, target='termux-aarch64', dev=False, with_browser=False):
        with contextlib.redirect_stdout(io.StringIO()):
            return checker.check(path or self.archive(), with_browser, target, dev)

    def camoufox(self, version='158.0.1-beta.41', **marker):
        """A dev bundle's Camoufox for a manifest pinning `version`."""
        self.files['bin/runtime-manifest.json'] = json.dumps({'camoufox': {'version': '158.0.1-beta.41', 'artifacts': {
            'windows-x64': {'sha256': 'a' * 64}}}}).encode()
        prefix = f'bin/camoufox-runtime/{version}-windows-x64/'
        self.files[prefix + 'camoufox.exe'] = b'exe'
        self.files[prefix + 'fonts/TwemojiMozilla.ttf'] = b'font'
        self.files[prefix + '.ais2api-runtime.json'] = json.dumps({
            'browser': 'camoufox', 'browser_version': version, 'platform': 'windows-x64', 'sha256': 'a' * 64,
            'skipped': ['fonts', 'addons/UBO'], 'kept': ['fonts/TwemojiMozilla.ttf'], **marker}).encode()
        return prefix

    def dev_archive(self):
        self.files['bin/ais2api'] = executable(mode=b'dev')
        return self.archive(metadata=False, windows=True)

    def linux_camoufox(self, version='158.0.1-beta.41'):
        """A Linux dev bundle's Camoufox, with the font groups a Linux identity reads."""
        self.files['bin/runtime-manifest.json'] = json.dumps({'camoufox': {'version': version, 'artifacts': {
            'linux-x64': {'sha256': 'a' * 64}}}}).encode()
        prefix = f'bin/camoufox-runtime/{version}-linux-x64/'
        self.files[prefix + 'camoufox-bin'] = b'exe'
        self.executables.add(prefix + 'camoufox-bin')
        for name in ('fonts/groups.json', 'fonts/TwemojiMozilla.ttf', 'fonts/L/a.ttf', 'fonts/LMW/b.ttf',
                     'fontconfig/linux/fonts.conf'):
            self.files[prefix + name] = b'fixture'
        self.files[prefix + '.ais2api-runtime.json'] = json.dumps({
            'browser': 'camoufox', 'browser_version': version, 'platform': 'linux-x64', 'sha256': 'a' * 64,
            'skipped': ['fonts', 'fontconfig', 'addons/UBO'],
            'kept': ['fonts/groups.json', 'fonts/TwemojiMozilla.ttf', 'fontconfig/linux',
                     'fonts/L', 'fonts/LM', 'fonts/LMW', 'fonts/LW']}).encode()
        return prefix

    def linux_dev_archive(self):
        self.files['bin/ais2api'] = executable(mode=b'dev')
        return self.archive(metadata=False)

    def test_release_archives_hold_only_release_builds(self):
        self.files['bin/ais2api'] = executable(mode=b'dev')
        with self.assertRaisesRegex(ValueError, 'dev_build_in_release_archive'):
            self.verify(self.archive(metadata=False, windows=True), target='desktop')
        self.files['bin/ais2api'] = executable(mode=None)
        with self.assertRaisesRegex(ValueError, 'unknown_build_mode'):
            self.verify(self.archive(metadata=False), target='desktop')
        self.files['bin/ais2api'] = executable(mode=None) + checker.BUILD_TAG + b'release' + checker.BUILD_TAG + b'dev'
        with self.assertRaisesRegex(ValueError, 'unknown_build_mode'):
            self.verify(self.archive(metadata=False), target='desktop')

    def test_release_archives_carry_no_camoufox(self):
        self.camoufox()
        with self.assertRaisesRegex(ValueError, 'camoufox_bundle_in_release_archive'):
            self.verify(self.archive(metadata=False, windows=True), target='desktop')

    def test_dev_bundle_with_and_without_camoufox(self):
        self.assertEqual(self.verify(self.dev_archive(), target='desktop', dev=True), 0)
        self.camoufox()
        self.assertEqual(self.verify(self.dev_archive(), target='desktop', dev=True), 0)
        self.files['bin/ais2api'] = executable()
        with self.assertRaisesRegex(ValueError, 'not_a_dev_build'):
            self.verify(self.archive(metadata=False, windows=True), target='desktop', dev=True)
        with self.assertRaisesRegex(ValueError, 'dev_bundle_cannot_carry_cloakbrowser'):
            self.verify(self.dev_archive(), target='desktop', dev=True, with_browser=True)
        with self.assertRaisesRegex(ValueError, 'termux_requires_backend_tar_gz'):
            self.verify(dev=True)

    def test_linux_dev_bundle_carries_the_linux_font_groups(self):
        prefix = self.linux_camoufox()
        self.assertEqual(self.verify(self.linux_dev_archive(), target='desktop', dev=True), 0)
        for name in ('fonts/W/c.ttf', 'fonts/M/c.ttf', 'fontconfig/windows/fonts.conf'):
            self.files[prefix + name] = b'fixture'
            with self.assertRaisesRegex(ValueError, 'camoufox_bundle_untrimmed'):
                self.verify(self.linux_dev_archive(), target='desktop', dev=True)
            self.files.pop(prefix + name)
        self.executables.discard(prefix + 'camoufox-bin')
        with self.assertRaisesRegex(ValueError, 'camoufox_is_not_executable'):
            self.verify(self.linux_dev_archive(), target='desktop', dev=True)

    def test_dev_bundle_camoufox_must_be_the_pinned_trimmed_build(self):
        prefix = self.camoufox()
        self.files[prefix + 'fonts/001__Arial.ttf'] = b'font'
        with self.assertRaisesRegex(ValueError, 'camoufox_bundle_untrimmed'):
            self.verify(self.dev_archive(), target='desktop', dev=True)
        self.files.pop(prefix + 'fonts/001__Arial.ttf')
        self.files[prefix + 'addons/UBO/manifest.json'] = b'{}'
        with self.assertRaisesRegex(ValueError, 'camoufox_bundle_untrimmed'):
            self.verify(self.dev_archive(), target='desktop', dev=True)
        self.files.pop(prefix + 'addons/UBO/manifest.json')
        self.files.pop(prefix + 'camoufox.exe')
        with self.assertRaisesRegex(ValueError, 'missing_camoufox_executable'):
            self.verify(self.dev_archive(), target='desktop', dev=True)

        self.setUp()
        self.camoufox(sha256='b' * 64)
        with self.assertRaisesRegex(ValueError, 'camoufox_bundle_manifest_mismatch'):
            self.verify(self.dev_archive(), target='desktop', dev=True)
        self.setUp()
        self.camoufox(version='157.0')
        with self.assertRaisesRegex(ValueError, 'unexpected_camoufox_bundle'):
            self.verify(self.dev_archive(), target='desktop', dev=True)
        self.setUp()
        prefix = self.camoufox()
        self.files[prefix + '.ais2api-runtime.json'] = json.dumps({
            'browser': 'camoufox', 'browser_version': '158.0.1-beta.41', 'platform': 'windows-x64',
            'sha256': 'a' * 64}).encode()
        with self.assertRaisesRegex(ValueError, 'camoufox_bundle_marker_without_trim_rules'):
            self.verify(self.dev_archive(), target='desktop', dev=True)

    def test_termux_runtime_with_system_dependencies(self):
        self.assertEqual(self.verify(), 0)

    def test_desktop_linux_contract_does_not_require_termux_metadata(self):
        self.assertEqual(self.verify(self.archive(metadata=False), target='desktop'), 0)

    def test_desktop_windows_contract_is_preserved(self):
        self.assertEqual(self.verify(self.archive(metadata=False, windows=True), target='desktop'), 0)

    def test_termux_requires_its_metadata(self):
        with self.assertRaisesRegex(ValueError, 'missing_termux_build_metadata'):
            self.verify(self.archive(metadata=False))

    def test_wrong_cpu_is_rejected(self):
        self.files['bin/ais2api'] = executable(machine=62)
        with self.assertRaisesRegex(ValueError, 'expected_AArch64_ELF'):
            self.verify()

    def test_glibc_loader_is_rejected(self):
        self.files['bin/ais2api'] = executable(interpreter='/lib/ld-linux-aarch64.so.1')
        with self.assertRaisesRegex(ValueError, 'wrong_termux_interpreter'):
            self.verify()

    def test_elf_dynamic_dependencies_must_match_metadata(self):
        self.metadata['executable']['needed'] = ['libc.so']
        with self.assertRaisesRegex(ValueError, 'termux_executable_metadata_mismatch'):
            self.verify()

    def test_missing_runtime_dependency_is_rejected(self):
        del self.metadata['runtime_libraries']['libc++_shared.so']
        with self.assertRaisesRegex(ValueError, 'unreported_termux_runtime_library'):
            self.verify()

    def test_transitive_dependency_must_be_reported(self):
        self.metadata['runtime_libraries']['libc++_shared.so']['needed'].append('libmissing.so')
        with self.assertRaisesRegex(ValueError, 'unreported_termux_runtime_library'):
            self.verify()

    def test_arbitrary_library_cannot_be_declared_an_android_library(self):
        self.metadata['runtime_libraries']['libc++_shared.so'] = {'provider': 'android-system'}
        with self.assertRaisesRegex(ValueError, 'unexpected_android_system_library'):
            self.verify()

    def test_static_experimental_library_license_is_required(self):
        del self.files['share/licenses/ais2api-dependencies/ndk-sysroot/copyright']
        with self.assertRaisesRegex(ValueError, 'missing_termux_dependency_license'):
            self.verify()

    def test_linked_library_license_is_required(self):
        del self.files['share/licenses/ais2api-dependencies/libc++/copyright']
        with self.assertRaisesRegex(ValueError, 'missing_termux_dependency_license'):
            self.verify()

    def test_external_dynamic_library_notice_absence_is_explicit(self):
        self.files['bin/ais2api'] = executable(needed=('libc.so', 'libc++_shared.so', 'libicuuc.so.78'))
        self.metadata['executable'] = checker.elf_runtime(self.files['bin/ais2api'])
        self.metadata['packages']['libicu'] = {'version': '78.3', 'architecture': 'aarch64'}
        self.metadata['runtime_libraries']['libicuuc.so.78'] = {
            'provider': 'termux', 'package': 'libicu', 'needed': ['libc.so'],
        }
        self.metadata['dependency_notices']['libicu'] = 'external-system-package'
        self.assertEqual(self.verify(), 0)
        del self.metadata['dependency_notices']['libicu']
        with self.assertRaisesRegex(ValueError, 'missing_termux_dependency_license'):
            self.verify()

    def test_embedded_code_cannot_use_external_notice_exemption(self):
        self.metadata['dependency_notices']['ndk-sysroot'] = 'external-system-package'
        with self.assertRaisesRegex(ValueError, 'missing_termux_dependency_license'):
            self.verify()

    def test_static_library_provenance_is_required(self):
        self.metadata['static_components'] = {}
        with self.assertRaisesRegex(ValueError, 'missing_experimental_library_provenance'):
            self.verify()

    def test_bundled_system_libraries_are_rejected(self):
        self.files['bin/libc++_shared.so'] = b'not shipped'
        with self.assertRaisesRegex(ValueError, 'unexpected_bundled_termux_library'):
            self.verify()

    def test_runtime_credentials_are_rejected(self):
        for name in ['bin/config.json', 'bin/cookies.txt', 'bin/data/state.sqlite3', 'bin/.env']:
            with self.subTest(name=name):
                self.files[name] = b'synthetic-test-material'
                with self.assertRaisesRegex(ValueError, 'private_or_build_files_in_archive'):
                    self.verify()
                del self.files[name]

    def test_archive_path_traversal_is_rejected(self):
        self.files['../outside'] = b'fixture'
        with self.assertRaisesRegex(ValueError, 'unsafe_archive_entry'):
            self.verify()

    def test_archive_checksum_mismatch_is_rejected(self):
        path = self.archive()
        Path(str(path) + '.sha256').write_text('0' * 64 + '\n')
        with self.assertRaisesRegex(ValueError, 'archive_checksum_mismatch'):
            self.verify(path)

    def test_launcher_is_required(self):
        for windows in (False, True):
            with self.subTest(windows=windows):
                files = dict(self.files)
                del self.files['start.sh']
                with self.assertRaisesRegex(ValueError, 'missing_runtime_assets'):
                    self.verify(self.archive(metadata=False, windows=windows), target='desktop')
                self.files = files

    def test_launcher_must_be_executable(self):
        self.executables.remove('start.sh')
        with self.assertRaisesRegex(ValueError, 'launcher_is_not_executable'):
            self.verify()

    def test_windows_launcher_must_be_ascii_with_crlf(self):
        for batch, error in [('@echo off\r\necho 中文\r\n'.encode(), 'launcher_must_be_ascii'),
                             (b'@echo off\necho ready\n', 'launcher_requires_crlf')]:
            with self.subTest(error=error):
                self.batch = batch
                with self.assertRaisesRegex(ValueError, error):
                    self.verify(self.archive(metadata=False, windows=True), target='desktop')

    def test_repository_windows_launcher_is_packageable(self):
        self.batch = (ROOT / 'launcher/start.cmd').read_bytes()
        self.assertEqual(self.verify(self.archive(metadata=False, windows=True), target='desktop'), 0)

    def test_missing_runtime_assets_are_rejected(self):
        del self.files['bin/browser/bridge.js']
        with self.assertRaisesRegex(ValueError, 'missing_runtime_assets'):
            self.verify()

    def test_unpinned_image_is_rejected(self):
        self.metadata['container_image'] = 'termux/termux-docker:aarch64'
        with self.assertRaisesRegex(ValueError, 'unpinned_termux_container'):
            self.verify()

    def test_source_version_is_required(self):
        self.metadata['source_commit'] = 'latest'
        with self.assertRaisesRegex(ValueError, 'invalid_source_commit'):
            self.verify()

    def test_prefix_qualified_sqlite_dependency_is_supported(self):
        name = checker.TERMUX_PREFIX + '/lib/libsqlite3.so'
        self.assertEqual(checker.termux_soname(name), 'libsqlite3.so')
        self.files['bin/ais2api'] = executable(needed=('libc.so', name))
        self.metadata['executable'] = checker.elf_runtime(self.files['bin/ais2api'])
        self.metadata['packages']['libsqlite'] = {'version': '3.53.2', 'architecture': 'aarch64'}
        self.metadata['dependency_notices']['libsqlite'] = 'included'
        self.files['share/licenses/ais2api-dependencies/libsqlite/copyright'] = b'fixture license'
        self.metadata['runtime_libraries']['libsqlite3.so'] = {
            'provider': 'termux', 'package': 'libsqlite', 'needed': ['libc.so'],
        }
        self.assertEqual(self.verify(), 0)

    def test_unexpected_absolute_library_path_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'invalid_termux_library_name'):
            checker.termux_soname('/tmp/libsqlite3.so')

    def test_truncated_elf_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'invalid_ELF_segment'):
            checker.elf_runtime(executable()[:400])


if __name__ == '__main__':
    unittest.main()
