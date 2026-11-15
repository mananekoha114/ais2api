#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
"""One-click launcher contracts, run against a stub executable (never the service)."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / 'launcher'

# Answers the commands start.sh uses with the single-line JSON the real CLI
# prints when stdout is not a console, and records every invocation.
STUB = r'''#!/bin/sh
printf '%s\n' "$*" >> "$STUB_LOG"
case $1 in
  status) if [ -n "${STUB_RUNNING:-}" ]; then echo '{"running":true}'; else echo '{"running":false}'; fi ;;
  init) echo '{"initialized":true,"config_file":"/opt/ais2api/bin/config.json","api_key_configured":false,"admin_password_default":true}' ;;
  doctor) echo "{\"browser_exists\":${STUB_BROWSER:-true},\"service_log\":\"/opt/ais2api/bin/data/service.log\"}" ;;
  setup) echo '{"status":"prepared"}' ;;
  start) echo '{"running":true}' ;;
  config) echo '{"config":{"version":1,"bind":"0.0.0.0","port":18452,"data_dir":"/opt/ais2api/bin/data"}}' ;;
  serve) echo 'serving' ;;
  *) exit 9 ;;
esac
'''


@unittest.skipUnless(shutil.which('sh'), 'POSIX sh is required')
class ShellLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name) / 'ais2api 0.1.0'
        (root / 'bin').mkdir(parents=True)
        shutil.copyfile(LAUNCHER / 'start.sh', root / 'start.sh')
        stub = root / 'bin/ais2api'
        stub.write_text(STUB, encoding='utf-8', newline='\n')
        stub.chmod(0o755)
        self.root, self.log = root, Path(self.temp.name) / 'calls.log'

    def run_launcher(self, *args, **environment):
        env = {key: value for key, value in os.environ.items() if not key.startswith('STUB_')}
        env.update(STUB_LOG=str(self.log), **environment)
        result = subprocess.run(['sh', str(self.root / 'start.sh'), *args], env=env, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=60)
        calls = self.log.read_text(encoding='utf-8').splitlines() if self.log.exists() else []
        return result.returncode, result.stdout.decode('utf-8'), result.stderr.decode('utf-8'), calls

    def test_first_run_initializes_then_serves_in_the_foreground(self):
        code, out, _, calls = self.run_launcher('--port', '18452')
        self.assertEqual(code, 0)
        self.assertEqual(calls, ['status --port 18452', 'init --port 18452', 'doctor --port 18452',
                                 'serve --port 18452'])
        self.assertIn('/opt/ais2api/bin/config.json 中的 api_key 为空', out)
        self.assertIn('默认密码为 admin', out)
        self.assertTrue(out.rstrip().endswith('serving'))

    def test_running_service_is_left_alone(self):
        code, out, _, calls = self.run_launcher(STUB_RUNNING='1')
        self.assertEqual(code, 0)
        self.assertEqual(calls, ['status'])
        self.assertIn('已在运行', out)

    def test_background_start_reports_the_local_address(self):
        code, out, _, calls = self.run_launcher('--background')
        self.assertEqual(code, 0)
        self.assertEqual(calls, ['status', 'init', 'doctor', 'start', 'config show'])
        self.assertIn('http://127.0.0.1:18452/', out)
        self.assertIn('/opt/ais2api/bin/data/service.log', out)

    def test_missing_browser_needs_confirmation(self):
        code, _, err, calls = self.run_launcher(STUB_BROWSER='false')
        self.assertEqual(code, 1)
        self.assertNotIn('setup --yes', calls)
        self.assertIn('setup --yes', err)

    def test_skip_setup_starts_even_when_browser_is_missing(self):
        code, out, err, calls = self.run_launcher('--port', '18460', '--skip-setup', '--background', STUB_BROWSER='false')
        self.assertEqual(code, 0)
        self.assertEqual(calls, ['status --port 18460', 'init --port 18460', 'doctor --port 18460',
                                 'start --port 18460', 'config show --port 18460'])
        self.assertIn('http://127.0.0.1:18452/', out)
        self.assertEqual(err, '')


class WindowsLauncherTests(unittest.TestCase):
    def test_every_message_exists_and_the_batch_file_stays_ascii(self):
        batch = (LAUNCHER / 'start.cmd').read_bytes()
        self.assertTrue(batch.isascii())
        self.assertEqual(batch.count(b'\n'), batch.count(b'\r\n'))
        self.assertIn(b'--skip-setup', batch)
        messages = (LAUNCHER / 'start-messages.txt').read_bytes()
        self.assertFalse(messages.startswith(b'\xef\xbb\xbf'), 'a BOM would hide the first message')
        names = {line.split('|', 1)[0] for line in messages.decode('utf-8').splitlines()}
        used = set(re.findall(r'call :say (\w+)', batch.decode('ascii')))
        self.assertTrue(used)
        self.assertLessEqual(used, names)


if __name__ == '__main__':
    unittest.main()
