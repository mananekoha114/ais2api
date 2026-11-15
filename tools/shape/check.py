#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Checks a shape table before it goes into the repository.

    python -I tools/shape/check.py <table.json.gz> [--forbid <text> ...]

- Format: the sections and fields dom.js and smhost.js read, and interfaces
  listed parents first.
- Privacy: no e-mail addresses, account paths (/u/N/), cookie values, local
  paths or loopback addresses, and the values merge.py fixes are the fixed
  ones. --forbid adds strings that must not appear, such as a machine's name.
- Size: at most 512 KiB compressed, the limit of scripts/embed-waa.cmake.

Exits with status 1 and lists the problems if there are any.
"""
import argparse
import gzip
import json
import pathlib
import re
import sys

LIMIT = 512 * 1024
SECTIONS = ['userAgent', 'capturedAt', 'interfaces', 'frame', 'top', 'namespaces', 'defaults', 'own', 'singletons',
            'topSingletons', 'tagMap', 'mediaQueries', 'builtins', 'promises']
INTERFACE_FIELDS = {'n', 'cn', 'p', 'pp', 'l', 'pw', 'st', 'pr', 'call', 'construct'}
KINDS = {'a', 'm', 'i', 'v', 'slot'}
VALUE_TYPES = {'u', 'null', 's', 'b', 'n', 'bigint', 'sym', 'a', 'ref', 'f', 'o', 'throw'}
FIXED = {
    ('singletons', 'navigator', 'hardwareConcurrency'): 8,
    ('topSingletons', 'navigator', 'hardwareConcurrency'): 8,
    ('singletons', 'navigator', 'language'): 'en-US',
    ('topSingletons', 'navigator', 'language'): 'en-US',
    ('singletons', 'screen', 'width'): 1920,
    ('singletons', 'screen', 'height'): 1080,
    ('topSingletons', 'screen', 'width'): 1920,
    ('topSingletons', 'screen', 'height'): 1080,
    ('singletons', 'localStorage', 'length'): 0,
    ('topSingletons', 'localStorage', 'length'): 0,
    ('singletons', 'document', 'cookie'): '',
    ('topSingletons', 'document', 'cookie'): '',
}
PATTERNS = {
    'an e-mail address': re.compile(r'[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z0-9.-]*[A-Za-z]{2,}'),
    'an account path': re.compile(r'/u/\d+/'),
    'an account ID': re.compile(r'\bacc_[0-9a-z]'),
    'a Google session cookie value': re.compile(r'\bg\.a000[0-9A-Za-z_-]{10,}|\bsidts-[0-9A-Za-z_-]{10,}'),
    'a local path': re.compile(r'[A-Za-z]:\\\\|/home/[a-z]|/Users/[A-Za-z]|file:///'),
    'a loopback address': re.compile(r'127\.0\.0\.1|localhost:\d|\[::1\]'),
}


def check_value(where, value, problems):
    if not isinstance(value, dict) or value.get('t') not in VALUE_TYPES:
        problems.append(f'{where}: not a value: {json.dumps(value)[:80]}')
        return
    if value['t'] == 'a':
        for i, item in enumerate(value['v']):
            check_value(f'{where}[{i}]', item, problems)


def check_entries(where, entries, problems):
    for e in entries:
        if not isinstance(e, dict) or not isinstance(e.get('n'), str) or e.get('k') not in KINDS:
            problems.append(f'{where}: not an entry: {json.dumps(e)[:80]}')
        elif e['k'] == 'v':
            check_value(f'{where}.{e["n"]}', e.get('v'), problems)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('table', type=pathlib.Path)
    parser.add_argument('--forbid', action='append', default=[])
    args = parser.parse_args()

    raw = args.table.read_bytes()
    text = gzip.decompress(raw).decode('utf-8')
    table = json.loads(text)
    problems = []

    if len(raw) > LIMIT:
        problems.append(f'size: {len(raw)} bytes compressed, more than {LIMIT}')
    if list(table) != SECTIONS:
        problems.append(f'sections: {list(table)}')
    seen = set()
    for info in table.get('interfaces', []):
        if set(info) != INTERFACE_FIELDS:
            problems.append(f'interface {info.get("n")}: fields {sorted(info)}')
            continue
        parent = info['pp'] if info['pp'] and info['pp'] != 'Object' and any(i['n'] == info['pp'] for i in table['interfaces']) else info['p']
        if parent and parent != info['n'] and any(i['n'] == parent for i in table['interfaces']) and parent not in seen:
            problems.append(f'interface {info["n"]}: listed before its parent {parent}')
        seen.add(info['n'])
        check_entries(f'interface {info["n"]}.st', info['st'], problems)
        check_entries(f'interface {info["n"]}.pr', info['pr'], problems)
    for realm in ('top', 'frame'):
        shape = table.get(realm, {})
        check_entries(f'{realm}.global', shape.get('global', []), problems)
        for name, value in shape.get('windowValues', {}).items():
            check_value(f'{realm}.windowValues.{name}', value, problems)
        for level in shape.get('chain', []):
            check_entries(f'{realm}.chain.{level.get("name")}', level.get('own', []), problems)
    if 'Function' not in table.get('frame', {}).get('freshKeys', []):
        problems.append('frame.freshKeys: no Function')
    for section in ('singletons', 'topSingletons'):
        for path, single in table.get(section, {}).items():
            if set(single) != {'iface', 'values', 'own'}:
                problems.append(f'{section}.{path}: fields {sorted(single)}')
                continue
            check_entries(f'{section}.{path}.own', single['own'], problems)
            for name, value in single['values'].items():
                check_value(f'{section}.{path}.{name}', value, problems)
    for iface, values in table.get('defaults', {}).items():
        for name, value in values.items():
            check_value(f'defaults.{iface}.{name}', value, problems)
    for text_, result in table.get('mediaQueries', {}).items():
        if not (isinstance(result, list) and len(result) == 3 and isinstance(result[0], bool) and isinstance(result[2], bool)):
            problems.append(f'mediaQueries {text_}: {result}')

    for (section, path, name), expected in FIXED.items():
        value = table.get(section, {}).get(path, {}).get('values', {}).get(name)
        if value is not None and value.get('v') != expected:
            problems.append(f'{section}.{path}.{name}: {value.get("v")!r}, not the fixed {expected!r}')
    for realm in ('top', 'frame'):
        values = table.get(realm, {}).get('windowValues', {})
        if values.get('devicePixelRatio', {}).get('v', 1) != 1:
            problems.append(f'{realm}.windowValues.devicePixelRatio is not 1')
    for label, pattern in PATTERNS.items():
        for match in list(pattern.finditer(text))[:3]:
            problems.append(f'privacy: {label}: …{text[max(0, match.start() - 40):match.end() + 20]}…')
    for forbidden in args.forbid:
        if forbidden in text:
            problems.append(f'privacy: contains {forbidden!r}')

    if problems:
        print('\n'.join(problems))
        print(f'{args.table}: {len(problems)} problem(s)')
        return 1
    print(f'{args.table}: ok ({len(raw)} bytes compressed, {len(text)} bytes of JSON)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
