#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Builds a shape table from three collections.

    python -I tools/shape/merge.py --base <live.json> --official <official-local.json>
        --camoufox <camoufox-local.json> --out <table.json.gz> --report <report.md>
        [--slot botguard ...] [--page-url https://aistudio.google.com/prompts/new_chat]
        [--labels "Camoufox,official Firefox"]

--base       Camoufox on the live page: the page's globals, structure and key order.
--official   official Firefox on collect.html.
--camoufox   Camoufox on collect.html, with the same identity as --base.

What Camoufox shows on collect.html and official Firefox does not is Camoufox's
own: keys only Camoufox has are dropped from the base, keys only official
Firefox has are added after the key that precedes them there, and entries
that differ take official Firefox's. A fresh frame's keys, the builtins, the
promise members and the tag map have nothing of the page and are taken from
official Firefox as they are. Everything an identity overrides at run time,
or that describes this machine, is replaced with fixed values (see
normalize()). Names given with --slot are globals the WAA program defines
itself; they keep their place in the key order and nothing else.

The same step moves a table to another platform: with the Windows table as
--base, official Firefox for Windows as --camoufox and official Firefox for
Linux as --official, what differs between the two official builds is
applied to it (--labels "Windows,Linux" names them in the report). The fixed
values come out the same when applied twice.
"""
import argparse
import copy
import difflib
import gzip
import json
import pathlib
import re
import sys

PAGE_URL = 'https://aistudio.google.com/prompts/new_chat'
TIME_ORIGIN = 1767225600000  # 2026-01-01T00:00:00Z
LAST_MODIFIED = '01/01/2026 00:00:00'
CHROME_HEIGHT = 85
SCREEN = {'width': 1920, 'height': 1080, 'availWidth': 1920, 'availHeight': 1040, 'availLeft': 0, 'availTop': 0,
          'left': 0, 'top': 0, 'colorDepth': 24, 'pixelDepth': 24}
WINDOW = {'outerWidth': 1920, 'outerHeight': 1040, 'innerWidth': 1920, 'innerHeight': 1040 - CHROME_HEIGHT,
          'screenX': 0, 'screenY': 0, 'screenLeft': 0, 'screenTop': 0, 'mozInnerScreenX': 0,
          'mozInnerScreenY': CHROME_HEIGHT, 'devicePixelRatio': 1, 'scrollX': 0, 'scrollY': 0, 'pageXOffset': 0,
          'pageYOffset': 0, 'scrollMaxX': 0, 'scrollMaxY': 0}
NAVIGATOR = {'hardwareConcurrency': 8, 'language': 'en-US', 'languages': ['en-US', 'en']}
# Media features no identity overrides: a desktop with a mouse, default
# preferences and an sRGB screen in landscape.
MEDIA = {
    'any-pointer:fine': True, 'any-pointer:coarse': False, 'any-pointer:none': False, 'pointer:fine': True,
    'pointer:coarse': False, 'pointer:none': False, 'any-hover:hover': True, 'any-hover:none': False,
    'hover:hover': True, 'hover:none': False, 'prefers-color-scheme:dark': False, 'prefers-color-scheme:light': True,
    'prefers-reduced-motion:reduce': False, 'prefers-reduced-motion:no-preference': True,
    'prefers-contrast:no-preference': True, 'prefers-contrast:more': False, 'prefers-contrast:less': False,
    'prefers-reduced-transparency:reduce': False, 'forced-colors:active': False, 'forced-colors:none': True,
    'inverted-colors:inverted': False, 'inverted-colors:none': True, 'color-gamut:srgb': True, 'color-gamut:p3': False,
    'color-gamut:rec2020': False, 'dynamic-range:high': False, 'dynamic-range:standard': True,
    'orientation:landscape': True, 'orientation:portrait': False, 'display-mode:browser': True,
    'display-mode:standalone': False, 'display-mode:fullscreen': False, 'scripting:enabled': True, 'update:fast': True,
    'overflow-block:scroll': True, 'monochrome': False, 'color': True, 'grid': False,
    'prefers-reduced-data:reduce': False,
}
# A frame is hidden on the page: its window has no size of its own.
FRAME_KEEP = {'innerWidth', 'innerHeight', 'mozInnerScreenX', 'mozInnerScreenY', 'scrollMaxX', 'scrollMaxY'}


def load(path):
    data = pathlib.Path(path).read_bytes()
    return json.loads(gzip.decompress(data) if str(path).endswith('.gz') else data)


def encode(value):
    if value is None:
        return {'t': 'null'}
    if isinstance(value, bool):
        return {'t': 'b', 'v': value}
    if isinstance(value, (int, float)):
        return {'t': 'n', 'v': value}
    if isinstance(value, str):
        return {'t': 's', 'v': value}
    if isinstance(value, list):
        return {'t': 'a', 'v': [encode(item) for item in value]}
    raise TypeError(value)


class Report:
    def __init__(self, before, after):
        self.lines = []
        self.before, self.after = before, after

    def add(self, section, text):
        self.lines.append(f'- **{section}**: {text}')

    def write(self, path, header):
        body = '\n'.join(self.lines) if self.lines else '- (nothing)'
        pathlib.Path(path).write_text(f'# {header}\n\n{body}\n', encoding='utf-8')


def meta(entry):
    """An entry without its value, which differs from page to page."""
    return {k: v for k, v in entry.items() if k != 'v'} if isinstance(entry, dict) and 'f' in entry else entry


def merge_entries(section, base, camoufox, official, report, key=lambda e: e['n']):
    """Applies Camoufox's differences from official Firefox to an ordered list.

    Entries are compared without their values; an entry that differs takes
    official Firefox's description and keeps the base's value."""
    cam = {key(e): e for e in camoufox}
    off = {key(e): e for e in official}
    result = [e for e in base if not (key(e) in cam and key(e) not in off)]
    dropped = [key(e) for e in base if key(e) in cam and key(e) not in off]
    if dropped:
        report.add(section, f'dropped, only {report.before} has them: {", ".join(map(str, dropped))}')
    names = [key(e) for e in result]
    added = []
    for index, e in enumerate(official):
        name = key(e)
        if name in cam or name in names:
            continue
        before = next((key(official[j]) for j in range(index - 1, -1, -1) if key(official[j]) in names), None)
        at = names.index(before) + 1 if before is not None else 0
        result.insert(at, copy.deepcopy(e))
        names.insert(at, name)
        added.append(name)
    if added:
        report.add(section, f'added, only {report.after} has them: {", ".join(map(str, added))}')
    replaced, kept = [], []
    for i, e in enumerate(result):
        name = key(e)
        if name in cam and name in off and meta(cam[name]) != meta(off[name]):
            if meta(e) == meta(cam[name]):
                result[i] = copy.deepcopy(off[name])
                if 'v' in e and 'v' in result[i]:
                    result[i]['v'] = e['v']
                replaced.append(name)
            else:
                kept.append(name)
    if replaced:
        report.add(section, f"{report.after}'s entries replace {report.before}'s: {', '.join(map(str, replaced))}")
    if kept:
        report.add(section, f'kept as the page left them, though {report.before} and {report.after} differ: {", ".join(map(str, kept))}')
    return result


def merge_keys(section, base, camoufox, official, report):
    """Applies Camoufox's differences in which keys a value map has."""
    result = {k: v for k, v in base.items() if not (k in camoufox and k not in official)}
    dropped = [k for k in base if k in camoufox and k not in official]
    added = [k for k in official if k not in camoufox and k not in result]
    for k in added:
        result[k] = copy.deepcopy(official[k])
    if dropped:
        report.add(section, f'dropped, only {report.before} has them: {", ".join(dropped)}')
    if added:
        report.add(section, f'added, only {report.after} has them: {", ".join(added)}')
    return result


def relocate(section, names, reference, moved, report):
    """Moves each name in `moved` to just after its predecessor in `reference`.

    Where a top window resolves its globals depends on what its page touched
    and when, so official Firefox's order on the local page differs a little
    from run to run; the report names each new predecessor for review.
    """
    result = list(names)
    done = []
    for name in moved:
        if name not in result or name not in reference:
            continue
        index = reference.index(name)
        before = next((reference[j] for j in range(index - 1, -1, -1) if reference[j] in result and reference[j] != name), None)
        result.remove(name)
        at = result.index(before) + 1 if before is not None else 0
        result.insert(at, name)
        done.append(f'{name} (after {before})' if before is not None else f'{name} (first)')
    if done:
        report.add(section, f"moved to {report.after}'s place in the key order: {', '.join(done)}")
    return result


def reorder(entries, names):
    by_name = {e['n']: e for e in entries}
    return [by_name[n] for n in names]


def order_artefacts(camoufox_keys, official_keys):
    """Keys a fresh Camoufox window has out of official Firefox's order."""
    matcher = difflib.SequenceMatcher(None, official_keys, camoufox_keys, autojunk=False)
    moved = []
    for op, i1, i2, _, _ in matcher.get_opcodes():
        if op != 'equal':
            moved += [k for k in official_keys[i1:i2] if k in camoufox_keys and k not in moved]
    return moved


def normalize(table, official, page_url, report):
    """Replaces what identities override, and what describes a machine, with fixed values."""
    origin = re.match(r'^(https?://[^/]+)', page_url).group(1)

    def set_values(values, fixed):
        for name, value in fixed.items():
            if name in values:
                values[name] = encode(value)

    def strip_account(value):
        if isinstance(value, dict):
            return {k: strip_account(v) for k, v in value.items()}
        if isinstance(value, list):
            return [strip_account(v) for v in value]
        if isinstance(value, str):
            return re.sub(r'(aistudio\.google\.com)/u/\d+/', r'\1/', value)
        return value

    off_navigator = official['singletons']['navigator']['values']
    navigator = {name: off_navigator[name] for name in ('userAgent', 'appVersion', 'platform', 'oscpu') if name in off_navigator}
    table['userAgent'] = official['userAgent']

    top_origin = table['topSingletons']['performance']['values']['timeOrigin']['v']

    # Times since the epoch move with the top page's time origin.
    def shift_times(values):
        for name, value in values.items():
            if value.get('t') == 'n' and isinstance(value.get('v'), (int, float)) and value['v'] >= 1e12:
                values[name] = {'t': 'n', 'v': round(value['v'] - top_origin + TIME_ORIGIN, 3)}

    page_location = {
        'href': page_url, 'origin': origin, 'protocol': origin.split(':')[0] + ':',
        'host': origin.split('//')[1], 'hostname': origin.split('//')[1].split(':')[0],
        'port': '', 'pathname': page_url[len(origin):] or '/', 'search': '', 'hash': '',
    }
    for realm_name, singles, wv in (('top', table['topSingletons'], table['top']['windowValues']),
                                    ('frame', table['singletons'], table['frame']['windowValues'])):
        captured_inner = (wv.get('innerWidth', {}).get('v'), wv.get('innerHeight', {}).get('v'))
        window = {k: v for k, v in WINDOW.items() if not (realm_name == 'frame' and k in FRAME_KEEP)}
        set_values(wv, window)
        if 'navigator' in singles:
            set_values(singles['navigator']['values'], NAVIGATOR)
            for name, value in navigator.items():
                if name in singles['navigator']['values']:
                    singles['navigator']['values'][name] = copy.deepcopy(value)
        if 'screen' in singles:
            set_values(singles['screen']['values'], SCREEN)
        if realm_name == 'top':
            if 'visualViewport' in singles:
                set_values(singles['visualViewport']['values'], {'width': WINDOW['innerWidth'], 'height': WINDOW['innerHeight'],
                                                                 'offsetLeft': 0, 'offsetTop': 0, 'pageLeft': 0, 'pageTop': 0, 'scale': 1})
            # Sizes that follow the window's: the window's size less a margin
            # or a scrollbar.
            for path in ('document.documentElement', 'document.body', 'document.head'):
                if path not in singles:
                    continue
                values = singles[path]['values']
                for name, value in values.items():
                    if value.get('t') != 'n' or not isinstance(value.get('v'), (int, float)):
                        continue
                    axis = 0 if name.endswith('Width') else 1 if name.endswith('Height') else None
                    if axis is None or captured_inner[axis] is None:
                        continue
                    less = captured_inner[axis] - value['v']
                    if 0 <= less <= 64 and value['v'] > 0:
                        values[name] = encode(WINDOW['innerWidth' if axis == 0 else 'innerHeight'] - less)
            if 'location' in singles:
                set_values(singles['location']['values'], page_location)
            if 'document' in singles:
                set_values(singles['document']['values'], {'URL': page_url, 'documentURI': page_url, 'referrer': ''})
        for path in ('localStorage', 'sessionStorage'):
            if path in singles:
                set_values(singles[path]['values'], {'length': 0})
                singles[path]['own'] = []
        if 'document' in singles:
            set_values(singles['document']['values'], {'cookie': '', 'lastModified': LAST_MODIFIED})
        if 'performance' in singles:
            shift_times(singles['performance']['values'])
    for name in ('Navigator', 'Screen', 'HTMLDocument', 'Storage', 'Performance', 'VisualViewport', 'Location'):
        path = {'Navigator': 'navigator', 'Screen': 'screen', 'HTMLDocument': 'document', 'Storage': 'localStorage',
                'Performance': 'performance', 'VisualViewport': 'visualViewport', 'Location': 'location'}[name]
        if name in table['defaults'] and path in table['singletons']:
            table['defaults'][name] = copy.deepcopy(table['singletons'][path]['values'])
    if 'Storage' in table['own']:
        table['own']['Storage'] = []
    if 'PerformanceTiming' in table['defaults']:
        shift_times(table['defaults']['PerformanceTiming'])

    stack = f'@{page_url}:1:1\n'
    for iface, fields in (('Error', ('fileName',)), ('DOMException', ('filename',))):
        values = table['defaults'].get(iface)
        if not values:
            continue
        set_values(values, {fields[0]: page_url, 'lineNumber': 1, 'columnNumber': 1, 'stack': stack})
    for e in table['own'].get('Error', []):
        if e['n'] == 'fileName':
            e['v'] = encode(page_url)
        if e['n'] in ('lineNumber', 'columnNumber'):
            e['v'] = encode(1)

    queries = {}
    for text, (_, media, _) in official['mediaQueries'].items():
        feature = re.sub(r'\s', '', text[1:-1])
        if feature.startswith('-moz-platform:'):
            top_match, frame_match = official['mediaQueries'][text][0], official['mediaQueries'][text][2]
        elif feature in MEDIA:
            top_match = frame_match = MEDIA[feature]
        else:
            sys.exit(f'merge.py: no fixed value for the media query {text}')
        queries[text] = [top_match, media, frame_match]
    table['mediaQueries'] = queries

    for section in list(table):
        table[section] = strip_account(table[section])
    report.add('values', 'identity, machine, account and time values replaced with fixed ones')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', required=True)
    parser.add_argument('--official', required=True)
    parser.add_argument('--camoufox', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--report', required=True)
    parser.add_argument('--slot', action='append', default=[])
    parser.add_argument('--page-url', default=PAGE_URL)
    parser.add_argument('--labels', default='Camoufox,official Firefox')
    args = parser.parse_args()
    labels = args.labels.split(',')
    if len(labels) != 2:
        parser.error('--labels takes two names separated by a comma')

    base, official, camoufox = load(args.base), load(args.official), load(args.camoufox)
    for name, data in (('base', base), ('official', official), ('camoufox', camoufox)):
        if 'error' in data:
            sys.exit(f'merge.py: the {name} collection reported an error')
    report = Report(*labels)
    table = copy.deepcopy(base)
    # Whole instances and namespaces one build has and the other not.
    for section in ('namespaces', 'defaults', 'own', 'singletons', 'topSingletons'):
        table[section] = merge_keys(section, table[section], camoufox[section], official[section], report)

    table['interfaces'] = merge_entries('interfaces', base['interfaces'], camoufox['interfaces'], official['interfaces'], report)
    moved = order_artefacts(camoufox['frame']['freshKeys'], official['frame']['freshKeys'])
    for realm in ('top', 'frame'):
        shape = table[realm]
        shape['global'] = merge_entries(f'{realm}.global', base[realm]['global'], camoufox[realm]['global'], official[realm]['global'], report)
        names = relocate(f'{realm}.global', [e['n'] for e in shape['global']], [e['n'] for e in official[realm]['global']], moved, report)
        shape['global'] = reorder(shape['global'], names)
        for i, level in enumerate(shape['chain']):
            cam_level = next((l for l in camoufox[realm]['chain'] if l['name'] == level['name']), None)
            off_level = next((l for l in official[realm]['chain'] if l['name'] == level['name']), None)
            if cam_level and off_level:
                level['own'] = merge_entries(f'{realm}.chain.{level["name"]}', level['own'], cam_level['own'], off_level['own'], report)
        shape['windowValues'] = merge_keys(f'{realm}.windowValues', base[realm]['windowValues'], camoufox[realm]['windowValues'], official[realm]['windowValues'], report)
    table['frame']['freshKeys'] = list(official['frame']['freshKeys'])
    for name, namespace in table['namespaces'].items():
        if name in camoufox['namespaces'] and name in official['namespaces']:
            namespace['own'] = merge_entries(f'namespaces.{name}', namespace['own'], camoufox['namespaces'][name]['own'], official['namespaces'][name]['own'], report)
    for iface in list(table['defaults']):
        if iface in camoufox['defaults'] and iface in official['defaults']:
            table['defaults'][iface] = merge_keys(f'defaults.{iface}', table['defaults'][iface], camoufox['defaults'][iface], official['defaults'][iface], report)
    for iface in list(table['own']):
        if iface in camoufox['own'] and iface in official['own']:
            table['own'][iface] = merge_entries(f'own.{iface}', table['own'][iface], camoufox['own'][iface], official['own'][iface], report)
    for section in ('singletons', 'topSingletons'):
        for path, single in table[section].items():
            cam, off = camoufox[section].get(path), official[section].get(path)
            if cam and off:
                single['values'] = merge_keys(f'{section}.{path}.values', single['values'], cam['values'], off['values'], report)
                single['own'] = merge_entries(f'{section}.{path}.own', single['own'], cam['own'], off['own'], report)
    for section in ('builtins', 'promises', 'tagMap'):
        table[section] = copy.deepcopy(official[section])

    for realm in ('top', 'frame'):
        for e in table[realm]['global']:
            if e['n'] in args.slot:
                for field in list(e):
                    if field != 'n':
                        del e[field]
                e['k'] = 'slot'
                table[realm]['windowValues'].pop(e['n'], None)
    if args.slot:
        report.add('slots', ', '.join(args.slot))

    normalize(table, official, args.page_url, report)
    text = json.dumps(table, ensure_ascii=False, separators=(',', ':'))
    pathlib.Path(args.out).write_bytes(gzip.compress(text.encode('utf-8'), compresslevel=9, mtime=0))
    report.write(args.report, f'Shape table merge: {pathlib.Path(args.out).name}')
    print(f'{args.out}: {len(text)} bytes of JSON')


if __name__ == '__main__':
    main()
