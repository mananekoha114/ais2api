// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import ModelsView from './ModelsView.vue'
import { toasts } from '../stores/ui'

const backend = vi.hoisted(() => ({ get: vi.fn(), discover: vi.fn() }))
vi.mock('../api/client', () => ({
  api: { get: backend.get },
  runOperation: backend.discover,
  describeError: (error: Error) => error.message,
}))

const saved = {
  models: [{ name: 'models/gemini-old', displayName: 'Old model', source: 'saved' }],
  source: 'saved', account_id: 'account-a', saved_at: 1792646460,
}
let wrapper: VueWrapper | undefined
async function show() {
  wrapper = mount(ModelsView, { global: { stubs: { RouterLink: true } } })
  await flushPromises()
  return wrapper
}

beforeEach(() => {
  backend.get.mockReset().mockImplementation((path: string) => Promise.resolve(path === '/models' ? saved : { config: {} }))
  backend.discover.mockReset()
  toasts.splice(0)
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

it('loads saved models and their timestamp without starting discovery', async () => {
  const view = await show()
  expect(view.text()).toContain('Old model')
  expect(view.text()).toContain('来源：实时发现')
  expect(view.get('time').attributes('datetime')).toBe(new Date(saved.saved_at * 1000).toISOString())
  expect(backend.discover).not.toHaveBeenCalled()
})

it('shows configured model metadata without a discovery timestamp', async () => {
  backend.get.mockImplementation((path: string) => Promise.resolve(path === '/models'
    ? { models: [{ name: 'models/manual', source: 'configured' }], source: 'configured', account_id: 'account-a', saved_at: null }
    : { config: {} }))
  const view = await show()
  expect(view.text()).toContain('manual')
  expect(view.text()).toContain('来源：手动配置')
  expect(view.find('time').exists()).toBe(false)
  expect(backend.discover).not.toHaveBeenCalled()
})

it('replaces configured models and their source after a successful discovery', async () => {
  backend.get.mockImplementation((path: string) => Promise.resolve(path === '/models'
    ? { models: saved.models, source: 'configured', account_id: 'account-a', saved_at: null }
    : { config: {} }))
  const view = await show()
  backend.discover.mockResolvedValue({
    ...saved, models: [{ name: 'models/gemini-new', displayName: 'New model', source: 'saved' }], saved_at: saved.saved_at + 60,
  })
  await view.get('button.accent').trigger('click')
  await flushPromises()
  expect(backend.discover).toHaveBeenCalledWith('/models/refresh', expect.any(Object))
  expect(view.text()).toContain('New model')
  expect(view.text()).not.toContain('Old model')
  expect(view.text()).toContain('来源：实时发现')
  expect(view.text()).not.toContain('来源：手动配置')
  expect(view.get('time').attributes('datetime')).toBe(new Date((saved.saved_at + 60) * 1000).toISOString())
  expect(toasts.map((toast) => toast.title)).toEqual(['已更新 1 个模型'])
})

it('keeps the saved list and shows the error when discovery or persistence fails', async () => {
  const view = await show()
  backend.discover.mockRejectedValue(new Error('storage failed'))
  await view.get('button.accent').trigger('click')
  await flushPromises()
  expect(view.text()).toContain('Old model')
  expect(view.findAll('[role="alert"]')).toHaveLength(1)
  expect(view.get('[role="alert"]').text()).toContain('发现失败')
  expect(view.get('[role="alert"]').text()).toContain('storage failed')
  expect(view.get('time').attributes('datetime')).toBe(new Date(saved.saved_at * 1000).toISOString())
  expect(toasts).toHaveLength(0)
})

it('shows an empty local list without claiming discovery has succeeded', async () => {
  backend.get.mockImplementation((path: string) => Promise.resolve(path === '/models'
    ? { models: [], source: 'none', account_id: null, saved_at: null } : { config: {} }))
  const view = await show()
  expect(view.text()).toContain('暂无模型')
  expect(view.find('time').exists()).toBe(false)
  expect(view.text()).not.toContain('来源：')
  expect(backend.discover).not.toHaveBeenCalled()
})
