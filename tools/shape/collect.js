// Copyright (c) 2026 Mana Nekoha (mananekoha114)
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Shape collector for the DOM emulation in waa/third_party/aistudio2api/dom.js
// and smhost.js.
//
// Evaluated once, as a script, in a page's top-level document. window[0] must
// be the same-origin frame the table describes as "frame" (AI Studio's
// bscframe, or the frame in collect.html). The completion value is a promise
// of the table as a JSON string; see README.md for the format.
//
// - It defines no globals. It lists both global objects' keys before it
//   touches anything else, because resolving a lazy global moves that global
//   to the end of the key order.
// - It reads properties, constructs objects and calls methods with an object
//   that fails their brand check, which is how a page probes the same things.
//   It does not construct what starts network, media or worker activity.
// - It never reads what carries the user's data: document.cookie is recorded
//   as "", Web Storage contents are not listed, and the text of the connected
//   html, head and body elements is left out. Objects are recorded by class,
//   never by content.
(() => {
  'use strict';
  const getNames = Object.getOwnPropertyNames;
  const topWindow = window;
  const frameWindow = topWindow[0];
  if (!frameWindow) throw new Error('window[0] is missing: the page needs its same-origin frame');
  const topNames = getNames(topWindow);
  const frameNames = getNames(frameWindow);

  const describe = Object.getOwnPropertyDescriptor;
  const protoOf = Object.getPrototypeOf;
  const ownKeys = Reflect.ownKeys;
  const apply = Reflect.apply;
  const functionSource = Function.prototype.toString;
  const objectTag = Object.prototype.toString;
  const promiseThen = Promise.prototype.then;
  const nativeSource = /\{\s*\[native code\]\s*\}\s*$/;

  const SINGLETONS = [
    'document', 'navigator', 'screen', 'location', 'history', 'performance', 'localStorage', 'sessionStorage',
    'visualViewport', 'crypto', 'speechSynthesis', 'customElements', 'external', 'indexedDB', 'trustedTypes',
    'scheduler', 'menubar', 'toolbar', 'locationbar', 'personalbar', 'scrollbars', 'statusbar',
    'performance.navigationEntry', 'document.documentElement', 'document.head', 'document.body',
    'document.implementation', 'document.fonts', 'navigator.plugins', 'navigator.mimeTypes',
    'navigator.mediaDevices', 'navigator.permissions', 'navigator.userActivation', 'navigator.storage',
    'navigator.locks', 'navigator.mediaCapabilities', 'navigator.clipboard', 'navigator.credentials',
    'navigator.geolocation', 'navigator.serviceWorker', 'navigator.mediaSession', 'navigator.wakeLock',
    'navigator.login', 'navigator.gpu', 'screen.orientation', 'crypto.subtle', 'cookieStore', 'caches',
  ];
  const TOP_SINGLETONS = [
    'document', 'navigator', 'screen', 'location', 'history', 'performance', 'localStorage', 'sessionStorage',
    'visualViewport', 'document.documentElement', 'document.head', 'document.body', 'performance.navigationEntry',
  ];
  // Paths whose text is the page's content.
  const CONNECTED_ELEMENTS = new Set(['document.documentElement', 'document.head', 'document.body']);
  const CONTENT = new Set(['innerText', 'outerText', 'innerHTML', 'outerHTML', 'textContent']);
  // Constructors left unconstructed: network, media, worker, stream and
  // notification activity.
  const NOT_CONSTRUCTED = new Set([
    'ReadableStream', 'VideoEncoder', 'FileReader', 'MediaRecorder', 'MessageChannel', 'WritableStream', 'WebTransport',
    'AudioDecoder', 'AudioEncoder', 'DecompressionStream', 'TextEncoderStream', 'BroadcastChannel',
    'SpeechSynthesisUtterance', 'VideoDecoder', 'Notification', 'RTCPeerConnection', 'Response', 'CompressionStream',
    'TextDecoderStream', 'AudioContext', 'MediaStream', 'EventSource', 'Worker', 'ImageDecoder', 'OfflineAudioContext',
    'SharedWorker', 'RTCIceCandidate', 'RTCSessionDescription', 'WebSocket', 'TransformStream', 'MediaSource', 'Request',
    'Audio', 'XMLHttpRequest',
  ]);
  const NAMESPACES = ['CSS', 'console', 'WebAssembly', 'Intl'];
  const EVENTS = ['Event', 'UIEvent', 'MouseEvent', 'KeyboardEvent', 'FocusEvent', 'CustomEvent', 'MessageEvent', 'CompositionEvent', 'DragEvent'];
  const TAGS = [
    'a', 'abbr', 'address', 'area', 'article', 'aside', 'audio', 'b', 'base', 'bdi', 'bdo', 'blockquote', 'body', 'br',
    'button', 'canvas', 'caption', 'cite', 'code', 'col', 'colgroup', 'data', 'datalist', 'dd', 'del', 'details', 'dfn',
    'dialog', 'div', 'dl', 'dt', 'em', 'embed', 'fieldset', 'figcaption', 'figure', 'footer', 'form', 'h1', 'head',
    'header', 'hgroup', 'hr', 'html', 'i', 'iframe', 'img', 'input', 'ins', 'kbd', 'label', 'legend', 'li', 'link',
    'main', 'map', 'mark', 'menu', 'meta', 'meter', 'nav', 'noscript', 'object', 'ol', 'optgroup', 'option', 'output',
    'p', 'picture', 'pre', 'progress', 'q', 'rp', 'rt', 'ruby', 's', 'samp', 'script', 'search', 'section', 'select',
    'slot', 'small', 'source', 'span', 'strong', 'style', 'sub', 'summary', 'sup', 'table', 'tbody', 'td', 'template',
    'textarea', 'tfoot', 'th', 'thead', 'time', 'title', 'tr', 'track', 'u', 'ul', 'var', 'video', 'wbr', 'applet',
    'acronym', 'bgsound', 'dir', 'frame', 'frameset', 'noframes', 'isindex', 'keygen', 'listing', 'menuitem', 'nextid',
    'noembed', 'plaintext', 'rb', 'rtc', 'strike', 'xmp', 'basefont', 'big', 'blink', 'center', 'font', 'marquee',
    'multicol', 'nobr', 'spacer', 'tt', 'image', 'math', 'svg', 'unknown-tag', 'x-custom',
  ];
  const MEDIA_FEATURES = [
    'any-pointer:fine', 'any-pointer:coarse', 'any-pointer:none', 'pointer:fine', 'pointer:coarse', 'pointer:none',
    'any-hover:hover', 'any-hover:none', 'hover:hover', 'hover:none', 'prefers-color-scheme:dark',
    'prefers-color-scheme:light', 'prefers-reduced-motion:reduce', 'prefers-reduced-motion:no-preference',
    'prefers-contrast:no-preference', 'prefers-contrast:more', 'prefers-contrast:less',
    'prefers-reduced-transparency:reduce', 'forced-colors:active', 'forced-colors:none', 'inverted-colors:inverted',
    'inverted-colors:none', 'color-gamut:srgb', 'color-gamut:p3', 'color-gamut:rec2020', 'dynamic-range:high',
    'dynamic-range:standard', 'orientation:landscape', 'orientation:portrait', 'display-mode:browser',
    'display-mode:standalone', 'display-mode:fullscreen', 'scripting:enabled', 'update:fast', 'overflow-block:scroll',
    'monochrome', 'color', 'grid', '-moz-platform:windows', 'prefers-reduced-data:reduce',
  ];
  const BUILTINS = [
    'Object', 'Object.prototype', 'Function', 'Function.prototype', 'Array', 'Array.prototype', 'Boolean',
    'Boolean.prototype', 'JSON', 'Date', 'Date.prototype', 'Math', 'Number', 'Number.prototype', 'String',
    'String.prototype', 'RegExp', 'RegExp.prototype', 'Error', 'Error.prototype', 'InternalError',
    'InternalError.prototype', 'AggregateError', 'AggregateError.prototype', 'EvalError', 'EvalError.prototype',
    'RangeError', 'RangeError.prototype', 'ReferenceError', 'ReferenceError.prototype', 'SuppressedError',
    'SuppressedError.prototype', 'SyntaxError', 'SyntaxError.prototype', 'TypeError', 'TypeError.prototype', 'URIError',
    'URIError.prototype', 'ArrayBuffer', 'ArrayBuffer.prototype', 'Int8Array', 'Int8Array.prototype', 'Uint8Array',
    'Uint8Array.prototype', 'Int16Array', 'Int16Array.prototype', 'Uint16Array', 'Uint16Array.prototype', 'Int32Array',
    'Int32Array.prototype', 'Uint32Array', 'Uint32Array.prototype', 'Float32Array', 'Float32Array.prototype',
    'Float64Array', 'Float64Array.prototype', 'Uint8ClampedArray', 'Uint8ClampedArray.prototype', 'BigInt64Array',
    'BigInt64Array.prototype', 'BigUint64Array', 'BigUint64Array.prototype', 'Float16Array', 'Float16Array.prototype',
    'BigInt', 'BigInt.prototype', 'Proxy', 'WeakMap', 'WeakMap.prototype', 'Map', 'Map.prototype', 'Set',
    'Set.prototype', 'DataView', 'DataView.prototype', 'Symbol', 'Symbol.prototype', 'Intl', 'Intl.getCanonicalLocales',
    'Intl.supportedValuesOf', 'Intl.Collator', 'Intl.Collator.prototype', 'Intl.DateTimeFormat',
    'Intl.DateTimeFormat.prototype', 'Intl.DisplayNames', 'Intl.DisplayNames.prototype', 'Intl.DurationFormat',
    'Intl.DurationFormat.prototype', 'Intl.ListFormat', 'Intl.ListFormat.prototype', 'Intl.Locale',
    'Intl.Locale.prototype', 'Intl.NumberFormat', 'Intl.NumberFormat.prototype', 'Intl.PluralRules',
    'Intl.PluralRules.prototype', 'Intl.RelativeTimeFormat', 'Intl.RelativeTimeFormat.prototype', 'Intl.Segmenter',
    'Intl.Segmenter.prototype', 'Reflect', 'WeakSet', 'WeakSet.prototype', 'Atomics', 'Promise', 'Promise.prototype',
    'WebAssembly', 'WebAssembly.compile', 'WebAssembly.instantiate', 'WebAssembly.validate',
    'WebAssembly.compileStreaming', 'WebAssembly.instantiateStreaming', 'WebAssembly.Module',
    'WebAssembly.Module.prototype', 'WebAssembly.Instance', 'WebAssembly.Instance.prototype', 'WebAssembly.Memory',
    'WebAssembly.Memory.prototype', 'WebAssembly.Table', 'WebAssembly.Table.prototype', 'WebAssembly.Global',
    'WebAssembly.Global.prototype', 'WebAssembly.CompileError', 'WebAssembly.CompileError.prototype',
    'WebAssembly.LinkError', 'WebAssembly.LinkError.prototype', 'WebAssembly.RuntimeError',
    'WebAssembly.RuntimeError.prototype', 'WebAssembly.Tag', 'WebAssembly.Tag.prototype', 'WebAssembly.Exception',
    'WebAssembly.Exception.prototype', 'WebAssembly.JSTag', 'WebAssembly.promising', 'WebAssembly.Suspending',
    'WebAssembly.Suspending.prototype', 'WebAssembly.SuspendError', 'WebAssembly.SuspendError.prototype',
    'FinalizationRegistry', 'FinalizationRegistry.prototype', 'WeakRef', 'WeakRef.prototype', 'Iterator',
    'Iterator.prototype', 'DisposableStack', 'DisposableStack.prototype', 'AsyncDisposableStack',
    'AsyncDisposableStack.prototype', 'Temporal', 'Temporal.Duration', 'Temporal.Duration.prototype',
    'Temporal.Instant', 'Temporal.Instant.prototype', 'Temporal.PlainDate', 'Temporal.PlainDate.prototype',
    'Temporal.PlainDateTime', 'Temporal.PlainDateTime.prototype', 'Temporal.PlainMonthDay',
    'Temporal.PlainMonthDay.prototype', 'Temporal.PlainTime', 'Temporal.PlainTime.prototype', 'Temporal.PlainYearMonth',
    'Temporal.PlainYearMonth.prototype', 'Temporal.ZonedDateTime', 'Temporal.ZonedDateTime.prototype', 'Temporal.Now',
  ];

  // --- reflection -----------------------------------------------------------

  // Symbols are named '@@' and their description, without 'Symbol.'.
  const keyName = key => typeof key !== 'symbol' ? key : '@@' + (key.description ?? '').replace(/^Symbol\./, '');
  const tryOr = (fallback, read) => { try { return read(); } catch { return fallback; } };
  const keysOf = object => tryOr([], () => ownKeys(object));
  const protoOrNull = object => tryOr(null, () => protoOf(object));
  const isNative = fn => tryOr(false, () => nativeSource.test(apply(functionSource, fn, [])));
  const ownValue = (object, key, type) => {
    const d = tryOr(undefined, () => describe(object, key));
    return d && 'value' in d && typeof d.value === type ? d.value : undefined;
  };
  const nameOf = fn => ownValue(fn, 'name', 'string') ?? '';
  const lengthOf = fn => ownValue(fn, 'length', 'number') ?? 0;
  const classOf = value => tryOr('Object', () => apply(objectTag, value, []).slice(8, -1));
  const isObject = value => value !== null && (typeof value === 'object' || typeof value === 'function');
  const isInterface = fn => {
    if (typeof fn !== 'function' || !isNative(fn)) return false;
    const d = tryOr(undefined, () => describe(fn, 'prototype'));
    return Boolean(d && 'value' in d && d.value !== null && typeof d.value === 'object');
  };
  const isConstructor = fn => tryOr(false, () => (Reflect.construct(String, [], fn), true));
  // The name of an object's level: its own constructor's name, else its own
  // @@toStringTag.
  const levelName = level => {
    const ctor = ownValue(level, 'constructor', 'function');
    if (ctor) return nameOf(ctor);
    return ownValue(level, Symbol.toStringTag, 'string') ?? null;
  };
  // The interface an object belongs to: the nearest level with a name.
  const interfaceOf = object => {
    for (let level = protoOrNull(object); level !== null; level = protoOrNull(level)) {
      const name = levelName(level);
      if (name !== null) return name;
    }
    return '';
  };
  const errorText = error => tryOr('Error: ', () => (isObject(error) ? String(error.name) + ': ' + String(error.message) : 'Error: ' + String(error)));
  const flagsOf = d => (d.configurable ? 'c' : '') + (d.enumerable ? 'e' : '') + ('value' in d && d.writable ? 'w' : '');

  // --- values ---------------------------------------------------------------

  const refs = new Map();
  const resolve = (win, path) => {
    if (path === 'performance.navigationEntry') return win.performance.getEntriesByType('navigation')[0];
    return path.split('.').reduce((object, name) => object == null ? undefined : object[name], win);
  };
  for (const [realm, win] of [['top', topWindow], ['frame', frameWindow]]) {
    refs.set(win, realm + '.window');
    for (const path of SINGLETONS) {
      if (path === 'performance.navigationEntry') continue;
      const object = tryOr(undefined, () => resolve(win, path));
      if (isObject(object) && !refs.has(object)) refs.set(object, realm + '.' + path);
    }
  }

  const encode = value => {
    if (value === undefined) return { t: 'u' };
    if (value === null) return { t: 'null' };
    switch (typeof value) {
      case 'string': return { t: 's', v: value };
      case 'boolean': return { t: 'b', v: value };
      case 'number': return { t: 'n', v: Number.isFinite(value) ? (Object.is(value, -0) ? '-0' : value) : String(value) };
      case 'bigint': return { t: 'bigint', v: String(value) };
      case 'symbol': return { t: 'sym', v: value.description ?? '' };
      case 'function': return { t: 'f', v: nameOf(value), n: isNative(value) };
    }
    // Objects, and document.all, whose typeof is 'undefined'.
    const path = refs.get(value);
    if (path) return { t: 'ref', v: path };
    // As in AIStudio2API's tables, an array is listed only when it holds
    // nothing but primitives and functions; dom.js makes any other array an
    // empty one.
    if (tryOr(false, () => Array.isArray(value))) {
      const items = tryOr(null, () => Array.from(value, item => (isObject(item) && typeof item !== 'function' ? null : encode(item))));
      if (items && !items.includes(null)) return { t: 'a', v: items };
    }
    return { t: 'o', c: classOf(value), k: interfaceOf(value) };
  };
  const capture = read => {
    try { return encode(read()); } catch (error) { return { t: 'throw', v: errorText(error) }; }
  };

  // --- entries --------------------------------------------------------------

  const entry = (object, key) => {
    const n = keyName(key);
    const d = tryOr(undefined, () => describe(object, key));
    if (!d) return null;
    const result = { n, f: flagsOf(d) };
    if (!('value' in d)) {
      result.k = 'a';
      if (d.get) { result.g = nameOf(d.get); result.gl = lengthOf(d.get); }
      if (d.set) { result.s = nameOf(d.set); result.sl = lengthOf(d.set); }
      return result;
    }
    const value = d.value;
    if (typeof value === 'function') {
      result.k = isInterface(value) ? 'i' : 'm';
      result.l = lengthOf(value);
      result.fn = nameOf(value);
      result.nat = isNative(value);
      return result;
    }
    result.k = 'v';
    result.v = encode(value);
    return result;
  };
  const entries = (object, keys = keysOf(object), skip = null) => {
    const out = [];
    for (const key of keys) {
      if (skip && skip.has(key)) continue;
      const e = entry(object, key);
      if (e) out.push(e);
    }
    return out;
  };
  const isStorage = object => classOf(object) === 'Storage';
  const isDocument = object => /^(HTML|XML)?Document$/.test(classOf(object));
  // Own accessors and own values other than functions first, then each
  // prototype's accessors, nearest first.
  const valuesOf = (object, skip = null) => {
    const out = {};
    const seen = new Set();
    const put = (name, read) => {
      if (skip && skip.has(name)) return;
      out[name] = name === 'cookie' && isDocument(object) ? { t: 's', v: '' } : capture(read);
    };
    if (!isStorage(object)) {
      for (const key of keysOf(object)) {
        const name = keyName(key);
        if (seen.has(name)) continue;
        seen.add(name);
        const d = tryOr(undefined, () => describe(object, key));
        if (!d) continue;
        if ('value' in d) { if (typeof d.value !== 'function') put(name, () => d.value); }
        else if (d.get) put(name, () => apply(d.get, object, []));
      }
    }
    for (let level = protoOrNull(object); level !== null && protoOrNull(level) !== null; level = protoOrNull(level)) {
      for (const key of keysOf(level)) {
        const name = keyName(key);
        if (seen.has(name)) continue;
        seen.add(name);
        const d = tryOr(undefined, () => describe(level, key));
        if (d && d.get) put(name, () => apply(d.get, object, []));
      }
    }
    return out;
  };
  const ownEntries = object => isStorage(object) ? [] : entries(object);

  // --- global objects -------------------------------------------------------

  const symbolsOf = object => tryOr([], () => Object.getOwnPropertySymbols(object));
  const realmShape = (win, names) => {
    const global = [];
    const windowValues = {};
    for (const key of names.concat(symbolsOf(win))) {
      const e = entry(win, key);
      if (!e) continue;
      global.push(e);
      if (e.k !== 'a' && e.k !== 'v') continue;
      const d = describe(win, key);
      windowValues[e.n] = 'value' in d ? encode(d.value) : d.get ? capture(() => apply(d.get, win, [])) : { t: 'u' };
    }
    const chain = [];
    for (let level = protoOrNull(win); level !== null && protoOrNull(level) !== null; level = protoOrNull(level)) {
      chain.push({ name: levelName(level), tag: tryOr(null, () => level[Symbol.toStringTag]) ?? null, own: entries(level) });
    }
    return { global, chain, windowValues };
  };
  const top = realmShape(topWindow, topNames);
  const frame = realmShape(frameWindow, frameNames);

  // --- interfaces -----------------------------------------------------------

  const F = frameWindow;
  const interfaceList = [];
  const interfaceNames = new Set();
  const constructorNames = new Map();
  for (const name of frameNames) {
    const d = describe(F, name);
    if (d && 'value' in d && isInterface(d.value)) constructorNames.set(name, d.value);
  }
  const interfaceInfo = (n, ctor) => {
    const proto = ctor.prototype;
    const parentCtor = protoOrNull(ctor);
    const parentProto = protoOrNull(proto);
    const st = entries(ctor, keysOf(ctor), new Set(['length', 'name', 'prototype']));
    const pr = entries(proto);
    let call;
    try { ctor(); call = 'ok'; } catch (error) { call = errorText(error); }
    let construct = '';
    if (!NOT_CONSTRUCTED.has(n)) {
      try { construct = 'ok:' + classOf(new ctor()); } catch (error) { construct = errorText(error); }
    }
    return {
      n, cn: nameOf(ctor),
      p: parentCtor === null || parentCtor === F.Function.prototype ? null : nameOf(parentCtor),
      pp: parentProto === null ? null : levelName(parentProto),
      l: lengthOf(ctor), pw: Boolean(tryOr(false, () => describe(ctor, 'prototype').writable)), st, pr, call, construct,
    };
  };
  const addInterface = n => {
    if (interfaceNames.has(n)) return;
    interfaceNames.add(n);
    const ctor = constructorNames.get(n);
    // Parents first: the prototype's level, else the constructor's parent.
    const parentProto = protoOrNull(ctor.prototype);
    const parents = [parentProto && levelName(parentProto), protoOrNull(ctor) && nameOf(protoOrNull(ctor))];
    for (const parent of parents) if (parent && parent !== n && constructorNames.has(parent)) addInterface(parent);
    interfaceList.push(interfaceInfo(n, ctor));
  };
  for (const n of constructorNames.keys()) addInterface(n);
  // Interfaces with no interface object, such as External: named after their
  // prototype, and never constructed.
  const addHiddenInterface = object => {
    const proto = protoOrNull(object);
    if (proto === null) return;
    const n = levelName(proto);
    if (!n || interfaceNames.has(n) || constructorNames.has(n)) return;
    interfaceNames.add(n);
    const parentProto = protoOrNull(proto);
    interfaceList.push({
      n, cn: n, p: null, pp: parentProto === null ? null : levelName(parentProto), l: 0, pw: false, st: [], pr: entries(proto),
      call: 'TypeError: Illegal constructor.', construct: 'TypeError: Illegal constructor.',
    });
  };

  // --- instances ------------------------------------------------------------

  const singletons = {};
  const topSingletons = {};
  const defaults = {};
  const own = {};
  const describeSingleton = (win, path) => {
    const object = tryOr(undefined, () => resolve(win, path));
    if (!isObject(object)) return null;
    addHiddenInterface(object);
    return { object, shape: { iface: interfaceOf(object), values: valuesOf(object, CONNECTED_ELEMENTS.has(path) ? CONTENT : null), own: ownEntries(object) } };
  };
  // The first instance of an interface gives its defaults, and its own
  // properties when it has any. Web Storage is listed as having none.
  const addDefaults = (iface, values, ownList, object) => {
    if (!iface || defaults[iface]) return;
    defaults[iface] = values;
    if (isStorage(object)) own[iface] = [];
    else if (ownList.length) own[iface] = ownList;
  };
  const addInstance = object => addDefaults(interfaceOf(object), valuesOf(object), ownEntries(object), object);
  for (const path of SINGLETONS) {
    const found = describeSingleton(F, path);
    if (!found) continue;
    singletons[path] = found.shape;
    addDefaults(found.shape.iface, found.shape.values, found.shape.own, found.object);
    if (path === 'navigator.plugins' || path === 'navigator.mimeTypes') {
      const first = tryOr(undefined, () => found.object[0]);
      if (isObject(first)) addInstance(first);
    }
  }
  for (const path of TOP_SINGLETONS) {
    const found = describeSingleton(topWindow, path);
    if (found) topSingletons[path] = found.shape;
  }
  const doc = F.document;
  const element = doc.createElement('div');
  const instances = [
    () => F.performance.timing, () => F.performance.navigation, () => F.performance.eventCounts,
    () => element.style, () => element.classList, () => element.attributes, () => element.dataset,
    () => element.childNodes, () => element.children, () => doc.createTextNode('x'), () => doc.createComment('x'),
    () => doc.createDocumentFragment(), () => doc.createRange(), () => new F.Image(), () => new F.Audio(),
    () => new F.Option(), () => doc.createElement('canvas').getContext('2d'), () => F.matchMedia('(min-width: 1px)'),
    () => new F.XMLHttpRequest(), () => new F.AbortController(), () => new F.TextEncoder(), () => new F.TextDecoder(),
    () => new F.URL('/', F.location.href), () => new F.Blob(['x']), () => new F.Headers(), () => new F.DOMParser(),
    () => new F.MutationObserver(() => {}), () => new F.Error('x'), () => new F.DOMException('x'),
    ...EVENTS.map(name => () => new F[name]('')),
  ];
  for (const make of instances) {
    const object = tryOr(undefined, make);
    if (isObject(object)) addInstance(object);
  }
  const tagMap = {};
  for (const tag of TAGS) {
    const object = doc.createElement(tag);
    tagMap[tag] = interfaceOf(object);
    addInstance(object);
  }

  // --- namespaces, media queries, promises, builtins ------------------------

  const namespaces = {};
  for (const name of NAMESPACES) {
    const object = tryOr(undefined, () => F[name]);
    if (isObject(object)) namespaces[name] = { tag: tryOr(null, () => object[Symbol.toStringTag]) ?? null, own: entries(object) };
  }

  const mediaQueries = {};
  for (const feature of MEDIA_FEATURES) {
    const texts = feature.includes(':') ? ['(' + feature + ')', '(' + feature.replace(':', ': ') + ')'] : ['(' + feature + ')'];
    for (const text of texts) {
      const inTop = topWindow.matchMedia(text);
      mediaQueries[text] = [inTop.matches, inTop.media, F.matchMedia(text).matches];
    }
  }

  // A member whose result for an object failing its brand check is a promise
  // (rejected, so the call does nothing) returns promises.
  const promises = [];
  const stranger = Object.create(null);
  const returnsPromise = (fn) => {
    let result;
    try { result = apply(fn, stranger, []); } catch { return false; }
    return tryOr(false, () => (apply(promiseThen, result, [undefined, () => {}]), true));
  };
  for (const info of interfaceList) {
    const proto = tryOr(undefined, () => describe(F, info.n).value.prototype);
    if (!isObject(proto)) continue;
    for (const key of keysOf(proto)) {
      const d = tryOr(undefined, () => describe(proto, key));
      if (!d) continue;
      const member = info.n + '.' + keyName(key);
      if ('value' in d) {
        if (typeof d.value === 'function' && !isInterface(d.value) && isNative(d.value) && returnsPromise(d.value)) promises.push(member);
      } else if (d.get && isNative(d.get) && returnsPromise(d.get)) {
        promises.push(member + ':get');
      }
    }
  }

  // Builtins from the frame, which no page script patches; the syntax-only
  // intrinsics come from this script's realm.
  const builtinEntry = (object, key) => {
    const n = keyName(key);
    const d = tryOr(undefined, () => describe(object, key));
    if (!d) return null;
    if (!('value' in d)) return { n, k: 'a', f: flagsOf(d), g: d.get ? nameOf(d.get) : null, gl: d.get ? lengthOf(d.get) : 0, s: d.set ? nameOf(d.set) : null };
    if (typeof d.value === 'function') return { n, k: 'm', f: flagsOf(d), l: lengthOf(d.value), fn: nameOf(d.value), ctor: isConstructor(d.value) };
    if (isObject(d.value)) return { n, k: 'o', f: flagsOf(d), tag: tryOr('[object Object]', () => apply(objectTag, d.value, [])) };
    return { n, k: 'v', f: flagsOf(d), v: encode(d.value) };
  };
  const generator = function* () {};
  const asyncFunction = async function () {};
  const asyncGenerator = async function* () {};
  const typedArray = protoOf(F.Int8Array);
  const arrayIterator = protoOf(apply(F.Array.prototype.values, new F.Array(), []));
  const intrinsics = {
    '%TypedArray%': typedArray,
    '%TypedArray%.prototype': typedArray.prototype,
    '%IteratorPrototype%': protoOf(arrayIterator),
    '%ArrayIteratorPrototype%': arrayIterator,
    '%MapIteratorPrototype%': protoOf(apply(F.Map.prototype.entries, new F.Map(), [])),
    '%SetIteratorPrototype%': protoOf(apply(F.Set.prototype.values, new F.Set(), [])),
    '%StringIteratorPrototype%': protoOf(apply(F.String.prototype[Symbol.iterator], '', [])),
    '%RegExpStringIteratorPrototype%': protoOf(apply(F.RegExp.prototype[Symbol.matchAll], new F.RegExp(''), [''])),
    '%GeneratorFunction%': protoOf(generator).constructor,
    '%GeneratorFunction.prototype%': protoOf(generator),
    '%GeneratorPrototype%': protoOf(generator.prototype),
    '%AsyncFunction%': protoOf(asyncFunction).constructor,
    '%AsyncFunction.prototype%': protoOf(asyncFunction),
    '%AsyncGeneratorFunction%': protoOf(asyncGenerator).constructor,
    '%AsyncGeneratorFunction.prototype%': protoOf(asyncGenerator),
    '%AsyncGeneratorPrototype%': protoOf(asyncGenerator.prototype),
    '%AsyncIteratorPrototype%': protoOf(protoOf(asyncGenerator.prototype)),
  };
  const builtins = [];
  for (const path of BUILTINS.concat(Object.keys(intrinsics))) {
    const object = path in intrinsics ? intrinsics[path] : tryOr(undefined, () => path.split('.').reduce((o, name) => o[name], F));
    if (!isObject(object)) continue;
    builtins.push({ path, own: keysOf(object).map(key => builtinEntry(object, key)).filter(Boolean) });
  }

  const table = {
    userAgent: topWindow.navigator.userAgent,
    capturedAt: new Date().toISOString(),
    interfaces: interfaceList,
    frame, top, namespaces, defaults, own, singletons, topSingletons, tagMap, mediaQueries, builtins, promises,
  };

  // Last, so that it is not in the frame while the rest is read: the keys of
  // a new child frame once it has loaded, read before anything touches them.
  return new Promise(done => {
    const holder = doc.createElement('iframe');
    const read = () => {
      clearTimeout(timer);
      try {
        frame.freshKeys = getNames(holder.contentWindow);
      } finally {
        holder.remove();
      }
      done(JSON.stringify(table));
    };
    const timer = setTimeout(read, 5000);
    holder.addEventListener('load', read, { once: true });
    (doc.body || doc.documentElement).appendChild(holder);
  });
})();
