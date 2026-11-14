#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Collects a shape table from a Firefox build on the local collection page.

    python -I tools/shape/run-local.py <firefox executable> <raw.json>
        [--headless] [--camou-config <config.json>] [--timeout 180]

Starts serve.py, opens its page in the browser with a new temporary profile
and stops the browser, by its process ID, once the table has arrived. The
profile sends every request except loopback ones to a closed port, and turns
off updates, telemetry and first-run pages, so the browser reaches nothing
outside this machine. No WebDriver is involved.

--camou-config passes a Camoufox configuration the way ais2api does
(CAMOU_CONFIG_1, CAMOU_CONFIG_2, ... in pieces of up to 2047 characters).
"""
import argparse
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
PREFS = {
    'browser.shell.checkDefaultBrowser': False,
    'browser.startup.homepage_override.mstone': 'ignore',
    'browser.aboutwelcome.enabled': False,
    'trailhead.firstrun.didSeeAboutWelcome': True,
    'datareporting.policy.dataSubmissionEnabled': False,
    'datareporting.healthreport.uploadEnabled': False,
    'toolkit.telemetry.enabled': False,
    'app.update.disabledForTesting': True,
    'app.normandy.enabled': False,
    'browser.discovery.enabled': False,
    'extensions.update.enabled': False,
    # Everything but loopback goes to a closed port.
    'network.proxy.type': 1,
    'network.proxy.http': '127.0.0.1',
    'network.proxy.http_port': 9,
    'network.proxy.ssl': '127.0.0.1',
    'network.proxy.ssl_port': 9,
    'network.proxy.no_proxies_on': '',
    'network.proxy.allow_hijacking_localhost': False,
    'network.dns.disablePrefetch': True,
    'network.prefetch-next': False,
}


def pref_line(name, value):
    if isinstance(value, bool):
        text = 'true' if value else 'false'
    elif isinstance(value, int):
        text = str(value)
    else:
        text = '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
    return f'user_pref("{name}", {text});'


def config_pieces(text):
    pieces, rest = [], text
    while rest:
        size = min(len(rest), 2047)
        pieces.append(rest[:size])
        rest = rest[size:]
    return {f'CAMOU_CONFIG_{i + 1}': piece for i, piece in enumerate(pieces)}


def stop(process):
    if process.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(15)
    except subprocess.TimeoutExpired:
        if os.name != 'nt':
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('browser')
    parser.add_argument('out', type=pathlib.Path)
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--camou-config', type=pathlib.Path)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()

    server = subprocess.Popen([sys.executable, '-I', str(HERE / 'serve.py'), '--out', str(args.out), '--timeout', str(args.timeout)],
                              stdout=subprocess.PIPE, text=True)
    line = server.stdout.readline().split()
    if len(line) != 2 or line[0] != 'url':
        server.kill()
        sys.exit('serve.py did not start')
    url = line[1]

    profile = pathlib.Path(tempfile.mkdtemp(prefix='shape-profile-'))
    (profile / 'user.js').write_text('\n'.join(pref_line(k, v) for k, v in PREFS.items()) + '\n', encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('CAMOU_CONFIG_', 'CAMOU_PREFS_'))}
    if args.camou_config:
        env.update(config_pieces(args.camou_config.read_text(encoding='utf-8')))
    # CreateProcess does not take a relative path with forward slashes.
    executable = pathlib.Path(args.browser)
    executable = str(executable.resolve()) if executable.is_file() else args.browser
    command = [executable, '-no-remote', '-wait-for-browser', '-profile', str(profile)]
    if args.headless:
        command.append('-headless')
    command += ['-new-tab', url]
    log = open(str(args.out) + '.log', 'wb')
    browser = None
    try:
        browser = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=os.name != 'nt')
        status = server.wait(args.timeout + 30)
    except subprocess.TimeoutExpired:
        status = 1
    finally:
        if server.poll() is None:
            server.kill()
        if browser is not None:
            stop(browser)
        log.close()
        shutil.rmtree(profile, ignore_errors=True)
    print(server.stdout.read().strip())
    return status


if __name__ == '__main__':
    sys.exit(main())
