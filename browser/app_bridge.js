// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

/* ais2api Build App bridge. Runs in the Build App preview frame, whose calls to
   the public Gemini API AI Studio completes with the account's credentials.
   No credentials or response bodies are persisted. */
(() => {
  if (globalThis.__ais2app) return;
  const fail = message => { throw new Error(message); };
  const api = 'https://generativelanguage.googleapis.com';
  // AI Studio may install its credentialed fetch after this script runs, so
  // look the page's fetch up on every call instead of keeping a reference.
  const pageFetch = (input, init) => globalThis.fetch(input, init);
  const modelId = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/;
  const state = { transfer: null };
  // Reads at most `limit` bytes of a response body as text; the rest of the
  // body is cancelled when `truncate` is set and rejected otherwise.
  async function boundedText(response, limit, truncate) {
    if (!response.body) return '';
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let text = '', size = 0;
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        const room = limit - size;
        if (value.byteLength > room) {
          if (!truncate) fail('response_too_large');
          text += decoder.decode(value.subarray(0, room), { stream: true });
          break;
        }
        size += value.byteLength;
        text += decoder.decode(value, { stream: true });
      }
      return text + decoder.decode();
    } finally { try { await reader.cancel(); } catch (_) {} }
  }
  function ready() {
    return { href: location.href, fetch: typeof globalThis.fetch === 'function' };
  }
  async function models() {
    const result = [];
    let token = '';
    for (let page = 0; page < 10; ++page) {
      const url = new URL('/v1beta/models', api);
      url.searchParams.set('pageSize', '1000');
      if (token) url.searchParams.set('pageToken', token);
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 10000);
      let raw;
      try {
        const response = await pageFetch(url.href, { method: 'GET', signal: controller.signal });
        if (!response.ok) fail('ListModels_http_' + response.status);
        raw = JSON.parse(await boundedText(response, 1024 * 1024, false));
      } catch (error) {
        if (error.name === 'AbortError') fail('ListModels_timeout');
        if (error instanceof SyntaxError) fail('browser_incompatible: invalid model list JSON');
        if (error instanceof TypeError) fail('ListModels_network_error');
        throw error;
      } finally { clearTimeout(timer); }
      if (!raw || typeof raw !== 'object' || (raw.models !== undefined && !Array.isArray(raw.models)))
        fail('browser_incompatible: unknown model list shape');
      for (const model of raw.models || []) {
        if (!model || typeof model.name !== 'string' || !model.name.startsWith('models/')) continue;
        const id = model.name.slice(7);
        result.push({
          id, name: model.name,
          displayName: typeof model.displayName === 'string' ? model.displayName : id,
          description: typeof model.description === 'string' ? model.description : '',
          inputTokenLimit: Number.isFinite(model.inputTokenLimit) ? model.inputTokenLimit : 0,
          outputTokenLimit: Number.isFinite(model.outputTokenLimit) ? model.outputTokenLimit : 0,
          supportedGenerationMethods: Array.isArray(model.supportedGenerationMethods)
            ? model.supportedGenerationMethods.filter(method => typeof method === 'string') : [],
          source: 'live'
        });
      }
      token = typeof raw.nextPageToken === 'string' ? raw.nextPageToken : '';
      if (!token) break;
    }
    return result;
  }
  // The host sends a request body in pieces as call arguments, kept as Blobs
  // outside the JavaScript heap, so the page never holds it as one string.
  let upload = null;
  function append(id, chunk) {
    if (typeof id !== 'string' || !id || typeof chunk !== 'string') fail('browser_incompatible: invalid upload chunk');
    if (upload?.id !== id) upload = { id, parts: [] };
    upload.parts.push(new Blob([chunk]));
    return upload.parts.length;
  }
  function takeUpload(id) {
    const current = upload;
    upload = null;
    if (!current || current.id !== id) fail('browser_incompatible: request body upload missing');
    return new Blob(current.parts);
  }
  function abort() {
    upload = null;
    const t = state.transfer;
    if (!t) return;
    t.controller.abort();
    t.pending = null;
    if (t.credit) { t.credit(); t.credit = null; }
    wake(t);
  }
  // Starts streamGenerateContent for args.model with the uploaded body. The
  // response is relayed in pieces of at most args.chunkBytes, one at a time.
  function start(args) {
    let body = takeUpload(args.upload);
    abort();
    if (typeof args.model !== 'string' || !modelId.test(args.model)) fail('browser_incompatible: invalid model id');
    const chunkBytes = Number.isSafeInteger(args.chunkBytes) && args.chunkBytes > 0 ? Math.min(32768, args.chunkBytes) : 32768;
    const t = { controller: new AbortController(), pending: null, sequence: 0, credit: null, done: false, error: '', errorBody: '', status: 0, retryAfter: 0 };
    state.transfer = t;
    const relay = async bytes => {
      for (let offset = 0; offset < bytes.byteLength; offset += chunkBytes) {
        if (t.controller.signal.aborted) fail('cancelled');
        const piece = bytes.subarray(offset, offset + chunkBytes);
        let binary = '';
        for (let i = 0; i < piece.byteLength; i += 8192)
          binary += String.fromCharCode.apply(null, piece.subarray(i, i + 8192));
        const sequence = ++t.sequence;
        await new Promise(resolve => { t.credit = resolve; t.pending = { sequence, bytes: btoa(binary) }; wake(t); });
      }
      if (t.controller.signal.aborted) fail('cancelled');
    };
    void (async () => {
      let reader;
      try {
        const url = api + '/v1beta/models/' + args.model + ':streamGenerateContent?alt=sse';
        const sent = pageFetch(url, { method: 'POST', headers: { 'content-type': 'application/json' }, body, signal: t.controller.signal });
        // The request owns the body now; do not keep it alive for the stream.
        body = null;
        const response = await sent;
        t.status = response.status;
        const retry = response.headers.get('retry-after');
        if (retry) {
          const seconds = /^\d+$/.test(retry.trim()) ? Number(retry) : Math.ceil((Date.parse(retry) - Date.now()) / 1000);
          if (Number.isFinite(seconds) && seconds > 0) t.retryAfter = Math.min(86400, seconds);
        }
        if (!response.ok) {
          // Retain at most 8 KiB for error classification, never a raw dump.
          t.errorBody = await boundedText(response, 8192, true);
          fail('upstream_http_' + response.status);
        }
        if (!response.body) fail('empty_upstream_body');
        // A byte stream reads into a fresh 32 KiB buffer each time. A stream
        // AI Studio builds itself may offer only the default reader, whose
        // chunks are relayed in pieces of the same size.
        let byob = true;
        try { reader = response.body.getReader({ mode: 'byob' }); }
        catch (_) { byob = false; reader = response.body.getReader(); }
        while (true) {
          const { value, done } = byob ? await reader.read(new Uint8Array(chunkBytes)) : await reader.read();
          if (value && value.byteLength) await relay(value instanceof Uint8Array ? value : new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
          if (done) break;
        }
      } catch (error) { t.error = error.name === 'AbortError' ? 'cancelled' : String(error.message || error); }
      finally { if (reader) { try { await reader.cancel(); } catch (_) {} } t.done = true; wake(t); }
    })();
    return true;
  }
  // The host waits in next() for the relay's next state instead of polling.
  function wake(t) { const resolve = t.waiter; t.waiter = null; if (resolve) resolve(); }
  const relayState = t => t ? { pending: t.pending, done: t.done, error: t.error, errorBody: t.errorBody, status: t.status, retryAfter: t.retryAfter } : null;
  // Acknowledges `sequence` (0: nothing to acknowledge) and resolves with the
  // relay state once a chunk is pending, the transfer is done, or `waitMs`
  // (at most 1 s) elapsed.
  async function next(sequence, waitMs) {
    if (sequence) ack(sequence);
    const t = state.transfer;
    const limit = Number.isSafeInteger(waitMs) && waitMs > 0 ? Math.min(waitMs, 1000) : 0;
    if (t && limit && !t.pending && !t.done)
      await new Promise(resolve => {
        const timer = setTimeout(() => { if (t.waiter === done) t.waiter = null; resolve(); }, limit);
        const done = () => { clearTimeout(timer); resolve(); };
        t.waiter = done;
      });
    return relayState(state.transfer);
  }
  function ack(sequence) {
    const t = state.transfer;
    if (!t || !t.pending || t.pending.sequence !== sequence) fail('invalid_credit');
    t.pending = null;
    const resolve = t.credit; t.credit = null; resolve();
    return true;
  }
  globalThis.__ais2app = {
    ready, models, append, start, ack, next, abort,
    poll: () => relayState(state.transfer)
  };
})()
