// Copyright (c) 2026 Mag1cFall
// SPDX-License-Identifier: MIT
//
// The JavaScript constants of AIStudio2API internal/waa/host.go and runtime.go
// at commit e38957a084c8928977e74a1d4faba80e5e852950, byte for byte.
// Generated; edit the reference commit, not this file.
#pragma once
#include <string_view>

namespace ais2api::waa::scripts {
// Installs Function.prototype.toString, the timers and the Firefox DOM in one realm.
inline constexpr std::string_view host_script = R"js(
(input => {
  const markNative = input.markNative;
  const nativeSourceOf = input.nativeSource;
  const originalFunctionToString = Function.prototype.toString;
  const nativeFunction = (name, implementation) => {
    Object.defineProperty(implementation, 'name', { configurable: true, value: name });
    markNative(implementation, 'function ' + name + '() {\n    [native code]\n}');
    return implementation;
  };
  const functionToString = function toString() {
    'use strict';
    const marked = nativeSourceOf(this);
    if (marked !== undefined) return marked;
    return originalFunctionToString.call(this);
  };
  markNative(functionToString, 'function toString() {\n    [native code]\n}');
  Function.prototype.toString = functionToString;
  const timers = input.timers;
  globalThis.setTimeout = nativeFunction('setTimeout', function setTimeout(callback, delay, ...args) { return timers.setTimeout(callback, delay, ...args); });
  globalThis.setInterval = nativeFunction('setInterval', function setInterval(callback, delay, ...args) { return timers.setInterval(callback, delay, ...args); });
  globalThis.clearTimeout = nativeFunction('clearTimeout', function clearTimeout(id) { timers.clear(id); });
  globalThis.clearInterval = nativeFunction('clearInterval', function clearInterval(id) { timers.clear(id); });
  let domAPI;
  let idleCallbackCount = 0;
  let idleHandle = 0;
  const idleCallbacks = new Set();
  globalThis.requestIdleCallback = nativeFunction('requestIdleCallback', function requestIdleCallback(callback, options = {}) {
    const timeout = options.timeout > 0 ? Math.min(options.timeout, 50) : 0;
    const requested = input.preciseNow();
    const timedOut = timeout > 0 && idleCallbackCount++ % 2 === 1;
    const handle = ++idleHandle;
    idleCallbacks.add(handle);
    input.queueTask(timedOut ? timeout : 1, () => {
      if (!idleCallbacks.delete(handle)) return;
      const deadline = timedOut ? 0 : timeout > 0 ? requested + timeout : input.preciseNow() + 50;
      callback(domAPI.idleDeadline(timedOut, () => Math.max(0, Math.min(50, deadline - input.performanceNow()))));
    });
    return handle;
  });
  globalThis.cancelIdleCallback = nativeFunction('cancelIdleCallback', function cancelIdleCallback(handle) { idleCallbacks.delete(handle); });
  domAPI = input.installDOM(input.shape, input.profile, {
    markNative,
    brands: input.brands,
    interfaceParents: input.interfaceParents,
    isA: input.isA,
    brandCheck: input.brandCheck,
    exposed: input.exposed,
    storage: input.storage,
    realm: input.realm,
    topRealm: input.topRealm,
    trust: input.trust,
    setTimeout: globalThis.setTimeout,
    clearTimeout: globalThis.clearTimeout,
    queueTask: input.queueTask,
    incumbent: input.incumbent,
    kindOf: input.kindOf,
    transferBuffer: input.transferBuffer,
    bufferDetached: input.bufferDetached,
    throwLocation: input.throwLocation,
    callListener: input.callListener,
    digest: input.digest,
    encodingName: input.encodingName,
    decodeText: input.decodeText,
    parseURL: input.parseURL,
    updateURL: input.updateURL,
    randomUUID: input.randomUUID,
    randomBytes: input.randomBytes,
    templatedObject: input.templatedObject,
    setTemplateResolver: input.setTemplateResolver,
    performanceNow: input.performanceNow,
    timeOrigin: input.timeOrigin,
    createIframeWindow: input.createIframeWindow,
    loadImage: input.loadImage,
    loadFrame: input.loadFrame,
    resolveInterface: input.resolveInterface,
    orderKeys: input.orderKeys,
  });
  return domAPI;
})
)js";
// Types into the page's prompt box as the page's worker does and, asked to, clicks Run.
inline constexpr std::string_view prompt_script = R"js(((prompt, submit) => {
  let box = document.querySelector('ms-prompt-box');
  if (!box) {
    box = document.createElement('ms-prompt-box');
    document.body.appendChild(box);
    box.appendChild(document.createElement('textarea'));
    const run = document.createElement('ms-run-button');
    document.body.appendChild(run);
    run.appendChild(document.createElement('button'));
  }
  const textarea = box.querySelector('textarea');
  const setter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set;
  setter.call(textarea, prompt);
  textarea.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertText', data: prompt }));
  textarea.dispatchEvent(new Event('change', { bubbles: true }));
  if (submit) document.querySelector('ms-run-button').querySelector('button').click();
  return textarea.value;
}))js";
// Empties the prompt box through the native setter, without events.
inline constexpr std::string_view clear_prompt_script = R"js((() => {
  const box = document.querySelector('ms-prompt-box');
  if (box) Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set.call(box.querySelector('textarea'), '');
}))js";
// Builds arrays, snapshot options and result callbacks in the top realm.
inline constexpr std::string_view helper_script = R"js(({
  array: (...items) => items,
  options: digest => [digest === undefined ? undefined : { content: digest }, undefined, undefined, undefined],
  bind: (deliver, id) => value => deliver(id, value),
}))js";
// The callbacks the page passes to BotGuard's initialisation function.
inline constexpr std::string_view noop_script = R"js([((v, x, C, G) => {}), ((v, x) => {}), (v => {}), (v => {}), ((v, x) => {})])js";
// The source name of host scripts: SpiderMonkey, as patched, keeps scripts of
// this name out of stacks and caller lookups, like its self-hosted code.
inline constexpr std::string_view host_script_name = "self-hosted";
} // namespace ais2api::waa::scripts
