// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Reads the tunnels capture_proxy.py recorded: each ClientHello in full, and,
// with an SSLKEYLOGFILE, the client's HTTP/2 frames and, per request, its
// stream's priority, the stream's first WINDOW_UPDATE and the request headers
// in order (README.md). Header values are kept only for a fixed list of names;
// cookies keep their names, paths lose their query.
//
// usage: node analyze.cjs CAPTURE_DIR [KEYLOG] [--hosts host1,host2] [--json OUT]
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const http2 = require('http2');
const net = require('net');

const args = process.argv.slice(2);
const dir = args[0];
let keylogPath = null, hostFilter = null, jsonOut = null;
for (let i = 1; i < args.length; ++i) {
  if (args[i] === '--hosts') hostFilter = args[++i].split(',');
  else if (args[i] === '--json') jsonOut = args[++i];
  else keylogPath = args[i];
}

const EXT = {0: 'server_name', 5: 'status_request', 10: 'supported_groups', 11: 'ec_point_formats', 13: 'signature_algorithms',
  16: 'alpn', 18: 'signed_certificate_timestamp', 21: 'padding', 23: 'extended_master_secret', 27: 'compress_certificate',
  28: 'record_size_limit', 34: 'delegated_credentials', 35: 'session_ticket', 41: 'pre_shared_key', 43: 'supported_versions',
  45: 'psk_key_exchange_modes', 51: 'key_share', 17513: 'application_settings_old', 17613: 'application_settings',
  0xfe0d: 'encrypted_client_hello', 0xff01: 'renegotiation_info'};
const GROUP = {0x001d: 'x25519', 0x0017: 'secp256r1', 0x0018: 'secp384r1', 0x0019: 'secp521r1', 0x0100: 'ffdhe2048', 0x0101: 'ffdhe3072',
  0x11ec: 'X25519MLKEM768', 0x6399: 'X25519Kyber768Draft00'};
const VALUE_NAMES = new Set(['user-agent', 'accept', 'accept-language', 'accept-encoding', 'sec-fetch-dest', 'sec-fetch-mode', 'sec-fetch-site',
  'sec-fetch-user', 'upgrade-insecure-requests', 'te', 'priority', 'content-type', 'origin', 'x-user-agent', 'x-goog-api-client',
  ':method', ':scheme', 'dnt', 'sec-gpc', 'connection', 'pragma', 'cache-control', 'x-aistudio-g1-tier']);
const grease = v => (v & 0x0f0f) === 0x0a0a && ((v >> 8) === (v & 0xff));
const hex4 = v => v.toString(16).padStart(4, '0');

function records(buffer) {
  const out = [];
  let offset = 0;
  while (offset + 5 <= buffer.length) {
    const type = buffer[offset], length = buffer.readUInt16BE(offset + 3);
    if (offset + 5 + length > buffer.length) break;
    out.push({type, header: buffer.subarray(offset, offset + 5), body: buffer.subarray(offset + 5, offset + 5 + length)});
    offset += 5 + length;
  }
  return out;
}

function parseHello(body, client) {
  // Handshake header: type(1) length(3)
  let p = 4;
  const hello = {legacyVersion: hex4(body.readUInt16BE(p))};
  p += 2;
  hello.random = body.subarray(p, p + 32).toString('hex');
  p += 32;
  const sid = body[p]; p += 1 + sid;
  if (client) {
    const n = body.readUInt16BE(p); p += 2;
    hello.ciphers = [];
    for (let i = 0; i < n; i += 2) hello.ciphers.push(body.readUInt16BE(p + i));
    p += n;
    const c = body[p]; p += 1 + c;
  } else {
    hello.cipher = body.readUInt16BE(p); p += 2;
    p += 1;
  }
  const end = p + 2 + body.readUInt16BE(p);
  p += 2;
  hello.extensions = [];
  while (p + 4 <= end) {
    const type = body.readUInt16BE(p), length = body.readUInt16BE(p + 2);
    hello.extensions.push({type, data: body.subarray(p + 4, p + 4 + length)});
    p += 4 + length;
  }
  return hello;
}

function u16list(data, offset, lengthBytes) {
  const n = lengthBytes === 1 ? data[offset] : data.readUInt16BE(offset);
  const out = [];
  for (let i = 0; i < n; i += 2) out.push(data.readUInt16BE(offset + lengthBytes + i));
  return out;
}

