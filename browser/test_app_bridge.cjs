// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Development-only tests. Node is not a production runtime dependency.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// `respond(url, init, page)` plays the frame's credentialed fetch, which the page may
// install only after the bridge runs.
function harness(respond) {
  const requests = [];
  const context = vm.createContext({
    Response, URL, TextDecoder, Blob, Uint8Array, AbortController, ReadableStream, btoa, setTimeout, clearTimeout,
    location: { href: 'https://example.scf.usercontent.goog/' }
  });
  vm.runInContext(fs.readFileSync(__dirname + '/app_bridge.js', 'utf8'), context, { filename: 'app_bridge.js' });
  context.fetch = async (url, init) => { requests.push({ url, init }); return respond(url, init, context); };
  return { bridge: context.__ais2app, requests };
}
const tick = () => new Promise(resolve => setTimeout(resolve, 5));
async function waitFor(predicate, description) {
  const deadline = Date.now() + 2000;
  while (!predicate()) {
    assert.ok(Date.now() < deadline, 'Timed out waiting for ' + description);
    await tick();
  }
}
// Reads the whole relay, acknowledging each chunk, and returns its bytes.
async function drain(h) {
  const output = [];
  for (let sequence = 1; ; ++sequence) {
    await waitFor(() => !!h.bridge.poll()?.pending || !!h.bridge.poll()?.done, 'relay state');
    const state = h.bridge.poll();
    if (!state.pending) return { bytes: Buffer.concat(output), state };
    assert.equal(state.pending.sequence, sequence);
    output.push(Buffer.from(state.pending.bytes, 'base64'));
    h.bridge.ack(sequence);
  }
}
function start(h, body, model = 'gemini-test', chunkBytes = 32768) {
  h.bridge.append('upload-1', body);
  return h.bridge.start({ upload: 'upload-1', model, chunkBytes });
}
const pattern = size => Uint8Array.from({ length: size }, (_, i) => i % 251);

test('generation posts the uploaded body to streamGenerateContent without credentials of its own', async () => {
  const sse = 'data: {"candidates":[{"content":{"parts":[{"text":"1"}]},"finishReason":"STOP"}]}\r\n\r\n';
  const h = harness(() => new Response(sse, { headers: { 'content-type': 'text/event-stream' } }));
  start(h, '{"contents":[]}');
  const { bytes, state } = await drain(h);
  assert.equal(bytes.toString(), sse);
  assert.equal(state.error, '');
  assert.equal(state.status, 200);
  assert.equal(h.requests.length, 1);
  const { url, init } = h.requests[0];
  assert.equal(url, 'https://generativelanguage.googleapis.com/v1beta/models/gemini-test:streamGenerateContent?alt=sse');
  assert.equal(init.method, 'POST');
  assert.deepEqual(Object.keys(init.headers), ['content-type']);
  assert.equal(init.headers['content-type'], 'application/json');
  assert.equal(await init.body.text(), '{"contents":[]}');
});

test('byte streams are read with a 32 KiB BYOB buffer and wait for credit', async () => {
  let reads = 0, offset = 0;
  const data = pattern(70000);
  const h = harness(() => new Response(new ReadableStream({
    type: 'bytes',
    pull(controller) {
      ++reads;
      if (offset === data.length) { controller.close(); controller.byobRequest?.respond(0); return; }
      const view = controller.byobRequest.view;
      const size = Math.min(view.byteLength, data.length - offset);
      view.set(data.subarray(offset, offset + size));
      offset += size;
      controller.byobRequest.respond(size);
    }
  })));
  start(h, '{}');
  await waitFor(() => !!h.bridge.poll()?.pending, 'first chunk');
  assert.equal(Buffer.from(h.bridge.poll().pending.bytes, 'base64').length, 32768);
  await tick();
  assert.equal(reads, 1, 'the page reads ahead only after credit');
  assert.throws(() => h.bridge.ack(2), /invalid_credit/);
  const { bytes } = await drain(h);
  assert.deepEqual(new Uint8Array(bytes), data);
});

