// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import ConfigView from './ConfigView.vue'
import type { Config } from '../api/types'

const backend = vi.hoisted(() => ({ get: vi.fn(), patch: vi.fn(), post: vi.fn() }))
vi.mock('../api/client', () => ({
  ApiError: class extends Error {},
  api: backend,
  describeError: (error: Error) => error.message,
}))

let config: Config
let wrapper: VueWrapper | undefined
async function show() {
  wrapper = mount(ConfigView)
  await flushPromises()
  return wrapper
}
const retrySwitch = () => wrapper!.get<HTMLInputElement>('input[aria-label="续接失败自动重试"]')
const retryCount = () => wrapper!.get<HTMLInputElement>('input[id="f-limits.max_continuation_retries"]')
const saveButton = () => wrapper!.get<HTMLButtonElement>('.save-bar button.accent')

beforeEach(() => {
  config = {
    bind: '127.0.0.1', port: 18451, proxy: '', browser_proxy_relay: false,
    browser_remote_allow_origins: false, browser_site_isolation: true,
    data_dir: 'data', browser_executable: '', headless: true,
    ignore_unsupported_parameters: false, search_model_drop_functions: true,
    continuation_retry_enabled: true, browser_idle_seconds: 0,
    cookie_import_max_accounts: 8, cookie_import_timeout_seconds: 180,
    limits: { request_bytes: 33554432, object_bytes: 67108864, relay_bytes: 1048576,
      queue_size: 8, queue_seconds: 30, generation_seconds: 240, stream_idle_seconds: 240,
      max_attempts: 3, max_continuation_retries: 1 },
  }
  backend.get.mockReset().mockImplementation(async () => ({ config, restart_required: false, config_file: 'config.json' }))
  backend.patch.mockReset().mockImplementation(async (_path: string, patch: Config) => {
    config = { ...config, ...patch, limits: { ...config.limits, ...patch.limits } }
    return { saved: true, applied: true, restart_required: false }
  })
  backend.post.mockReset().mockResolvedValue({ valid: true, restart_required: false })
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

it('shows the default enabled policy without creating unsaved changes', async () => {
  const view = await show()
  expect(retrySwitch().element.checked).toBe(true)
  expect(retryCount().element.value).toBe('1')
  expect(retryCount().element.disabled).toBe(false)
  expect(view.text()).toContain('整条回答共享额外补试次数')
  expect(view.find('.save-bar').exists()).toBe(false)
})

it('saves a disabled policy while preserving the count across re-enabling', async () => {
  await show()
  await retryCount().setValue('3')
  await retrySwitch().setValue(false)
  expect(retryCount().element.disabled).toBe(true)
  expect(retryCount().element.value).toBe('3')
  await saveButton().trigger('click')
  await flushPromises()
  expect(backend.patch).toHaveBeenLastCalledWith('/config', {
    continuation_retry_enabled: false, limits: { max_continuation_retries: 3 },
  })
  await retrySwitch().setValue(true)
  expect(retryCount().element.disabled).toBe(false)
  expect(retryCount().element.value).toBe('3')
  await saveButton().trigger('click')
  await flushPromises()
  expect(backend.patch).toHaveBeenLastCalledWith('/config', { continuation_retry_enabled: true })
})

it.each(['0', '17', '1.5', ''])('rejects an invalid retry count %j before saving', async (value) => {
  await show()
  await retryCount().setValue(value)
  expect(retryCount().attributes('aria-invalid')).toBe('true')
  expect(saveButton().element.disabled).toBe(true)
  expect(backend.patch).not.toHaveBeenCalled()
})

it('loads a saved disabled policy and validates the upper bound without restarting', async () => {
  config.continuation_retry_enabled = false
  config.limits!.max_continuation_retries = 5
  await show()
  expect(retryCount().element.disabled).toBe(true)
  expect(retryCount().element.value).toBe('5')
  await retrySwitch().setValue(true)
  await retryCount().setValue('16')
  const validate = wrapper!.findAll('button').find((button) => button.text() === '校验')!
  await validate.trigger('click')
  await flushPromises()
  expect(backend.post).toHaveBeenCalledWith('/config/validate', {
    continuation_retry_enabled: true, limits: { max_continuation_retries: 16 },
  })
  expect(wrapper!.text()).toContain('校验通过，保存后立即生效')
})
