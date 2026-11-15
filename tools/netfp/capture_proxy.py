#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""A local HTTP CONNECT proxy that records each tunnel's raw bytes and chains
to an upstream HTTP proxy, for comparing network fingerprints (README.md).
Nothing is decrypted or changed. The captures hold account cookies once
decrypted: keep them out of the repository and delete them after analysis.

usage: python -I capture_proxy.py LISTEN_PORT UPSTREAM_HOST UPSTREAM_PORT OUT_DIR
Each tunnel N to host H writes N-H.c2s (client to server) and N-H.s2c, plus a
line in OUT_DIR/tunnels.tsv.
"""
import asyncio, itertools, os, sys, time

LISTEN, UP_HOST, UP_PORT, OUT = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4]
os.makedirs(OUT, exist_ok=True)
counter = itertools.count(1)


async def pump(reader, writer, sink):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            sink.write(data)
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        sink.flush()
        try:
            writer.close()
        except Exception:
            pass


async def handle(client_reader, client_writer):
    try:
        head = await client_reader.readuntil(b'\r\n\r\n')
    except Exception:
        client_writer.close()
        return
    line = head.split(b'\r\n', 1)[0].decode('latin-1')
    parts = line.split(' ')
    if len(parts) < 2 or parts[0] != 'CONNECT':
        client_writer.write(b'HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n')
        await client_writer.drain()
        client_writer.close()
        return
    target = parts[1]
    try:
        up_reader, up_writer = await asyncio.open_connection(UP_HOST, UP_PORT)
        up_writer.write(f'CONNECT {target} HTTP/1.1\r\nHost: {target}\r\n\r\n'.encode())
        await up_writer.drain()
        reply = await up_reader.readuntil(b'\r\n\r\n')
    except Exception:
        client_writer.write(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n')
        await client_writer.drain()
        client_writer.close()
        return
    status = reply.split(b'\r\n', 1)[0]
    if b' 200' not in status:
        client_writer.write(reply)
        await client_writer.drain()
        client_writer.close()
        up_writer.close()
        return
    number = next(counter)
    host = target.replace(':', '_')
    with open(os.path.join(OUT, 'tunnels.tsv'), 'a', encoding='utf-8') as log:
        log.write(f'{number}\t{target}\t{time.strftime("%H:%M:%S")}\n')
    client_writer.write(b'HTTP/1.1 200 Connection Established\r\n\r\n')
    await client_writer.drain()
    with open(os.path.join(OUT, f'{number}-{host}.c2s'), 'wb') as c2s, open(os.path.join(OUT, f'{number}-{host}.s2c'), 'wb') as s2c:
        await asyncio.gather(pump(client_reader, up_writer, c2s), pump(up_reader, client_writer, s2c))


async def main():
    server = await asyncio.start_server(handle, '127.0.0.1', LISTEN)
    print(f'listening on 127.0.0.1:{LISTEN}', flush=True)
    async with server:
        await server.serve_forever()


asyncio.run(main())