test('default reader chunks are relayed in pieces of at most chunkBytes', async () => {
  const data = pattern(70000);
  const h = harness(() => new Response(new ReadableStream({
    start(controller) { controller.enqueue(data); controller.close(); }
  })));
  start(h, '{}', 'gemini-test', 16384);
  const sizes = [];
  for (let sequence = 1; ; ++sequence) {
    await waitFor(() => !!h.bridge.poll()?.pending || !!h.bridge.poll()?.done, 'relay state');
    const pending = h.bridge.poll().pending;
    if (!pending) break;
    sizes.push(Buffer.from(pending.bytes, 'base64').length);
    h.bridge.ack(sequence);
  }
  assert.deepEqual(sizes, [16384, 16384, 16384, 16384, 4464]);
  assert.equal(h.bridge.poll().error, '');
});

test('HTTP errors keep status, Retry-After and at most 8 KiB of the body', async () => {
  const body = JSON.stringify({ error: { code: 429, status: 'RESOURCE_EXHAUSTED', message: 'x'.repeat(20000) } });
  const h = harness(() => new Response(body, { status: 429, headers: { 'retry-after': '42' } }));
  start(h, '{}');
  const { bytes, state } = await drain(h);
  assert.equal(bytes.length, 0);
  assert.equal(state.status, 429);
  assert.equal(state.retryAfter, 42);
  assert.equal(state.error, 'upstream_http_429');
  assert.equal(state.errorBody.length, 8192);
  assert.ok(state.errorBody.startsWith('{"error":{"code":429'));
});

test('abort releases a pending chunk and ends the transfer as cancelled', async () => {
  const h = harness(() => new Response(new ReadableStream({
    start(controller) { controller.enqueue(pattern(100)); }
  })));
  start(h, '{}');
  await waitFor(() => !!h.bridge.poll()?.pending, 'first chunk');
  h.bridge.abort();
  await waitFor(() => !!h.bridge.poll()?.done, 'completion');
  assert.equal(h.bridge.poll().pending, null);
  assert.equal(h.bridge.poll().error, 'cancelled');
});

test('model ids that are not a single path segment never reach the network', () => {
  const h = harness(() => assert.fail('unexpected fetch'));
  for (const model of ['../files', 'models/x', 'a?b', '', 'x'.repeat(129)])
    assert.throws(() => start(h, '{}', model), /invalid model id/);
  assert.equal(h.requests.length, 0);
  assert.throws(() => h.bridge.start({ upload: 'missing', model: 'gemini-test' }), /upload missing/);
});

test('model listing follows page tokens and keeps only real model entries', async () => {
  const pages = {
    '': { models: [{ name: 'models/gemini-a', displayName: 'Gemini A', inputTokenLimit: 10, outputTokenLimit: 5, supportedGenerationMethods: ['generateContent', 7] }, { name: 'tunedModels/x' }], nextPageToken: 'next' },
    next: { models: [{ name: 'models/embedding-b' }] }
  };
  const h = harness(url => new Response(JSON.stringify(pages[new URL(url).searchParams.get('pageToken') || ''])));
  const models = await h.bridge.models();
  assert.deepEqual(JSON.parse(JSON.stringify(models)), [
    { id: 'gemini-a', name: 'models/gemini-a', displayName: 'Gemini A', description: '', inputTokenLimit: 10, outputTokenLimit: 5, supportedGenerationMethods: ['generateContent'], source: 'live' },
    { id: 'embedding-b', name: 'models/embedding-b', displayName: 'embedding-b', description: '', inputTokenLimit: 0, outputTokenLimit: 0, supportedGenerationMethods: [], source: 'live' }
  ]);
  assert.equal(h.requests.length, 2);
  assert.equal(h.requests[0].init.method, 'GET');
  assert.equal(new URL(h.requests[0].url).pathname, '/v1beta/models');
});

test('model listing reports upstream failures by category', async () => {
  await assert.rejects(harness(() => new Response('denied', { status: 403 })).bridge.models(), /ListModels_http_403/);
  await assert.rejects(harness(() => new Response('not json')).bridge.models(), /invalid model list JSON/);
  await assert.rejects(harness(() => new Response('{"models":{}}')).bridge.models(), /unknown model list shape/);
  await assert.rejects(harness((url, init, page) => { throw vm.runInContext("new TypeError('Failed to fetch')", page); }).bridge.models(), /ListModels_network_error/);
});
