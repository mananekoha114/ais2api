#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Serves the local collection page on loopback and saves the table it posts back.

    python -I tools/shape/serve.py --out <raw.json> [--port 0] [--timeout 180]
    python -I tools/shape/serve.py --serve-only [--port 0] [--timeout 600]

Prints "url <address>" once listening, then exits after the first table
arrives (status 0), the page reports an error (status 2) or the time runs
out (status 1). With --serve-only it takes no table and serves until the time
runs out, for a collector that opens collect.html?manual and evaluates
collect.js itself (ais2api_shape_collect --local). Only collect.html,
frame.html and collect.js are served.
"""
import argparse
import http.server
import json
import pathlib
import sys
import threading

HERE = pathlib.Path(__file__).resolve().parent
FILES = {
    '/collect.html': ('collect.html', 'text/html; charset=utf-8'),
    '/frame.html': ('frame.html', 'text/html; charset=utf-8'),
    '/collect.js': ('collect.js', 'text/javascript; charset=utf-8'),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--serve-only', action='store_true')
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    if not args.serve_only and args.out is None:
        parser.error('--out is required unless --serve-only is given')

    done = threading.Event()
    outcome = {'status': 1}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            path = self.path.split('?', 1)[0]
            if path not in FILES:
                self.send_error(404)
                return
            name, kind = FILES[path]
            body = (HERE / name).read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if self.path != '/result' or args.serve_only:
                self.send_error(404)
                return
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_bytes(body)
            self.send_response(204)
            self.end_headers()
            try:
                failed = 'error' in json.loads(body) and len(json.loads(body)) == 1
            except ValueError:
                failed = True
            outcome['status'] = 2 if failed else 0
            done.set()

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f'url http://127.0.0.1:{server.server_address[1]}/collect.html', flush=True)
    done.wait(args.timeout)
    server.shutdown()
    if args.serve_only:
        return 0
    if outcome['status'] == 2:
        print(args.out.read_text(encoding='utf-8', errors='replace')[:4000], file=sys.stderr)
    print({0: 'saved', 1: 'timed out', 2: 'the page reported an error'}[outcome['status']], flush=True)
    return outcome['status']


if __name__ == '__main__':
    sys.exit(main())