function describeHello(hello) {
  const ext = Object.fromEntries(hello.extensions.map(e => [e.type, e.data]));
  const d = {
    ciphers: hello.ciphers.map(c => (grease(c) ? 'GREASE' : hex4(c))),
    extensions: hello.extensions.map(e => (grease(e.type) ? 'GREASE' : `${EXT[e.type] || 'unknown'}(${e.type})`)),
  };
  if (ext[10]) d.supportedGroups = u16list(ext[10], 0, 2).map(g => (grease(g) ? 'GREASE' : GROUP[g] || hex4(g)));
  if (ext[11]) d.pointFormats = [...ext[11].subarray(1, 1 + ext[11][0])];
  if (ext[13]) d.signatureAlgorithms = u16list(ext[13], 0, 2).map(hex4);
  if (ext[43]) d.supportedVersions = u16list(ext[43], 0, 1).map(v => (grease(v) ? 'GREASE' : hex4(v)));
  if (ext[16]) {
    const list = [];
    let p = 2;
    while (p < ext[16].length) { const n = ext[16][p]; list.push(ext[16].subarray(p + 1, p + 1 + n).toString()); p += 1 + n; }
    d.alpn = list;
  }
  if (ext[51]) {
    const shares = [];
    let p = 2;
    while (p + 4 <= ext[51].length) { const g = ext[51].readUInt16BE(p), n = ext[51].readUInt16BE(p + 2); shares.push(`${grease(g) ? 'GREASE' : GROUP[g] || hex4(g)}:${n}`); p += 4 + n; }
    d.keyShares = shares;
  }
  if (ext[28]) d.recordSizeLimit = ext[28].readUInt16BE(0);
  if (ext[34]) d.delegatedCredentials = u16list(ext[34], 0, 2).map(hex4);
  if (ext[27]) d.compressCertificate = [...ext[27].subarray(1, 1 + ext[27][0])].reduce((a, _, i, arr) => (i % 2 ? a : [...a, (arr[i] << 8) | arr[i + 1]]), []);
  if (ext[45]) d.pskModes = [...ext[45].subarray(1, 1 + ext[45][0])];
  if (ext[5]) d.statusRequest = true;
  if (ext[0xfe0d]) d.ech = {bytes: ext[0xfe0d].length, outer: ext[0xfe0d][0] === 0};
  if (ext[21]) d.paddingBytes = ext[21].length;
  // JA3 and JA4 as their authors define them.
  const ciphers = hello.ciphers.filter(c => !grease(c));
  const types = hello.extensions.map(e => e.type).filter(t => !grease(t));
  const groups = ext[10] ? u16list(ext[10], 0, 2).filter(g => !grease(g)) : [];
  d.ja3 = [771, ciphers.join('-'), types.join('-'), groups.join('-'), (d.pointFormats || []).join('-')].join(',');
  d.ja3Hash = crypto.createHash('md5').update(d.ja3).digest('hex');
  const versions = ext[43] ? u16list(ext[43], 0, 1).filter(v => !grease(v)) : [];
  const top = versions.length ? Math.max(...versions) : 0x0303;
  const version = {0x0304: '13', 0x0303: '12', 0x0302: '11', 0x0301: '10'}[top] || '00';
  const alpn = d.alpn && d.alpn.length ? d.alpn[0][0] + d.alpn[0][d.alpn[0].length - 1] : '00';
  const two = n => String(Math.min(n, 99)).padStart(2, '0');
  const sha12 = s => crypto.createHash('sha256').update(s).digest('hex').slice(0, 12);
  const sortedCiphers = ciphers.map(hex4).sort().join(',');
  const sortedExt = types.filter(t => t !== 0 && t !== 16).map(hex4).sort().join(',');
  const sigs = ext[13] ? u16list(ext[13], 0, 2).map(hex4).join(',') : '';
  d.ja4 = `t${version}${ext[0] ? 'd' : 'i'}${two(ciphers.length)}${two(types.length)}${alpn}_${sha12(sortedCiphers)}_${sha12(sigs ? sortedExt + '_' + sigs : sortedExt)}`;
  return d;
}

// ---- TLS 1.3 decryption of the client's records ----
const SUITE = {0x1301: {cipher: 'aes-128-gcm', key: 16, hash: 'sha256'}, 0x1302: {cipher: 'aes-256-gcm', key: 32, hash: 'sha384'},
  0x1303: {cipher: 'chacha20-poly1305', key: 32, hash: 'sha256'}};
