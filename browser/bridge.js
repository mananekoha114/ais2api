// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

/* ais2api page bridge. No credentials, snapshots or response bodies are persisted. */
(() => {
  if (globalThis.__ais2bridge) return;
  const nativeFetch = globalThis.fetch.bind(globalThis);
  const state = { template: null, snapshotCall: null, models: [], transfer: null, discoveryRequest: null, templateTransport: null, snapshotCandidates: 0, snapshotHooked: 0 };
  const fail = message => { throw new Error(message); };
  const snapshotInvocations = new Map();
  const snapshotWrappers = new WeakSet();
  const benefit = { tier: 'unknown', authUser: null, expires: 0, retryAt: 0, pending: null, controller: null };
  function selectSnapshotService() {
    if (state.snapshotCall || !state.template) return;
    const ticket = state.template.body[4];
    if (typeof ticket !== 'string' || !ticket) return;
    for (const invocation of snapshotInvocations.values()) {
      if (invocation.ticket === ticket) {
        state.snapshotCall = invocation.call;
        snapshotInvocations.clear();
        return;
      }
    }
  }
  const isDiscovery = url => url.protocol === 'https:' && url.hostname === 'alkalimakersuite-pa.clients6.google.com' &&
    url.pathname === '/$rpc/google.internal.alkali.applications.makersuite.v1.MakerSuiteService/ListModels';
  function captureDiscovery(url, headers, method) {
    if (method.toUpperCase() !== 'POST' || !isDiscovery(new URL(url))) return;
    state.discoveryRequest = { url, headers: { ...headers } };
  }
  const rpcKind = url => url.hostname === 'alkalimakersuite-pa.clients6.google.com' &&
    /\/(GenerateContent|SavePrompt|CreatePrompt)$/.exec(url.pathname)?.[1];
  function captureTemplate(url, headers, text, transport) {
    let body;
    try { body = JSON.parse(text); } catch (_) { fail('browser_incompatible: invalid live GenerateContent JSON'); }
    if (!Array.isArray(body)) fail('browser_incompatible: invalid live GenerateContent array');
    state.template = { url, headers: { ...headers }, body };
    state.templateTransport = transport;
    state.snapshotCall = null;
    selectSnapshotService();
  }
  // AI Studio may use either transport. Capture requests at send time, after
  // every application header has been set, without reading cookie values.
  if (typeof XMLHttpRequest !== 'undefined') {
    const requests = new WeakMap();
    const { open, setRequestHeader, send, abort } = XMLHttpRequest.prototype;
    XMLHttpRequest.prototype.open = function(method, url, ...rest) {
      const previous = requests.get(this);
      if (previous?.synthetic) previous.synthetic = false;
      if (previous?.intercepted) for (const key of ['readyState', 'status', 'statusText', 'responseURL', 'response', 'responseText', 'responseXML', 'getResponseHeader', 'getAllResponseHeaders']) delete this[key];
      const result = Reflect.apply(open, this, [method, url, ...rest]);
      requests.set(this, { method: String(method), url: new URL(String(url), location.href).href, headers: {}, async: rest[0] !== false });
      return result;
    };
    XMLHttpRequest.prototype.setRequestHeader = function(name, value) {
      const result = Reflect.apply(setRequestHeader, this, [name, value]);
      const request = requests.get(this);
      if (request) {
        const key = String(name).toLowerCase();
        request.headers[key] = request.headers[key] ? request.headers[key] + ', ' + value : String(value);
      }
      return result;
    };
    XMLHttpRequest.prototype.send = function(...args) {
      const request = requests.get(this);
      if (request) captureDiscovery(request.url, request.headers, request.method);
      const kind = request && rpcKind(new URL(request.url));
      if (!kind) return Reflect.apply(send, this, args);
      if (kind === 'GenerateContent') {
        if (typeof args[0] !== 'string') fail('browser_incompatible: unsupported live XHR request body');
        captureTemplate(request.url, request.headers, args[0], 'xhr');
      }
      // Keep the native object and its listeners. Shadow the browser's read-only
      // response views for this intercepted request; never send the probe or save.
      let readyState = 1, status = 0, response = null, responseText = '';
      request.synthetic = true;
      request.intercepted = true;
      const emit = type => {
        this.dispatchEvent(new Event(type));
      };
      const views = {
        readyState: () => readyState, status: () => status,
        statusText: () => status ? 'OK' : '', responseURL: () => status ? request.url : '',
        response: () => response,
        responseText: () => {
          if (this.responseType && this.responseType !== 'text') throw new DOMException('Response is not text', 'InvalidStateError');
          return responseText;
        },
        responseXML: () => null
      };
      for (const [key, get] of Object.entries(views)) Object.defineProperty(this, key, { configurable: true, get });
      Object.defineProperties(this, {
        getResponseHeader: { configurable: true, value: name => status && String(name).toLowerCase() === 'content-type' ? 'application/json+protobuf' : null },
        getAllResponseHeaders: { configurable: true, value: () => status ? 'content-type: application/json+protobuf\r\n' : '' }
      });
      request.cancelSynthetic = () => { readyState = 0; status = 0; response = null; responseText = ''; emit('readystatechange'); emit('abort'); emit('loadend'); };
      const finish = () => {
        if (!request.synthetic) return;
        status = 200;
        readyState = 2; emit('readystatechange');
        if (!request.synthetic) return;
        readyState = 3; emit('readystatechange');
        if (!request.synthetic) return;
        responseText = '[]';
        switch (this.responseType) {
          case 'json': response = []; break;
          case 'arraybuffer': response = new TextEncoder().encode(responseText).buffer; break;
          case 'blob': response = new Blob([responseText], { type: 'application/json+protobuf' }); break;
          case 'document': response = null; break;
          default: response = responseText;
        }
        readyState = 4; emit('readystatechange');
        if (!request.synthetic) return;
        request.synthetic = false;
        emit('load'); emit('loadend');
      };
      emit('loadstart');
      if (request.async) setTimeout(finish, 0); else finish();
    };
    if (abort) XMLHttpRequest.prototype.abort = function(...args) {
      const request = requests.get(this);
      if (!request?.synthetic) return Reflect.apply(abort, this, args);
      request.synthetic = false;
      request.cancelSynthetic();
    };
  }
  function install() {
    const exports = globalThis.default_MakerSuite;
    if (!exports) return false;
    let candidates = 0, hooked = 0;
    for (const key of Object.keys(exports)) {
      // Inspect descriptors rather than invoking arbitrary export getters.
      const descriptor = Object.getOwnPropertyDescriptor(exports, key);
      if (!descriptor || !('value' in descriptor)) continue;
      const fn = descriptor.value;
      if (typeof fn !== 'function') continue;
      if (snapshotWrappers.has(fn)) { ++candidates; ++hooked; continue; }
      const source = Function.prototype.toString.call(fn);
      if (!/\.snapshot\s*(?:\(|\))/.test(source)) continue;
      ++candidates;
      if (!descriptor.writable && !descriptor.configurable) continue;
      const wrapped = new Proxy(fn, {
        apply(target, receiver, args) {
          const result = Reflect.apply(target, receiver, args);
          // Observe only ordinary calls. Default Proxy construction preserves
          // class/static/prototype behavior of other snapshot-related exports.
          try {
            if (args[0]) {
              const service = args[0], trailing = args.slice(2);
              const observe = ticket => {
                try {
                  if (typeof ticket !== 'string' || !ticket) return;
                  snapshotInvocations.set(wrapped, { ticket, call: hash => Reflect.apply(target, receiver, [service, hash, ...trailing]) });
                  selectSnapshotService();
                } catch (_) { /* Observation cannot change page behavior. */ }
              };
              const then = result && result.then;
              if (typeof then === 'function') Reflect.apply(then, result, [observe, () => {}]);
              else observe(result);
            }
          } catch (_) { /* Includes unusual then getters and thenables. */ }
          return result;
        }
      });
      snapshotWrappers.add(wrapped);
      try {
        Object.defineProperty(exports, key, { ...descriptor, value: wrapped });
        ++hooked;
      } catch (_) { /* Immutable export: report it without claiming a hook. */ }
    }
    state.snapshotCandidates = candidates;
    state.snapshotHooked = hooked;
    return true;
  }
  const installTimer = setInterval(install, 250);
  globalThis.fetch = async function(input, init) {
    const req = new Request(input, init);
    const url = new URL(req.url);
    if (isDiscovery(url)) {
      const headers = {};
      req.headers.forEach((value, key) => { headers[key] = value; });
      captureDiscovery(req.url, headers, req.method);
    }
    if (url.hostname === 'alkalimakersuite-pa.clients6.google.com' && /\/GenerateContent$/.test(url.pathname)) {
      const headers = {};
      req.headers.forEach((v, k) => { headers[k] = v; });
      captureTemplate(req.url, headers, await req.clone().text(), 'fetch');
      // Bootstrap only needs the real page-generated request, not its output.
      return new Response('[]', { status: 200, headers: { 'content-type': 'application/json+protobuf' } });
    }
    if (url.hostname === 'alkalimakersuite-pa.clients6.google.com' && /\/(SavePrompt|CreatePrompt)$/.test(url.pathname)) {
      return new Response('[]', { status: 200, headers: { 'content-type': 'application/json+protobuf' } });
    }
    return nativeFetch(req);
  };
  function identity() {
    const url = new URL(location.href);
    const login = url.hostname === 'accounts.google.com' || /\/signin|\/ServiceLogin/.test(url.pathname);
    const visible = el => !el.hidden && el.getAttribute('aria-hidden') !== 'true' &&
      !(typeof el.closest === 'function' && el.closest('[hidden], [aria-hidden="true"]')) &&
      !(typeof el.getClientRects === 'function' && el.getClientRects().length === 0);
    const accountLinks = [...document.querySelectorAll('a[href]')].filter(el => {
      if (!visible(el)) return false;
      try {
        const target = new URL(el.getAttribute('href'), url.href);
        return target.protocol === 'https:' && ['accounts.google.com', 'myaccount.google.com'].includes(target.hostname) &&
          /@/.test([el.getAttribute('aria-label'), el.getAttribute('data-email')].filter(Boolean).join(' '));
      } catch (_) { return false; }
    });
    // Prefer account controls to unrelated emails in content or hidden menus.
    // Multiple visible account controls still fail closed as ambiguous.
    const candidates = accountLinks.length ? accountLinks :
      [...document.querySelectorAll('[data-email], [aria-label*="@"], img[alt*="@"]')].filter(visible);
    const emails = new Set();
    for (const el of candidates) {
      const label = [el.getAttribute('data-email'), el.getAttribute('aria-label'), el.getAttribute('alt')].filter(Boolean).join(' ');
      for (const match of label.matchAll(/[A-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Z0-9.-]+\.[A-Z]{2,}/ig)) emails.add(match[0].toLowerCase());
    }
    const pageReady = url.hostname === 'aistudio.google.com' && !login && document.readyState === 'complete';
    return { login, regionBlocked: url.pathname.includes('available-regions'), ready: pageReady && !!document.querySelector('textarea'), identityReady: pageReady && emails.size === 1, email: emails.size === 1 ? [...emails][0] : '', identityAmbiguous: emails.size > 1, termsRequired: termsDialog(url), welcomeContinued };
  }
  // AI Studio can open a modal that asks the account to accept its terms. The
  // page behind it is aria-hidden, so the account badge never appears.
  const dialogSelector = 'mat-dialog-container, cdk-dialog-container, [role="dialog"], [role="alertdialog"]';
  const dialogShown = dialog => !(typeof dialog.getClientRects === 'function' && dialog.getClientRects().length === 0);
  function asksForTerms(dialog, url) {
    const links = typeof dialog.querySelectorAll === 'function' ? [...dialog.querySelectorAll('a[href]')] : [];
    if (links.some(el => {
      try {
        const target = new URL(el.getAttribute('href'), url.href);
        return /(^|\.)google\.(com|dev)$/.test(target.hostname) && /\/terms(\/|$)/.test(target.pathname);
      } catch (_) { return false; }
    })) return true;
    return /terms of service|additional terms|服务条款/i.test(dialog.innerText || dialog.textContent || '');
  }
  // The welcome dialog opens in new sessions even for accounts that accepted
  // it before, so it is continued here. It is told apart by its shape, in any
  // language: it links the terms and offers one button and nothing to tick.
  // Any other dialog about terms is left to the account's owner.
  const welcomePressed = new WeakMap();
  let welcomeContinued = 0;
  function continueWelcome(dialog) {
    if (typeof dialog.querySelectorAll !== 'function') return false;
    const buttons = [...dialog.querySelectorAll('button')];
    if (buttons.length !== 1 || buttons[0].disabled || buttons[0].getAttribute('aria-disabled') === 'true') return false;
    if (dialog.querySelectorAll('input, textarea, select, mat-checkbox, [role="checkbox"], [role="switch"], [role="radio"]').length) return false;
    try { buttons[0].click(); } catch (_) { return false; }
    welcomePressed.set(dialog, Date.now());
    ++welcomeContinued;
    return true;
  }
  // 'none' when the dialog is not about terms, 'continued' while a welcome
  // dialog that was continued is closing, 'required' when the owner must act.
  function termsState(dialog, url) {
    if (!dialogShown(dialog) || !asksForTerms(dialog, url)) return 'none';
    const pressed = welcomePressed.get(dialog);
    if (pressed === undefined) return continueWelcome(dialog) ? 'continued' : 'required';
    // A dialog that stays open did not take the button.
    return Date.now() - pressed < 10000 ? 'continued' : 'required';
  }
  function termsDialog(url) {
    let required = false;
    for (const dialog of document.querySelectorAll(dialogSelector))
      if (termsState(dialog, url) === 'required') required = true;
    return required;
  }
  const controlLabel = el => (el.innerText || el.getAttribute('aria-label') || '').trim();
  // Other overlays, such as announcements, are only ever dismissed: never tick
  // a checkbox or press a button that agrees, accepts or continues, since
  // those can change account settings or terms. The welcome dialog is the one
  // exception, see continueWelcome.
  const dismissLabels = /^(close|dismiss|got it|no,? thanks|not now|maybe later|skip|cancel)$/i;
  function dismissDialogs() {
    let dismissed = 0;
    const url = new URL(location.href);
    for (const dialog of document.querySelectorAll(dialogSelector)) {
      if (termsState(dialog, url) === 'continued') { ++dismissed; continue; }
      const button = [...dialog.querySelectorAll('button')].find(el => !el.disabled &&
        el.getAttribute('aria-disabled') !== 'true' && dismissLabels.test(controlLabel(el)));
      try {
        if (button) button.click();
        // CDK dialogs close on Escape unless the dialog forbids it.
        else dialog.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', code: 'Escape', keyCode: 27, bubbles: true, cancelable: true }));
        ++dismissed;
      } catch (_) {}
    }
    return dismissed;
  }
  let bootstrapInput = null;
  function finishBootstrap() {
    if (!bootstrapInput) return true;
    const { input, value } = bootstrapInput;
    bootstrapInput = null;
    const stop = [...document.querySelectorAll('button')].find(el => !el.disabled && /^Stop(\s|$)/.test((el.innerText || el.getAttribute('aria-label') || '').trim()));
    if (stop) stop.click();
    const setter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set;
    setter.call(input, value);
    input.dispatchEvent(new Event('input', { bubbles: true }));
    return true;
  }
  function bootstrap() {
    install();
    dismissDialogs();
    const input = document.querySelector('textarea');
    if (!input) return false;
    if (!bootstrapInput) bootstrapInput = { input, value: input.value };
    // A repeated bootstrap replaces a template the host discarded as stale.
    state.template = null;
    state.snapshotCall = null;
    state.templateTransport = null;
    // Temporary chat is optional; only activate an explicit inactive control.
    const temporary = [...document.querySelectorAll('button')].find(el => !el.disabled &&
      /^Temporary chat$/i.test((el.innerText || el.getAttribute('aria-label') || '').trim()) &&
      el.getAttribute('aria-pressed') === 'false');
    if (temporary) { try { temporary.click(); } catch (_) {} }
    const setter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set;
    setter.call(input, 'Reply with the single digit 1.');
    input.dispatchEvent(new Event('input', { bubbles: true }));
    return true;
  }
  // Returns true after clicking Run, 'shortcut' after sending the editor's
  // submit shortcut (the host follows with a trusted key press if needed).
  function run() {
    dismissDialogs();
    const button = [...document.querySelectorAll('button')].find(el => !el.disabled && /^(Run|Build)(\s|$)/.test(controlLabel(el)));
    if (button) { button.click(); return true; }
    const input = document.querySelector('textarea');
    if (!input || input.disabled) return false;
    const mac = /Mac/i.test(globalThis.navigator?.platform || '');
    input.focus?.();
    for (const type of ['keydown', 'keyup'])
      input.dispatchEvent(new KeyboardEvent(type, { key: 'Enter', code: 'Enter', keyCode: 13, which: 13, ctrlKey: !mac, metaKey: mac, bubbles: true, cancelable: true }));
    return 'shortcut';
  }
  // A busy page can drop or stall a snapshot call: retry it a bounded number
  // of times. The host evaluation timeout covers every attempt.
  const snapshotAttempts = 3, snapshotAttemptMs = 9000;
  async function snapshot(hash) {
    if (!state.snapshotCall || !state.template) fail('browser_incompatible: snapshot service or live template unavailable');
    const call = state.snapshotCall;
    for (let attempt = 1; ; ++attempt) {
      let timer;
      try {
        const value = await Promise.race([call(hash), new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error('snapshot attempt timed out')), snapshotAttemptMs);
        })]);
        if (typeof value === 'string' && value.length) return value;
      } catch (_) {
        // Retry below; the page's own failure details are not reported.
      } finally { clearTimeout(timer); }
      if (attempt === snapshotAttempts) fail('browser_incompatible: snapshot service returned no ticket');
      await new Promise(resolve => setTimeout(resolve, 100));
    }
  }
  function authenticatedHeaders(captured, expectedAuthUser, authorization) {
    if (typeof expectedAuthUser !== 'string' || !/^\d+$/.test(expectedAuthUser))
      fail('browser_incompatible: invalid request auth_user');
    if (new URL(location.href).origin !== 'https://aistudio.google.com')
      fail('identity_mismatch: unexpected authentication origin');
    const sameUser = value => /^\d+$/.test(value) && BigInt(value) === BigInt(expectedAuthUser);
    if (new URL(captured.url).searchParams.getAll('authuser').some(value => !sameUser(value)))
      fail('identity_mismatch: captured request auth_user differs from verified account');
    const headers = {};
    let capturedAuthorization = '';
    for (const [key, value] of Object.entries(captured.headers)) {
      const lower = key.toLowerCase();
      if (lower === 'x-goog-authuser' && !sameUser(value))
        fail('identity_mismatch: captured request auth_user differs from verified account');
      if (lower === 'authorization' && typeof value === 'string' && value.trim()) capturedAuthorization = value;
      if (!['host', 'content-length', 'authorization', 'x-goog-authuser', 'x-aistudio-g1-tier'].includes(lower)) headers[lower] = value;
    }
    // The native host signs current CDP cookies. As in the reference transport,
    // use only this page's captured Authorization when a fresh one is unavailable.
    const selected = typeof authorization === 'string' && authorization.trim() ? authorization : capturedAuthorization;
    if (!selected) fail('browser_incompatible: current signing cookies and captured Authorization unavailable');
    headers.authorization = selected;
    headers['x-goog-authuser'] = expectedAuthUser;
    return headers;
  }
  async function benefitHeaders(captured, authUser, authorization, signal) {
    const headers = authenticatedHeaders(captured, authUser, authorization);
    const url = new URL(captured.url);
    url.pathname = url.pathname.replace(/\/(ListModels|GenerateContent)$/, '/GetAiStudioBenefitTier');
    const source = new URL(captured.url);
    source.pathname = source.pathname.replace(/\/GenerateContent$/, '/ListModels');
    if (!isDiscovery(source)) fail('browser_incompatible: invalid benefit endpoint');
    const account = BigInt(authUser).toString();
    if (benefit.authUser !== null && benefit.authUser !== account) fail('identity_mismatch: benefit account changed within page context');
    benefit.authUser = account;
    if (signal?.aborted) fail('cancelled');
    if (Date.now() >= benefit.expires && Date.now() >= benefit.retryAt && !benefit.pending) {
      benefit.tier = 'unknown';
      const controller = new AbortController();
      benefit.controller = controller;
      const cancel = () => controller.abort();
      signal?.addEventListener('abort', cancel, { once: true });
      benefit.pending = (async () => {
        const timer = setTimeout(() => controller.abort(), 8000);
        let reader;
        try {
          const response = await nativeFetch(url.href, { method: 'POST', headers: { ...headers }, credentials: 'include', redirect: 'error', body: '[]', signal: controller.signal });
          if (!response.ok || !response.body) throw new Error('benefit_unavailable');
          reader = response.body.getReader();
          const decoder = new TextDecoder();
          let text = '', size = 0;
          while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            size += value.byteLength;
            if (size > 8192) throw new Error('benefit_invalid');
            text += decoder.decode(value, { stream: true });
          }
          text = (text + decoder.decode()).trim().replace(/^\)\]\}'/, '').trim().replace(/^\[,/, '[null,');
          const raw = JSON.parse(text);
          if (!Array.isArray(raw)) throw new Error('benefit_invalid');
          const code = raw[0] === undefined || raw[0] === null ? 0 : raw[0];
          if (!Number.isInteger(code) || code < 0 || code > 3) throw new Error('benefit_invalid');
          if (controller.signal.aborted) throw new Error('benefit_unavailable');
          benefit.tier = ['free', 'pro', 'ultra', 'plus'][code];
          benefit.expires = Date.now() + 600000;
          benefit.retryAt = 0;
        } catch (_) {
          benefit.tier = 'unknown';
          benefit.expires = 0;
          benefit.retryAt = Date.now() + 30000;
        } finally {
          clearTimeout(timer);
          signal?.removeEventListener('abort', cancel);
          if (reader) { try { await reader.cancel(); } catch (_) {} }
          benefit.controller = null;
        }
      })();
      try { await benefit.pending; } finally { benefit.pending = null; }
    } else if (benefit.pending) {
      await new Promise((resolve, reject) => {
        const cancel = () => reject(new Error('cancelled'));
        signal?.addEventListener('abort', cancel, { once: true });
        benefit.pending.then(() => { signal?.removeEventListener('abort', cancel); resolve(); });
      });
    }
    if (signal?.aborted) fail('cancelled');
    const tierHeader = { plus: 'TIER0', pro: 'TIER1', ultra: 'TIER2' }[benefit.tier];
    if (tierHeader && Date.now() < benefit.expires) headers['x-aistudio-g1-tier'] = tierHeader;
    return headers;
  }
  async function discoverModels(expectedAuthUser, authorization) {
    if (typeof expectedAuthUser !== 'string' || !/^\d+$/.test(expectedAuthUser)) fail('browser_incompatible: invalid discovery auth_user');
    for (let attempt = 0; !state.discoveryRequest && attempt < 20; ++attempt)
      await new Promise(resolve => setTimeout(resolve, 100));
    const captured = state.discoveryRequest;
    if (!captured) fail('browser_incompatible: no live ListModels request captured');
    const url = new URL(captured.url);
    if (!isDiscovery(url)) fail('browser_incompatible: invalid discovery endpoint');
    const headers = { ...captured.headers };
    const sameUser = value => /^\d+$/.test(value) && BigInt(value) === BigInt(expectedAuthUser);
    if ((headers['x-goog-authuser'] !== undefined && !sameUser(headers['x-goog-authuser'])) ||
        url.searchParams.getAll('authuser').some(value => !sameUser(value)))
      fail('identity_mismatch: captured ListModels auth_user differs from verified account');
    const authenticated = await benefitHeaders(captured, expectedAuthUser, authorization);
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10000);
    try {
      const response = await nativeFetch(url.href, { method: 'POST', headers: authenticated, credentials: 'include', body: '[]', signal: controller.signal });
      if (!response.ok) fail('ListModels_http_' + response.status);
      if (!response.body) fail('browser_incompatible: empty ListModels response');
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let text = '', size = 0;
      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) break;
          size += value.byteLength;
          if (size > 1024 * 1024) fail('ListModels_response_too_large');
          text += decoder.decode(value, { stream: true });
        }
        text += decoder.decode();
      } finally { await reader.cancel(); }
      let raw;
      try { raw = JSON.parse(text); } catch (_) { fail('browser_incompatible: invalid ListModels JSON'); }
      if (!Array.isArray(raw) || !Array.isArray(raw[0])) fail('browser_incompatible: unknown ListModels wire shape');
      state.models = raw[0].flatMap(row => {
        if (!Array.isArray(row) || typeof row[0] !== 'string' || !row[0].startsWith('models/')) return [];
        return [{ id: row[0].slice(7), name: row[0], displayName: typeof row[3] === 'string' ? row[3] : row[0].slice(7), description: typeof row[4] === 'string' ? row[4] : '', inputTokenLimit: Number.isFinite(row[5]) ? row[5] : 0, outputTokenLimit: Number.isFinite(row[6]) ? row[6] : 0, supportedGenerationMethods: Array.isArray(row[7]) ? row[7] : [], source: 'live' }];
      });
      return state.models;
    } catch (error) {
      if (error.name === 'AbortError') fail('ListModels_timeout');
      if (error instanceof TypeError) fail('ListModels_network_error');
      throw error;
    } finally { clearTimeout(timer); }
  }
  // The host sends a request body in pieces as call arguments. Each piece is
  // kept as a Blob, which Chromium stores outside the JavaScript heap, so the
  // page never holds the whole body as a string.
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
  function start(args) {
    let body = args.upload === undefined ? args.body : takeUpload(args.upload);
    abort();
    const chunkBytes = Number.isSafeInteger(args.chunkBytes) && args.chunkBytes > 0 ? Math.min(32768, args.chunkBytes) : 32768;
    const t = { controller: new AbortController(), pending: null, sequence: 0, credit: null, done: false, error: '', errorBody: '', status: 0, retryAfter: 0, benefitTier: 'unknown' };
    state.transfer = t;
    void (async () => {
      let reader;
      try {
        const authUser = args.authUser === undefined ? new URL(location.href).searchParams.get('authuser') || '0' : args.authUser;
        const headers = await benefitHeaders(state.template, authUser, args.authorization, t.controller.signal);
        if (t.controller.signal.aborted) fail('cancelled');
        t.benefitTier = Date.now() < benefit.expires ? benefit.tier : 'unknown';
        // A string body gets this type by default; a typeless Blob gets none.
        if (typeof body !== 'string' && !('content-type' in headers)) headers['content-type'] = 'text/plain;charset=UTF-8';
        const sent = nativeFetch(state.template.url, { method: 'POST', credentials: 'include', headers, body, signal: t.controller.signal });
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
          // Retain at most 8 KiB for protocol classification, never a raw dump.
          if (response.body) {
            try { reader = response.body.getReader({ mode: 'byob' }); } catch (_) {}
            if (reader) {
              const decoder = new TextDecoder();
              let remaining = 8192;
              while (remaining) {
                const { value, done } = await reader.read(new Uint8Array(remaining));
                if (value) { t.errorBody += decoder.decode(value, { stream: true }); remaining -= value.byteLength; }
                if (done) break;
              }
              t.errorBody += decoder.decode();
            } else { try { await response.body.cancel(); } catch (_) {} }
          }
          fail('upstream_http_' + response.status);
        }
        if (!response.body) fail('empty_upstream_body');
        // Require the byte reader: each application read is limited to 32 KiB.
        // Do not fall back to a reader that can allocate an arbitrary chunk.
        try { reader = response.body.getReader({ mode: 'byob' }); }
        catch (_) { fail('browser_incompatible: fetch BYOB reader unavailable'); }
        while (true) {
          const { value, done } = await reader.read(new Uint8Array(chunkBytes));
          if (value && value.byteLength) {
            if (t.controller.signal.aborted) fail('cancelled');
            let binary = '';
            for (let i = 0; i < value.byteLength; i += 8192)
              binary += String.fromCharCode.apply(null, value.subarray(i, i + 8192));
            const sequence = ++t.sequence;
            await new Promise(resolve => { t.credit = resolve; t.pending = { sequence, bytes: btoa(binary) }; wake(t); });
            if (t.controller.signal.aborted) fail('cancelled');
          }
          if (done) break;
        }
      } catch (error) { t.error = error.name === 'AbortError' ? 'cancelled' : String(error.message || error); }
      finally { if (reader) { try { await reader.cancel(); } catch (_) {} } t.done = true; wake(t); }
    })();
    return true;
  }
  // The host waits in next() for the relay's next state instead of polling.
  function wake(t) { const resolve = t.waiter; t.waiter = null; if (resolve) resolve(); }
  const relayState = t => t ? { pending: t.pending, done: t.done, error: t.error, errorBody: t.errorBody, status: t.status, retryAfter: t.retryAfter, benefitTier: t.benefitTier } : null;
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
  globalThis.__ais2bridge = {
    identity, bootstrap, finishBootstrap, run, snapshot, append, start, ack, next, abort, discoverModels,
    poll: () => relayState(state.transfer),
    capability: () => ({ benefitTier: Date.now() < benefit.expires ? benefit.tier : 'unknown', template: state.template ? { body: state.template.body } : null, timezone: Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC', snapshot: !!state.snapshotCall, discovery: !!state.discoveryRequest, templateTransport: state.templateTransport, snapshotCandidates: state.snapshotCandidates, snapshotHooked: state.snapshotHooked }),
    models: () => state.models,
    dispose: () => { abort(); benefit.controller?.abort(); clearInterval(installTimer); }
  };
})()
