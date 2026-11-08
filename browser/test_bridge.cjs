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

function harness(options = {}) {
  let fetches = 0, tierFetches = 0, reads = 0, sourceOffset = 0;
  class FakeXHR extends EventTarget {
    open(method, url) { this.method = method; this.url = url; }
    dispatchEvent(event) { super.dispatchEvent(event); this['on' + event.type]?.(event); return true; }
    abort() { this.aborted = true; }
    setRequestHeader() {}
    send() { this.sent = true; }
  }
  const errorBytes = options.errorBody === undefined ? null : new TextEncoder().encode(options.errorBody);
  const length = errorBytes ? errorBytes.length : 70000;
  const context = vm.createContext({
    crypto: require('node:crypto').webcrypto,
    Request, Response, URL, TextDecoder, TextEncoder, Blob, DOMException, Event, Uint8Array, AbortController, btoa, XMLHttpRequest: FakeXHR,
    KeyboardEvent: class extends Event { constructor(type, init = {}) { super(type, init); const { bubbles, cancelable, ...keys } = init; Object.assign(this, keys); } },
    Date: class extends Date { static now() { return options.now ? options.now() : Date.now(); } },
    setInterval: callback => { context.install = callback; return 1; }, clearInterval() {},
    setTimeout: (fn, ms) => setTimeout(fn, options.fastDiscoveryWait && ms === 100 ? 0 : options.fastDiscoveryTimeout && ms === 10000 ? 5 : options.fastBenefitTimeout && ms === 8000 ? 5 : ms), clearTimeout,
    location: { href: 'https://aistudio.google.com/prompts/new_chat?authuser=0', origin: 'https://aistudio.google.com', hostname: 'aistudio.google.com' },
    document: { cookie: 'SAPISID=synthetic-cookie', readyState: 'complete', querySelector: () => ({}), querySelectorAll: () => [] },
    default_MakerSuite: options.exports || { adapter(service, hash) { return service.snapshot(hash); } },
    fetch: async (input, init) => {
      const url = typeof input === 'string' ? input : input.url;
      if (url.includes('/GetAiStudioBenefitTier')) {
        ++tierFetches;
        if (options.tierFetch) return options.tierFetch(input, init);
        return new Response('[0]');
      }
      ++fetches;
      if (options.observeFetch) options.observeFetch(input, init);
      if (url.includes('/ListModels')) {
        if (options.discoveryFetch) return options.discoveryFetch(input, init);
        return new Response(JSON.stringify([[['models/live-model', null, null, 'Live Model', 'Returned by RPC', 123, 45, ['generateContent']]]]));
      }
      if (options.stall) return new Response(new ReadableStream({ type: 'bytes', pull: () => new Promise(() => {}) }));
      return new Response(new ReadableStream({
        type: 'bytes',
        pull(controller) {
          ++reads;
          if (sourceOffset === length) { controller.close(); if (controller.byobRequest) controller.byobRequest.respond(0); return; }
          const target = controller.byobRequest.view;
          const size = Math.min(target.byteLength, length - sourceOffset);
          for (let i = 0; i < size; ++i) target[i] = errorBytes ? errorBytes[sourceOffset + i] : (sourceOffset + i) % 251;
          sourceOffset += size;
          controller.byobRequest.respond(size);
        }
      }), { status: options.httpStatus || 200, headers: options.retryAfter ? { 'retry-after': options.retryAfter } : {} });
    }
  });
  vm.runInContext(fs.readFileSync(__dirname + '/bridge.js', 'utf8'), context, { filename: 'bridge.js' });
  context.install();
  const bridge = context.__ais2bridge;
  async function capture() {
    const response = await context.fetch('https://alkalimakersuite-pa.clients6.google.com/$rpc/google.internal.alkali.applications.makersuite.v1.MakerSuiteService/GenerateContent', { method: 'POST', headers: { 'content-type': 'application/json+protobuf', 'x-goog-api-key': 'test-key-never-real', authorization: 'SAPISIDHASH synthetic-captured', ...options.captureHeaders }, body: '["real-captured-template",null,null,null,"ticket:bootstrap"]' });
    assert.equal(await response.text(), '[]');
    await context.default_MakerSuite.adapter({ snapshot: hash => Promise.resolve(hash === 'a'.repeat(64) ? 'ticket:bootstrap' : 'ticket:' + hash) }, 'a'.repeat(64));

  }
  return { context, bridge, capture, fetches: () => fetches, tierFetches: () => tierFetches, reads: () => reads };
}
const tick = () => new Promise(resolve => setTimeout(resolve, 5));
test('capability reports browser timezone for grounding requests', () => {
  const h = harness();
  assert.equal(h.bridge.capability().timezone, Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC');
});
async function waitFor(predicate, description) {
  const deadline = Date.now() + 2000;
  while (!predicate()) {
    assert.ok(Date.now() < deadline, 'Timed out waiting for ' + description);
    await tick();
  }
}
async function waitPending(h) {
  await waitFor(() => !!h.bridge.poll()?.pending || !!h.bridge.poll()?.done, 'relay pending chunk');
  const state = h.bridge.poll();
  assert.ok(state.pending, 'Relay finished before producing a chunk: ' + state.error);
  return state.pending;
}
const waitDone = h => waitFor(() => !!h.bridge.poll()?.done, 'relay completion');
const modelsUrl = 'https://alkalimakersuite-pa.clients6.google.com/$rpc/google.internal.alkali.applications.makersuite.v1.MakerSuiteService/ListModels';
function captureModelsXHR(h, headers = {}, url = modelsUrl) {
  const xhr = new h.context.XMLHttpRequest();
  xhr.open('POST', url);
  xhr.setRequestHeader('Authorization', 'SAPISIDHASH synthetic-discovery');
  for (const [key, value] of Object.entries(headers)) xhr.setRequestHeader(key, value);
  xhr.send('[]');
  assert.equal(xhr.sent, true);
}

test('live capture, fresh snapshot service and secret-free capability report', async () => {
  const h = harness(); await h.capture();
  assert.equal(h.fetches(), 0, 'bootstrap generation must not reach upstream');
  assert.equal(await h.bridge.snapshot('content-hash-one'), 'ticket:content-hash-one');
  assert.equal(await h.bridge.snapshot('content-hash-two'), 'ticket:content-hash-two');
  assert.equal(JSON.stringify(h.bridge.capability()).includes('test-key'), false);
  h.bridge.dispose();
});

test('BYOB relay waits for consumer credit and caps chunks at 32 KiB', async () => {
  const h = harness(); await h.capture(); h.bridge.start({ body: '[]' });
  await waitPending(h);
  const first = h.bridge.poll().pending;
  assert.equal(first.sequence, 1); assert.equal(Buffer.from(first.bytes, 'base64').length, 32768);
  await tick(); assert.equal(h.bridge.poll().pending.sequence, 1); assert.equal(h.reads(), 1);
  assert.throws(() => h.bridge.ack(2), /invalid_credit/);
  const output = [];
  for (let sequence = 1; sequence <= 3; ++sequence) {
    const pending = await waitPending(h);
    assert.equal(pending.sequence, sequence);
    output.push(Buffer.from(pending.bytes, 'base64'));
    h.bridge.ack(sequence);
  }
  await waitDone(h);
  const all = Buffer.concat(output);
  assert.equal(all.length, 70000);
  for (let i = 0; i < all.length; ++i) assert.equal(all[i], i % 251);
  assert.equal(h.bridge.poll().done, true); h.bridge.dispose();
});

test('abort releases pending credit without reading the remaining response', async () => {
  const h = harness(); await h.capture(); h.bridge.start({ body: '[]' }); await waitPending(h);
  h.bridge.abort(); await waitDone(h);
  assert.equal(h.bridge.poll().done, true); assert.equal(h.bridge.poll().pending, null); assert.equal(h.reads(), 1); h.bridge.dispose();
});

test('ListModels uses captured RPC endpoint and returns only real response rows', async () => {
  const h = harness();
  await h.context.fetch(modelsUrl, { method: 'POST', headers: { 'x-goog-api-key': 'page-test-key', authorization: 'SAPISIDHASH synthetic-discovery' }, body: '[]' });
  const models = await h.bridge.discoverModels('0');
  assert.equal(models.length, 1); assert.equal(models[0].id, 'live-model');
  assert.equal(models[0].source, 'live'); assert.equal(models[0].inputTokenLimit, 123);
  h.bridge.dispose();
});

test('missing live service has no synthetic snapshot fallback', async () => {
  const h = harness({ fastDiscoveryWait: true });
  await assert.rejects(h.bridge.snapshot('content'), /browser_incompatible/);
  await assert.rejects(h.bridge.discoverModels('0'), /no live ListModels request captured/);
  h.bridge.dispose();
});

test('XHR discovery works without generation, binds verified slot and keeps headers private', async () => {
  let request;
  const h = harness({ discoveryFetch: async (url, init) => {
    request = { url, init };
    return new Response('[[["models/xhr-model"]]]');
  } });
  captureModelsXHR(h, { 'X-Goog-Api-Key': 'page-only-key', 'Content-Type': 'application/json+protobuf' });
  assert.equal(h.bridge.capability().template, null);
  assert.equal(h.bridge.capability().discovery, true);
  assert.equal(JSON.stringify(h.bridge.capability()).includes('page-only-key'), false);
  const models = await h.bridge.discoverModels('2');
  assert.equal(models[0].id, 'xhr-model');
  assert.equal(request.url, modelsUrl);
  assert.equal(request.init.method, 'POST');
  assert.equal(request.init.body, '[]');
  assert.equal(request.init.credentials, 'include');
  assert.equal(request.init.headers['x-goog-authuser'], '2');
  assert.equal(request.init.headers['x-goog-api-key'], 'page-only-key');
  h.bridge.dispose();
});

test('discovery waits briefly for actual XHR and never substitutes a generation template', async () => {
  const h = harness({ fastDiscoveryWait: true });
  setTimeout(() => captureModelsXHR(h), 5);
  assert.equal((await h.bridge.discoverModels('0'))[0].id, 'live-model');
  h.bridge.dispose();
  const generation = harness({ fastDiscoveryWait: true });
  await generation.capture();
  await assert.rejects(generation.bridge.discoverModels('0'), /no live ListModels request captured/);
  generation.bridge.dispose();
});

test('discovery rejects account conflicts before sending any request', async () => {
  for (const [headers, url] of [
    [{ 'X-Goog-AuthUser': '1' }, modelsUrl],
    [{ 'x-goog-authuser': '0, 1' }, modelsUrl],
    [{}, modelsUrl + '?authuser=1'],
    [{}, modelsUrl + '?authuser=0&authuser=1']
  ]) {
    const h = harness(); captureModelsXHR(h, headers, url);
    await assert.rejects(h.bridge.discoverModels('0'), /identity_mismatch/);
    assert.equal(h.fetches(), 0); h.bridge.dispose();
  }
  const h = harness();
  await assert.rejects(h.bridge.discoverModels('../0'), /invalid discovery auth_user/);
  h.bridge.dispose();
});

test('only actual POST ListModels on the exact HTTPS service can supply discovery credentials', async () => {
  for (const url of [modelsUrl.replace('https:', 'http:'), modelsUrl.replace('/$rpc/', '/other/'), modelsUrl.replace('.com/', '.com.evil/')]) {
    const h = harness({ fastDiscoveryWait: true }); captureModelsXHR(h, {}, url);
    assert.equal(h.bridge.capability().discovery, false);
    await assert.rejects(h.bridge.discoverModels('0'), /no live ListModels request captured/);
    h.bridge.dispose();
  }
  const h = harness({ fastDiscoveryWait: true });
  await h.context.fetch(modelsUrl);
  assert.equal(h.bridge.capability().discovery, false); h.bridge.dispose();
});

test('discovery bounds responses and classifies HTTP, wire and timeout failures', async () => {
  for (const [respond, expected] of [
    [() => new Response('private error', { status: 403 }), /ListModels_http_403/],
    [() => new Response('x'.repeat(1024 * 1024 + 1)), /ListModels_response_too_large/],
    [() => new Response('invalid'), /invalid ListModels JSON/],
    [() => new Response('{}'), /unknown ListModels wire shape/],
    [() => { const error = new Error('private'); error.name = 'AbortError'; throw error; }, /ListModels_timeout/]
  ]) {
    const h = harness({ discoveryFetch: respond }); captureModelsXHR(h);
    await assert.rejects(h.bridge.discoverModels('0'), expected); h.bridge.dispose();
  }
});

test('discovery request deadline actively aborts a stalled upstream fetch', async () => {
  let aborted = false;
  const h = harness({ fastDiscoveryTimeout: true, discoveryFetch: (url, init) => new Promise((resolve, reject) => {
    init.signal.addEventListener('abort', () => {
      aborted = true;
      const error = new Error('aborted'); error.name = 'AbortError'; reject(error);
    }, { once: true });
  }) });
  captureModelsXHR(h);
  await assert.rejects(h.bridge.discoverModels('0'), /ListModels_timeout/);
  assert.equal(aborted, true); h.bridge.dispose();
});

test('configured relay credit can be smaller than 32 KiB', async () => {
  const h = harness(); await h.capture(); h.bridge.start({ body: '[]', chunkBytes: 4096 }); await waitPending(h);
  assert.equal(Buffer.from(h.bridge.poll().pending.bytes, 'base64').length, 4096);
  assert.equal(h.reads(), 1); h.bridge.abort(); await waitDone(h); h.bridge.dispose();
});

test('identity probe reports ambiguity and Google sign-in redirects', () => {
  const h = harness();
  const element = email => ({ getAttribute: name => name === 'data-email' ? email : null });
  h.context.document.querySelectorAll = () => [element('User@Example.com')];
  assert.equal(h.bridge.identity().email, 'user@example.com');
  h.context.document.querySelectorAll = () => [element('first@example.com'), element('other@example.com')];
  assert.equal(h.bridge.identity().email, ''); assert.equal(h.bridge.identity().identityAmbiguous, true);
  h.context.location.href = 'https://accounts.google.com/ServiceLogin';
  h.context.location.hostname = 'accounts.google.com';
  assert.equal(h.bridge.identity().login, true);
  assert.equal(h.bridge.identity().ready, false); h.bridge.dispose();
});

test('identity probe reports an AI Studio terms dialog and nothing else', () => {
  const h = harness();
  const link = href => ({ getAttribute: name => name === 'href' ? href : null });
  const modal = ({ text = '', links = [], shown = true } = {}) => ({
    innerText: text, clicks: 0, getClientRects: () => shown ? [{}] : [],
    querySelectorAll: selector => selector === 'a[href]' ? links : selector === 'button' ? [control('Continue')] : []
  });
  let dialogs = [];
  h.context.document.querySelectorAll = selector => selector.includes('[role="dialog"]') ? dialogs : [];
  assert.equal(h.bridge.identity().termsRequired, false);
  dialogs = [modal({ text: 'What is new in AI Studio', links: [link('https://blog.google/technology/')] })];
  assert.equal(h.bridge.identity().termsRequired, false);
  dialogs = [modal({ text: 'Welcome to AI Studio', links: [link('https://ai.google.dev/gemini-api/terms')] })];
  assert.equal(h.bridge.identity().termsRequired, true);
  dialogs = [modal({ text: '继续即表示您同意 Gemini API 附加服务条款' })];
  assert.equal(h.bridge.identity().termsRequired, true);
  dialogs = [modal({ links: [link('https://policies.google.com/terms?hl=en')] })];
  assert.equal(h.bridge.identity().termsRequired, true);
  // Look-alike hosts, other paths and closed dialogs are not the terms modal.
  dialogs = [modal({ links: [link('https://google.com.example.com/terms'), link('https://policies.google.com/termsfeed')] }),
             modal({ text: 'Additional Terms of Service', shown: false })];
  assert.equal(h.bridge.identity().termsRequired, false);
  h.bridge.dispose();
});

test('a textarea on any other origin cannot make the account ready', () => {
  const h = harness();
  assert.equal(h.bridge.identity().ready, true);
  for (const hostname of ['accounts.google.com', 'example.com', 'aistudio.google.com.example.com']) {
    h.context.location.href = `https://${hostname}/`;
    h.context.location.hostname = hostname;
    assert.equal(h.bridge.identity().ready, false);
  }
  h.context.location.href = 'https://aistudio.google.com/prompts/new_chat';
  h.context.location.hostname = 'aistudio.google.com';
  assert.equal(h.bridge.identity().ready, true);
  h.context.document.readyState = 'loading';
  assert.equal(h.bridge.identity().ready, false);
  h.bridge.dispose();
});

test('account import verifies identity independently of the prompt editor', () => {
  const h = harness();
  h.context.document.querySelector = () => null;
  const badge = { getAttribute: name => ({ href:'https://accounts.google.com/SignOutOptions', 'aria-label':'Google Account: User (user@example.com)' })[name] ?? null };
  const content = { getAttribute: name => name === 'data-email' ? 'unrelated@example.com' : null };
  h.context.document.querySelectorAll = selector => selector === 'a[href]' ? [badge] : [badge, content];
  const result = h.bridge.identity();
  assert.equal(result.identityReady, true); assert.equal(result.ready, false);
  assert.equal(result.email, 'user@example.com'); assert.equal(result.identityAmbiguous, false);
  h.context.location.href = 'https://accounts.google.com/ServiceLogin';
  assert.equal(h.bridge.identity().identityReady, false); h.bridge.dispose();
});

test('hidden account menu entries do not override a visible identity and ambiguity fails closed', () => {
  const h = harness();
  const account = (email, hidden = false) => ({ hidden, getAttribute: name => name === 'data-email' ? email : null });
  let entries = [account('one@example.com'), account('two@example.com', true)];
  h.context.document.querySelectorAll = selector => selector === 'a[href]' ? [] : entries;
  assert.equal(h.bridge.identity().identityReady, true);
  assert.equal(h.bridge.identity().email, 'one@example.com');
  entries[1].hidden = false;
  assert.equal(h.bridge.identity().identityReady, false);
  assert.equal(h.bridge.identity().identityAmbiguous, true);
  assert.equal(h.bridge.identity().email, ''); h.bridge.dispose();
});

test('non-200 metadata preserves Retry-After and bounds private error text', async () => {
  const h = harness({ httpStatus: 429, retryAfter: '75', errorBody: 'x'.repeat(20000) });
  await h.capture(); h.bridge.start({ body: '[]' }); await waitDone(h);
  const result = h.bridge.poll();
  assert.equal(result.status, 429); assert.equal(result.retryAfter, 75);
  assert.equal(result.errorBody.length, 8192); assert.equal(result.done, true);
  assert.equal(result.pending, null); assert.equal(h.reads(), 1); h.bridge.dispose();
});

test('HTTP 200 RPC error body is relayed intact for native status classification', async () => {
  const wire = '[[[16,"authentication expired"]]]';
  const h = harness({ errorBody: wire, retryAfter: '30' });
  await h.capture(); h.bridge.start({ body: '[]' }); await waitPending(h);
  const result = h.bridge.poll();
  assert.equal(result.status, 200); assert.equal(result.retryAfter, 30);
  assert.equal(Buffer.from(result.pending.bytes, 'base64').toString(), wire);
  h.bridge.abort(); await waitDone(h); h.bridge.dispose();
});

const generationUrl = modelsUrl.replace('/ListModels', '/GenerateContent');
test('XHR captures its live template and private headers without sending bootstrap upstream', async () => {
  let request;
  const h = harness({ observeFetch: (input, init) => { request = { input, init }; } });
  const xhr = new h.context.XMLHttpRequest();
  const events = [];
  for (const type of ['readystatechange', 'load', 'loadend']) xhr.addEventListener(type, () => events.push(type + ':' + xhr.readyState));
  xhr.open('POST', generationUrl);
  xhr.setRequestHeader('X-Test-Page', 'private-header');
  xhr.setRequestHeader('Authorization', 'SAPISIDHASH synthetic-xhr');
  xhr.send('["models/actual-page-model",[[[[null,"probe"]],"user"]],null,null,"actual-ticket"]');
  assert.equal(xhr.sent, undefined);
  await tick();
  assert.equal(xhr.status, 200); assert.equal(xhr.responseText, '[]'); assert.equal(xhr.response, '[]');
  assert.equal(xhr.getResponseHeader('Content-Type'), 'application/json+protobuf');
  assert.deepEqual(events, ['readystatechange:2', 'readystatechange:3', 'readystatechange:4', 'load:4', 'loadend:4']);
  const capability = h.bridge.capability();
  assert.equal(capability.templateTransport, 'xhr');
  assert.equal(capability.template.body[0], 'models/actual-page-model');
  assert.equal(JSON.stringify(capability).includes('private-header'), false);
  await h.context.default_MakerSuite.adapter({ snapshot: () => 'actual-ticket' }, 'a'.repeat(64));
  // Observe the replay parameters through the existing native-fetch test seam.
  h.context.fetch = () => { throw new Error('bridge must use its captured native transport'); };
  h.bridge.start({ body: '[]' }); await waitPending(h);
  assert.equal(h.fetches(), 1);
  assert.equal(request.input, generationUrl);
  assert.equal(request.init.headers['x-test-page'], 'private-header');
  h.bridge.abort(); h.bridge.dispose();
});

test('XHR prompt saves are acknowledged locally, including JSON responses and property listeners', async () => {
  const h = harness();
  for (const action of ['SavePrompt', 'CreatePrompt']) {
    const xhr = new h.context.XMLHttpRequest(); let loaded = 0;
    xhr.open('POST', generationUrl.replace('GenerateContent', action)); xhr.responseType = 'json';
    xhr.onload = () => { ++loaded; };
    xhr.send('[]'); await tick();
    assert.equal(xhr.sent, undefined); assert.equal(loaded, 1); assert.equal(xhr.status, 200);
    assert.equal(JSON.stringify(xhr.response), '[]');
    assert.throws(() => xhr.responseText, { name: 'InvalidStateError' });
  }
  assert.equal(h.bridge.capability().template, null); assert.equal(h.fetches(), 0); h.bridge.dispose();
});

test('intercepted XHR abort cancels completion and reopening restores native response views', async () => {
  const h = harness(); const xhr = new h.context.XMLHttpRequest(); let loaded = false, aborted = false;
  xhr.open('POST', generationUrl); xhr.onload = () => { loaded = true; }; xhr.onabort = () => { aborted = true; };
  xhr.send('[]'); xhr.abort(); await tick();
  assert.equal(loaded, false); assert.equal(aborted, true); assert.equal(xhr.status, 0);
  xhr.open('POST', modelsUrl); xhr.send('[]');
  assert.equal(xhr.sent, true); assert.equal(Object.hasOwn(xhr, 'responseText'), false); h.bridge.dispose();
});

test('snapshot adapter discovery skips getters and immutable exports and preserves receiver, arguments and return identity', async () => {
  let getters = 0;
  const exports = {
    tag: 'receiver',
    direct(service, hash, extra) { return service.snapshot({ content: hash, receiver: this.tag, extra }); },
    indirect(service, hash) { return Promise.resolve(service.snapshot).then(fn => fn(hash)); }
  };
  Object.defineProperty(exports, 'getter', { enumerable: true, get() { ++getters; throw new Error('must not evaluate'); } });
  Object.defineProperty(exports, 'locked', { enumerable: true, value: function(service, hash) { return service.snapshot(hash); } });
  const h = harness({ exports });
  assert.equal(getters, 0); assert.equal(h.bridge.capability().snapshotCandidates, 3); assert.equal(h.bridge.capability().snapshotHooked, 2);
  const object = { opaque: true }; let input;
  const service = { snapshot(value) { input = value; return object; } };
  assert.equal(exports.direct(service, 'seed', 17), object);
  assert.equal(input.receiver, 'receiver'); assert.equal(input.extra, 17);
  service.snapshot = value => { input = value; return 'fresh-ticket'; };
  exports.direct(service, 'a'.repeat(64), 17);
  const xhr = new h.context.XMLHttpRequest(); xhr.open('POST', generationUrl); xhr.send('[null,null,null,null,"fresh-ticket"]');
  assert.equal(await h.bridge.snapshot('fresh'), 'fresh-ticket');
  assert.equal(input.content, 'fresh'); assert.equal(input.receiver, 'receiver');
  assert.equal(input.extra, 17);
  const sentinel = Promise.resolve('promise-ticket');
  assert.equal(exports.direct({ snapshot: () => sentinel }, 'seed'), sentinel);
  const promise = exports.indirect({ snapshot: hash => 'async:' + hash }, 'next');
  assert.equal(await promise, 'async:next');
  assert.equal(await h.bridge.snapshot('later'), 'fresh-ticket', 'background adapters cannot replace the selected generation service');
  h.bridge.dispose();
});


test('snapshot export proxies preserve construction, prototypes, metadata and static behavior', () => {
  class SnapshotClient {
    static token = 'static-token';
    constructor(service) { this.service = service; this.target = new.target; }
    getSnapshot(hash) { return this.service.snapshot(hash); }
  }
  function RelatedClient(service, extra) { this.service = service; this.extra = extra; this.target = new.target; }
  RelatedClient.prototype.run = function(hash) { return this.service.snapshot(hash); };
  // Its body contains the production-style snapshot reference, while the export
  // remains a constructor rather than the two-argument signing adapter.
  function SnapshotFactory(service) { this.service = service; this.target = new.target; this.method = service.snapshot; if (!this.method) return service.snapshot(); }
  SnapshotFactory.staticValue = { marker: true };
  const exports = { SnapshotClient, RelatedClient, SnapshotFactory };
  const h = harness({ exports });
  assert.notEqual(exports.SnapshotClient, SnapshotClient);
  assert.notEqual(exports.SnapshotFactory, SnapshotFactory);
  for (const [key, original] of [['SnapshotClient', SnapshotClient], ['SnapshotFactory', SnapshotFactory]]) {
    const proxy = exports[key];
    assert.equal(proxy.prototype, original.prototype); assert.equal(proxy.name, original.name); assert.equal(proxy.length, original.length);
    assert.deepEqual(Object.getOwnPropertyNames(proxy), Object.getOwnPropertyNames(original));
    const instance = new proxy({ snapshot: hash => hash });
    assert.equal(instance instanceof original, true); assert.equal(instance instanceof proxy, true); assert.equal(instance.target, proxy);
    const alternateTarget = function Alternate() {};
    const alternate = Reflect.construct(proxy, [{ snapshot: hash => hash }], alternateTarget);
    assert.equal(Object.getPrototypeOf(alternate), alternateTarget.prototype); assert.equal(alternate.target, alternateTarget);
  }
  assert.equal(exports.SnapshotClient.token, 'static-token'); assert.equal(exports.SnapshotFactory.staticValue, SnapshotFactory.staticValue);
  assert.throws(() => exports.SnapshotClient({ snapshot() {} }), TypeError);
  const before = exports.SnapshotClient; h.context.install(); assert.equal(exports.SnapshotClient, before);
  h.bridge.dispose();
});

test('snapshot observation cannot turn unusual return values into page failures', () => {
  let current;
  const exports = { adapter(service) { if (service.snapshot) return current; } };
  const h = harness({ exports });
  current = Object.defineProperty({}, 'then', { get() { throw new Error('getter failure'); } });
  assert.equal(exports.adapter({ snapshot: true }), current);
  current = { then() { throw new Error('then failure'); } };
  assert.equal(exports.adapter({ snapshot: true }), current);
  const failure = new Error('original failure');
  const service = Object.defineProperty({}, 'snapshot', { get() { throw failure; } });
  assert.throws(() => exports.adapter(service), error => error === failure);
  h.bridge.dispose();
});

test('bootstrap restores the original editor and stops the probe', () => {
  const h = harness();
  class Textarea extends EventTarget { constructor() { super(); this.original = 'existing draft'; } }
  Object.defineProperty(Textarea.prototype, 'value', { get() { return this.original; }, set(value) { this.original = value; } });
  const input = new Textarea(); let stopped = 0, temporary = 0;
  h.context.HTMLTextAreaElement = Textarea;
  h.context.document.querySelector = () => input;
  h.context.document.querySelectorAll = selector => selector !== 'button' ? [] : [
    { disabled: false, innerText: 'Temporary chat', getAttribute: name => name === 'aria-pressed' ? 'false' : null, click() { ++temporary; } },
    { disabled: false, innerText: 'Stop', getAttribute: () => null, click() { ++stopped; } }
  ];
  assert.equal(h.bridge.bootstrap(), true);
  assert.equal(input.value, 'Reply with the single digit 1.');
  assert.equal(temporary, 1);
  assert.equal(h.bridge.finishBootstrap(), true);
  assert.equal(input.value, 'existing draft');
  assert.equal(stopped, 1);
  h.bridge.finishBootstrap();
  assert.equal(stopped, 1);
  h.bridge.dispose();
});

test('a repeated bootstrap drops the stale template and captures a fresh one', async () => {
  const h = harness();
  class Textarea extends EventTarget { constructor() { super(); this.original = ''; } }
  Object.defineProperty(Textarea.prototype, 'value', { get() { return this.original; }, set(value) { this.original = value; } });
  h.context.HTMLTextAreaElement = Textarea;
  const input = new Textarea();
  h.context.document.querySelector = () => input;
  await h.capture();
  assert.equal(h.bridge.capability().snapshot, true);
  assert.equal(h.bridge.bootstrap(), true);
  // The host polls capability() until a new capture; the old one must not count.
  assert.equal(h.bridge.capability().template, null);
  assert.equal(h.bridge.capability().snapshot, false);
  await assert.rejects(h.bridge.snapshot('content-hash'), /snapshot service or live template unavailable/);
  await h.capture();
  assert.equal(h.bridge.capability().snapshot, true);
  assert.equal(await h.bridge.snapshot('content-hash'), 'ticket:content-hash');
  h.bridge.finishBootstrap();
  assert.equal(h.fetches(), 0, 'bootstrap generation must not reach upstream');
  h.bridge.dispose();
});

function flakySnapshots() {
  // The page's snapshot export fails while `failures` is positive.
  const control = { failures: 0, calls: 0 };
  control.exports = { adapter(service, hash) {
    ++control.calls;
    if (control.failures > 0) { --control.failures; return control.mode === 'empty' ? Promise.resolve('') : Promise.reject(new Error('synthetic busy page')); }
    return service.snapshot(hash);
  } };
  return control;
}

test('snapshot retries a failing or empty page snapshot up to three times', async () => {
  for (const mode of ['reject', 'empty']) {
    const control = flakySnapshots(); control.mode = mode;
    const h = harness({ exports: control.exports }); await h.capture();
    control.calls = 0; control.failures = 2;
    assert.equal(await h.bridge.snapshot('content-hash'), 'ticket:content-hash', mode);
    assert.equal(control.calls, 3, mode);
    control.calls = 0; control.failures = 3;
    await assert.rejects(h.bridge.snapshot('content-hash'), /snapshot service returned no ticket/);
    assert.equal(control.calls, 3, mode);
    h.bridge.dispose();
  }
});

function page(h, { buttons = [], dialogs = [] } = {}) {
  class Textarea extends EventTarget { constructor() { super(); this.value = ''; this.events = []; } focus() { this.focused = true; } dispatchEvent(event) { this.events.push(event); return super.dispatchEvent(event); } }
  const input = new Textarea();
  h.context.HTMLTextAreaElement = class {};
  Object.defineProperty(h.context.HTMLTextAreaElement.prototype, 'value', { set(value) { this._value = value; }, get() { return this._value; } });
  h.context.document.querySelector = selector => selector === 'textarea' ? input : null;
  h.context.document.querySelectorAll = selector => selector === 'button' ? [...buttons, ...dialogs.flatMap(d => d.buttons)] :
    selector.includes('[role="dialog"]') ? dialogs : [];
  return input;
}
function control(text, extra = {}) {
  return { disabled: false, innerText: text, clicks: 0, getAttribute: name => extra[name] ?? null, click() { ++this.clicks; } };
}
function dialog(buttons) {
  return { buttons, events: [], querySelectorAll: selector => selector === 'button' ? buttons : [], dispatchEvent(event) { this.events.push(event); return true; } };
}

test('bootstrap dismisses blocking dialogs without agreeing to anything', () => {
  const h = harness();
  const notice = dialog([control('Learn more'), control('Got it')]);
  const terms = dialog([control('I agree'), control('Continue'), control('Accept all')]);
  const run = control('Run Ctrl+Enter');
  page(h, { buttons: [run], dialogs: [notice, terms] });
  assert.equal(h.bridge.bootstrap(), true);
  assert.equal(notice.buttons[1].clicks, 1);
  assert.equal(notice.buttons[0].clicks, 0);
  // Consent-like buttons are never pressed; Escape is the only fallback.
  for (const button of terms.buttons) assert.equal(button.clicks, 0, button.innerText);
  assert.equal(terms.events.length, 1);
  assert.equal(terms.events[0].key, 'Escape');
  assert.equal(h.bridge.run(), true);
  assert.equal(run.clicks, 1);
  h.bridge.finishBootstrap();
  h.bridge.dispose();
});

test('run falls back to the editor submit shortcut when no Run button exists', () => {
  const h = harness();
  const unrelated = control('Share');
  const input = page(h, { buttons: [unrelated, { ...control('Run'), disabled: true }] });
  assert.equal(h.bridge.bootstrap(), true);
  assert.equal(h.bridge.run(), 'shortcut');
  assert.equal(input.focused, true);
  assert.equal(unrelated.clicks, 0);
  const keys = input.events.filter(event => event.key === 'Enter');
  assert.deepEqual(keys.map(event => event.type), ['keydown', 'keyup']);
  for (const event of keys) assert.ok(event.ctrlKey || event.metaKey);
  // Without an editor there is nothing to submit.
  h.context.document.querySelector = () => null;
  assert.equal(h.bridge.run(), false);
  h.bridge.finishBootstrap();
  h.bridge.dispose();
});

test('next acknowledges and returns as soon as the relay has a chunk or ends', async () => {
  const h = harness(); await h.capture();
  assert.equal(await h.bridge.next(0, 1000), null, 'no transfer yet');
  h.bridge.start({ body: '[]' });
  const sequences = [];
  let state, sequence = 0, began = Date.now();
  while (!(state = await h.bridge.next(sequence, 1000)).done) {
    assert.ok(state.pending, 'a wait that returns early carries a chunk');
    sequence = state.pending.sequence;
    sequences.push(sequence);
  }
  assert.deepEqual(sequences, [1, 2, 3]);
  assert.equal(state.error, '');
  assert.ok(Date.now() - began < 900, 'chunks must not wait for the full timeout');
  await assert.rejects(h.bridge.next(7, 10), /invalid_credit/);
  h.bridge.dispose();
});

test('next waits at most its limit on a stalled stream and wakes on abort', async () => {
  const h = harness({ stall: true }); await h.capture();
  h.bridge.start({ body: '[]' });
  let began = Date.now();
  let state = await h.bridge.next(0, 80);
  assert.equal(state.pending, null); assert.equal(state.done, false);
  assert.ok(Date.now() - began >= 60);
  began = Date.now();
  const waiting = h.bridge.next(0, 1000);
  await tick();
  h.bridge.abort();
  await waiting;
  assert.ok(Date.now() - began < 900, 'abort must wake the waiting host');
  h.bridge.dispose();
});

test('generation prefers native CDP authentication and rebuilds the verified account header', async () => {
  let sent;
  const h = harness({ observeFetch: (_url, init) => { sent = init; } });
  await h.capture();
  // HttpOnly signing cookies are unavailable to document.cookie.
  Object.defineProperty(h.context.document, 'cookie', { get() { throw new Error('must use native signing'); } });
  const first = 'SAPISIDHASH 1_first SAPISID1PHASH 1_one SAPISID3PHASH 1_three';
  h.bridge.start({ body: '[]', authUser: '2', authorization: first }); await waitPending(h);
  assert.equal(sent.headers.authorization, first);
  assert.equal(sent.headers['x-goog-authuser'], '2');
  h.bridge.abort(); await waitDone(h);
  const second = 'SAPISIDHASH 2_new SAPISID1PHASH 2_new-one SAPISID3PHASH 2_new-three';
  h.bridge.start({ body: '[]', authUser: '2', authorization: second }); await waitPending(h);
  assert.equal(sent.headers.authorization, second);
  assert.equal(JSON.stringify(h.bridge.capability()).includes(second), false);
  assert.equal(JSON.stringify(h.bridge.poll()).includes(second), false);
  h.bridge.abort(); await waitDone(h);
  h.bridge.dispose();
});

test('generation falls back to its captured authorization only when native signing is unavailable', async () => {
  let sent;
  const h = harness({ observeFetch: (_url, init) => { sent = init; } });
  await h.capture();
  h.bridge.start({ body: '[]', authUser: '0', authorization: '' }); await waitPending(h);
  assert.equal(sent.headers.authorization, 'SAPISIDHASH synthetic-captured');
  assert.equal(sent.headers['x-goog-authuser'], '0');
  h.bridge.abort(); await waitDone(h); h.bridge.dispose();

  const missing = harness({ captureHeaders: { authorization: '' } }); await missing.capture();
  missing.bridge.start({ body: '[]', authUser: '0', authorization: '' }); await waitDone(missing);
  assert.equal(missing.fetches(), 0);
  assert.match(missing.bridge.poll().error, /signing cookies and captured Authorization unavailable/);
  missing.bridge.dispose();
});

test('generation rejects captured account conflicts and unexpected origins even with fresh authentication', async () => {
  for (const headers of [{ 'x-goog-authuser': '1' }, { 'x-goog-authuser': '0, 1' }]) {
    const h = harness({ captureHeaders: headers }); await h.capture();
    h.bridge.start({ body: '[]', authUser: '0', authorization: 'SAPISIDHASH fresh' }); await waitDone(h);
    assert.equal(h.fetches(), 0); assert.match(h.bridge.poll().error, /identity_mismatch/);
    h.bridge.dispose();
  }
  const h = harness(); await h.capture(); h.context.location.href = 'https://accounts.google.com/';
  h.bridge.start({ body: '[]', authUser: '0', authorization: 'SAPISIDHASH fresh' }); await waitDone(h);
  assert.equal(h.fetches(), 0); assert.match(h.bridge.poll().error, /unexpected authentication origin/);
  h.bridge.dispose();
});

test('model discovery uses native authentication with captured fallback and rejects missing credentials', async () => {
  let sent;
  const h = harness({ observeFetch: (_url, init) => { sent = init; } });
  captureModelsXHR(h);
  h.context.document.cookie = '';
  await h.bridge.discoverModels('2', 'SAPISIDHASH native-models');
  assert.equal(sent.headers.authorization, 'SAPISIDHASH native-models');
  assert.equal(sent.headers['x-goog-authuser'], '2');
  await h.bridge.discoverModels('2', '');
  assert.equal(sent.headers.authorization, 'SAPISIDHASH synthetic-discovery');
  h.bridge.dispose();

  const missing = harness();
  await missing.context.fetch(modelsUrl, { method: 'POST', body: '[]' });
  const count = missing.fetches();
  await assert.rejects(missing.bridge.discoverModels('0', ''), /signing cookies and captured Authorization unavailable/);
  assert.equal(missing.fetches(), count); missing.bridge.dispose();
});

test('benefit tier parses exact JSPB enums and rebuilds headers for generation and discovery', async () => {
  for (const [body, tier, header] of [['[]', 'free'], ['[null]', 'free'], ['[0]', 'free'], [")]}'\n[1]", 'pro', 'TIER1'], ['[2]', 'ultra', 'TIER2'], ['[3]', 'plus', 'TIER0']]) {
    let sent, queried;
    const h = harness({ captureHeaders: { 'X-AIStudio-G1-Tier': 'STALE' },
      tierFetch: (url, init) => { queried = { url, init }; return new Response(body); },
      observeFetch: (_url, init) => { sent = init; } });
    await h.capture();
    h.bridge.start({ body: '[]', authUser: '0', authorization: 'SAPISIDHASH native' });
    await waitPending(h);
    assert.match(queried.url, /MakerSuiteService\/GetAiStudioBenefitTier$/);
    assert.equal(queried.init.headers.authorization, 'SAPISIDHASH native');
    assert.equal(queried.init.headers['x-aistudio-g1-tier'], undefined);
    assert.equal(queried.init.credentials, 'include'); assert.equal(queried.init.body, '[]');
    assert.equal(sent.headers['x-aistudio-g1-tier'], header);
    assert.equal(h.bridge.capability().benefitTier, tier);
    h.bridge.abort(); await waitDone(h);
    captureModelsXHR(h, { 'x-aistudio-g1-tier': 'STALE' });
    await h.bridge.discoverModels('0', 'SAPISIDHASH newer');
    assert.equal(sent.headers['x-aistudio-g1-tier'], header);
    assert.equal(sent.headers.authorization, 'SAPISIDHASH newer');
    assert.equal(h.tierFetches(), 1); h.bridge.dispose();
  }
});

test('benefit cache expires at 600 seconds and stays isolated between pages', async () => {
  let now = 1000, code = 1, sent;
  const h = harness({ now: () => now, tierFetch: () => new Response(JSON.stringify([code])), observeFetch: (_url, init) => { sent = init; } });
  captureModelsXHR(h);
  await h.bridge.discoverModels('0');
  now += 599999; code = 2;
  await h.bridge.discoverModels('0'); assert.equal(h.tierFetches(), 1);
  assert.equal(sent.headers['x-aistudio-g1-tier'], 'TIER1');
  now += 1; assert.equal(h.bridge.capability().benefitTier, 'unknown');
  await h.bridge.discoverModels('0'); assert.equal(h.tierFetches(), 2);
  assert.equal(sent.headers['x-aistudio-g1-tier'], 'TIER2');
  await assert.rejects(h.bridge.discoverModels('1'), /identity_mismatch/);
  h.bridge.dispose();
  const other = harness(); captureModelsXHR(other); await other.bridge.discoverModels('0');
  assert.equal(other.bridge.capability().benefitTier, 'free'); assert.equal(other.tierFetches(), 1); other.bridge.dispose();
});

test('benefit failures stay unknown, discard old tier headers, and suppress retries for 30 seconds', async () => {
  for (const response of ['[true]', '["1"]', '[4]', '{}', 'secret-body-invalid', ' '.repeat(8193)]) {
    let now = 1000, sent;
    const h = harness({ now: () => now, tierFetch: () => new Response(response), observeFetch: (_url, init) => { sent = init; } });
    captureModelsXHR(h, { 'X-AIStudio-G1-Tier': 'TIER2' });
    await h.bridge.discoverModels('0');
    assert.equal(h.bridge.capability().benefitTier, 'unknown'); assert.equal(sent.headers['x-aistudio-g1-tier'], undefined);
    now += 29999; await h.bridge.discoverModels('0'); assert.equal(h.tierFetches(), 1);
    now += 1; await h.bridge.discoverModels('0'); assert.equal(h.tierFetches(), 2);
    assert.equal(JSON.stringify(h.bridge.capability()).includes('secret-body'), false); h.bridge.dispose();
  }
});

test('benefit lookup is single flight and cancellation prevents generation without retry', async () => {
  let release;
  const h = harness({ tierFetch: () => new Promise(resolve => { release = resolve; }) });
  captureModelsXHR(h);
  const first = h.bridge.discoverModels('0');
  const second = h.bridge.discoverModels('0');
  assert.equal(h.tierFetches(), 1); release(new Response('[3]'));
  await Promise.all([first, second]); assert.equal(h.tierFetches(), 1); h.bridge.dispose();
  const cancelled = harness({ tierFetch: (_url, init) => new Promise((_, reject) => init.signal.addEventListener('abort', () => reject(new DOMException('private upstream detail', 'AbortError')), { once: true })) });
  await cancelled.capture(); cancelled.bridge.start({ body: '[]' });
  await waitFor(() => cancelled.tierFetches() === 1, 'benefit request');
  cancelled.bridge.abort(); await waitDone(cancelled);
  assert.equal(cancelled.fetches(), 0); assert.equal(cancelled.tierFetches(), 1);
  assert.equal(cancelled.bridge.poll().error, 'cancelled'); cancelled.bridge.dispose();
});

test('benefit timeout and HTTP failure allow requests without disclosing response details', async () => {
  for (const timeout of [false, true]) {
    const h = harness({ fastBenefitTimeout: true, tierFetch: (_url, init) => timeout
      ? new Promise((_, reject) => init.signal.addEventListener('abort', () => reject(new DOMException('private token', 'AbortError')), { once: true }))
      : new Response('private token', { status: 403 }) });
    await h.capture(); h.bridge.start({ body: '[]' }); await waitPending(h);
    assert.equal(h.bridge.capability().benefitTier, 'unknown'); assert.equal(h.tierFetches(), 1);
    assert.equal(JSON.stringify(h.bridge.poll()).includes('private token'), false);
    h.bridge.abort(); await waitDone(h); h.bridge.dispose();
  }
});

test('a cancelled generation stops waiting on a shared discovery tier query', async () => {
  let release;
  const h = harness({ tierFetch: () => new Promise(resolve => { release = resolve; }) });
  await h.capture(); captureModelsXHR(h);
  const discovery = h.bridge.discoverModels('0');
  h.bridge.start({ body: '[]' }); h.bridge.abort(); await waitDone(h);
  assert.equal(h.bridge.poll().error, 'cancelled');
  assert.equal(h.fetches(), 0); assert.equal(h.tierFetches(), 1);
  release(new Response('[1]')); await discovery; h.bridge.dispose();
});

test('relay records benefit tier at send time even after cache expiry or refresh', async () => {
  let now = 1000, code = 1;
  const h = harness({ now: () => now, tierFetch: () => new Response(JSON.stringify([code])) });
  await h.capture(); h.bridge.start({ body: '[]' }); await waitPending(h);
  assert.equal(h.bridge.poll().benefitTier, 'pro');
  now += 600000;
  assert.equal(h.bridge.capability().benefitTier, 'unknown');
  assert.equal(h.bridge.poll().benefitTier, 'pro');
  code = 2; captureModelsXHR(h); await h.bridge.discoverModels('0');
  assert.equal(h.bridge.capability().benefitTier, 'ultra');
  assert.equal(h.bridge.poll().benefitTier, 'pro');
  h.bridge.abort(); await waitDone(h); h.bridge.dispose();
  const unknown = harness({ tierFetch: () => new Response('[9]') });
  await unknown.capture(); unknown.bridge.start({ body: '[]' }); await waitPending(unknown);
  assert.equal(unknown.bridge.poll().benefitTier, 'unknown');
  unknown.bridge.abort(); await waitDone(unknown); unknown.bridge.dispose();
});

test('uploaded pieces reach upstream as one Blob with the captured content type', async () => {
  let sent;
  const h = harness({ observeFetch: (_url, init) => { sent = init; } });
  await h.capture();
  const pieces = ['["你好', ' 😀', 'x'.repeat(70000) + '"]'];
  pieces.forEach((piece, index) => assert.equal(h.bridge.append('upload-1', piece), index + 1));
  h.bridge.start({ upload: 'upload-1' }); await waitPending(h);
  assert.ok(sent.body instanceof Blob, 'the body must stay a Blob, not a page string');
  assert.equal(await sent.body.text(), pieces.join(''));
  assert.equal(sent.body.size, Buffer.byteLength(pieces.join('')));
  assert.equal(sent.headers['content-type'], 'application/json+protobuf');
  h.bridge.abort(); await waitDone(h); h.bridge.dispose();
});

test('an uploaded body gets the same default content type as a string body', async () => {
  const seen = [];
  const h = harness({ observeFetch: (_url, init) => { seen.push(init); } });
  // An XHR template carries only the headers the page set: here no type.
  const xhr = new h.context.XMLHttpRequest();
  xhr.open('POST', generationUrl);
  xhr.setRequestHeader('Authorization', 'SAPISIDHASH synthetic-xhr');
  xhr.send('["models/actual-page-model",null,null,null,"actual-ticket"]');
  await tick();
  await h.context.default_MakerSuite.adapter({ snapshot: () => 'actual-ticket' }, 'a'.repeat(64));
  h.bridge.append('upload-2', '[]');
  h.bridge.start({ upload: 'upload-2' }); await waitPending(h);
  assert.equal(seen[0].headers['content-type'], 'text/plain;charset=UTF-8');
  // For a string body fetch adds the same type itself.
  h.bridge.start({ body: '[]' }); await waitPending(h);
  assert.equal('content-type' in seen[1].headers, false);
  assert.equal(new Request(generationUrl, { method: 'POST', body: '[]' }).headers.get('content-type'), 'text/plain;charset=UTF-8');
  h.bridge.abort(); await waitDone(h); h.bridge.dispose();
});

test('a body upload is used once and never mixes pieces from another upload', async () => {
  const h = harness(); await h.capture();
  assert.throws(() => h.bridge.append('', 'x'), /invalid upload chunk/);
  assert.throws(() => h.bridge.append('upload-3', 7), /invalid upload chunk/);
  assert.throws(() => h.bridge.start({ upload: 'upload-3' }), /upload missing/);
  h.bridge.append('stale', 'old');
  h.bridge.append('upload-4', 'new');
  assert.throws(() => h.bridge.start({ upload: 'stale' }), /upload missing/, 'a new upload replaces unfinished pieces');
  h.bridge.append('upload-5', '[]');
  h.bridge.abort();
  assert.throws(() => h.bridge.start({ upload: 'upload-5' }), /upload missing/, 'abort drops a pending upload');
  h.bridge.append('upload-6', '[]');
  h.bridge.start({ upload: 'upload-6' }); await waitPending(h);
  h.bridge.abort(); await waitDone(h);
  assert.throws(() => h.bridge.start({ upload: 'upload-6' }), /upload missing/, 'an upload is consumed by start');
  h.bridge.dispose();
});