function expandLabel(hash, secret, label, length) {
  const full = Buffer.from('tls13 ' + label);
  const info = Buffer.concat([Buffer.from([length >> 8, length & 255, full.length]), full, Buffer.from([0])]);
  let out = Buffer.alloc(0), block = Buffer.alloc(0);
  for (let i = 1; out.length < length; ++i) {
    block = crypto.createHmac(hash, secret).update(Buffer.concat([block, info, Buffer.from([i])])).digest();
    out = Buffer.concat([out, block]);
  }
  return out.subarray(0, length);
}
function keysFor(suite, secretHex) {
  const secret = Buffer.from(secretHex, 'hex');
  return {key: expandLabel(suite.hash, secret, 'key', suite.key), iv: expandLabel(suite.hash, secret, 'iv', 12), seq: 0n};
}
function open(suite, keys, record) {
  const nonce = Buffer.from(keys.iv);
  const seq = Buffer.alloc(8);
  seq.writeBigUInt64BE(keys.seq);
  for (let i = 0; i < 8; ++i) nonce[4 + i] ^= seq[i];
  const tag = record.body.subarray(record.body.length - 16);
  const decipher = crypto.createDecipheriv(suite.cipher, keys.key, nonce, {authTagLength: 16});
  decipher.setAAD(record.header);
  decipher.setAuthTag(tag);
  const plain = Buffer.concat([decipher.update(record.body.subarray(0, record.body.length - 16)), decipher.final()]);
  keys.seq += 1n;
  let end = plain.length;
  while (end > 0 && plain[end - 1] === 0) --end;
  return {type: plain[end - 1], data: plain.subarray(0, end - 1)};
}

function readKeylog(file) {
  const map = new Map();
  if (!file || !fs.existsSync(file)) return map;
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const [label, random, secret] = line.trim().split(/\s+/);
    if (!secret) continue;
    if (!map.has(random)) map.set(random, {});
    map.get(random)[label] = secret;
  }
  return map;
}

// ---- HTTP/2 ----
const FRAME = {0: 'DATA', 1: 'HEADERS', 2: 'PRIORITY', 3: 'RST_STREAM', 4: 'SETTINGS', 5: 'PUSH_PROMISE', 6: 'PING', 7: 'GOAWAY', 8: 'WINDOW_UPDATE',
  9: 'CONTINUATION', 16: 'PRIORITY_UPDATE'};
const SETTING = {1: 'HEADER_TABLE_SIZE', 2: 'ENABLE_PUSH', 3: 'MAX_CONCURRENT_STREAMS', 4: 'INITIAL_WINDOW_SIZE', 5: 'MAX_FRAME_SIZE',
  6: 'MAX_HEADER_LIST_SIZE', 8: 'ENABLE_CONNECT_PROTOCOL', 9: 'NO_RFC7540_PRIORITIES'};
const PREFACE = Buffer.from('PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n');
function h2Frames(stream) {
  const frames = [];
  if (!stream.subarray(0, PREFACE.length).equals(PREFACE)) return null;
  let p = PREFACE.length;
  while (p + 9 <= stream.length) {
    const length = stream.readUIntBE(p, 3), type = stream[p + 3], flags = stream[p + 4], id = stream.readUInt32BE(p + 5) & 0x7fffffff;
    if (p + 9 + length > stream.length) break;
    const payload = stream.subarray(p + 9, p + 9 + length);
    const frame = {type: FRAME[type] || `0x${type.toString(16)}`, flags, stream: id, length};
    if (type === 4 && !(flags & 1)) {
      frame.settings = [];
      for (let i = 0; i + 6 <= payload.length; i += 6) frame.settings.push(`${SETTING[payload.readUInt16BE(i)] || payload.readUInt16BE(i)}=${payload.readUInt32BE(i + 2)}`);
    }
    if (type === 8) frame.increment = payload.readUInt32BE(0) & 0x7fffffff;
    if (type === 2) frame.priority = {exclusive: !!(payload[0] & 0x80), dependsOn: payload.readUInt32BE(0) & 0x7fffffff, weight: payload[4] + 1};
    if (type === 1 && (flags & 0x20)) frame.priority = {exclusive: !!(payload[0] & 0x80), dependsOn: payload.readUInt32BE(0) & 0x7fffffff, weight: payload[4] + 1};
    if (type === 16) frame.priorityField = payload.subarray(4).toString();
    frames.push(frame);
    p += 9 + length;
  }
  return frames;
}

// The request headers, decoded by Node's own HTTP/2 server from the replayed
// client bytes; HPACK state follows the stream as it was sent.
function decodeHeaders(stream) {
  return new Promise(resolve => {
    const requests = [];
    const server = http2.createServer();
    server.on('stream', (s, headers, flags, rawHeaders) => {
      const names = [];
      const raw = rawHeaders || Object.entries(headers).flat();
      for (let i = 0; i < raw.length; i += 2) {
        const name = raw[i], value = raw[i + 1];
        if (name === ':path') names.push([name, String(value).split('?')[0]]);
        else if (name === ':authority') names.push([name, value]);
        else if (name === 'cookie') names.push([name, String(value).split(/;\s*/).map(c => c.split('=')[0]).join(',')]);
        else names.push(VALUE_NAMES.has(name) ? [name, value] : [name]);
      }
      requests.push(names);
      s.on('error', () => {});
      s.resume();
    });
    server.on('sessionError', () => {});
    server.listen(0, '127.0.0.1', () => {
      const socket = net.connect(server.address().port, '127.0.0.1', () => socket.end(stream));
      socket.on('data', () => {});
      socket.on('error', () => {});
      socket.on('close', () => setTimeout(() => { server.close(); resolve(requests); }, 200));
    });
  });
}

(async () => {
  const keylog = readKeylog(keylogPath);
  const tunnels = fs.readFileSync(path.join(dir, 'tunnels.tsv'), 'utf8').trim().split(/\r?\n/).map(l => l.split('\t'));
  const report = [];
  for (const [number, target] of tunnels) {
    const host = target.split(':')[0];
    if (hostFilter && !hostFilter.includes(host)) continue;
    const base = path.join(dir, `${number}-${target.replace(':', '_')}`);
    if (!fs.existsSync(base + '.c2s')) continue;
    const c2s = records(fs.readFileSync(base + '.c2s'));
    const s2c = records(fs.readFileSync(base + '.s2c'));
    if (!c2s.length || c2s[0].type !== 22) continue;
    const hello = parseHello(c2s[0].body, true);
    const entry = {tunnel: Number(number), host, clientHello: describeHello(hello)};
    const serverHello = s2c.length && s2c[0].type === 22 ? parseHello(s2c[0].body, false) : null;
    if (serverHello) {
      entry.cipher = hex4(serverHello.cipher);
      const sv = serverHello.extensions.find(e => e.type === 43);
      entry.version = sv ? hex4(sv.data.readUInt16BE(0)) : 'tls1.2';
    }
    const secrets = keylog.get(hello.random);
    const suite = serverHello && SUITE[serverHello.cipher];
    if (secrets && suite && secrets.CLIENT_HANDSHAKE_TRAFFIC_SECRET && secrets.CLIENT_TRAFFIC_SECRET_0) {
      let keys = keysFor(suite, secrets.CLIENT_HANDSHAKE_TRAFFIC_SECRET);
      let application = false;
      const appData = [];
      for (const record of c2s.slice(1)) {
        if (record.type !== 23) continue;
        try {
          const plain = open(suite, keys, record);
          if (!application && plain.type === 22) { keys = keysFor(suite, secrets.CLIENT_TRAFFIC_SECRET_0); application = true; continue; }
          if (plain.type === 23) appData.push(plain.data);
        } catch (error) { entry.decryptError = String(error.message); break; }
      }
      const stream = Buffer.concat(appData);
      const frames = h2Frames(stream);
      if (frames) {
        entry.h2 = {first: frames.slice(0, 8), frameTypes: [...new Set(frames.map(f => f.type))],
          priorityUpdates: frames.filter(f => f.type === 'PRIORITY_UPDATE').slice(0, 3).map(f => f.priorityField)};
        // Node reports the streams in the order of their HEADERS frames.
        const headers = frames.map((f, i) => [f, i]).filter(([f]) => f.type === 'HEADERS');
        const decoded = await decodeHeaders(stream);
        entry.requests = decoded.map((fields, n) => {
          const [frame, index] = headers[n] || [null, -1];
          const request = {headers: fields};
          if (frame) {
            request.stream = frame.stream;
            request.priority = frame.priority || null;
            const update = frames.findIndex((f, i) => i > index && f.stream === frame.stream && f.type === 'WINDOW_UPDATE');
            request.streamWindowUpdate = update < 0 ? null : {increment: frames[update].increment, framesAfterHeaders: update - index};
          }
          return request;
        });
      } else if (stream.length) entry.http1 = stream.subarray(0, stream.indexOf('\r\n\r\n')).toString().split('\r\n').map(l => l.split(':')[0]);
    } else entry.decrypted = false;
    report.push(entry);
  }
  if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify(report, null, 2));
  for (const entry of report) {
    console.log(`== tunnel ${entry.tunnel} ${entry.host} ${entry.version || ''} ${entry.cipher || ''}`);
    console.log('  ja4', entry.clientHello.ja4, ' ja3', entry.clientHello.ja3Hash);
    if (entry.h2) console.log('  h2 first frames', JSON.stringify(entry.h2.first.map(f => [f.type, f.settings || f.increment || f.priority || f.priorityField || ''])));
    for (const r of entry.requests || []) {
      const field = name => (r.headers.find(h => h[0] === name) || [])[1];
      console.log(`  ${field(':method')} ${field(':path')} weight=${r.priority ? r.priority.weight : '-'}` +
        ` streamWindowUpdate=${r.streamWindowUpdate ? r.streamWindowUpdate.increment : '-'}`);
      console.log('    ' + r.headers.filter(h => !h[0].startsWith(':')).map(h => (h.length > 1 && h[0] !== 'cookie' ? `${h[0]}=${h[1]}` : h[0])).join(' | '));
    }
  }
})();
