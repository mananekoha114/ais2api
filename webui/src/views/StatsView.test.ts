// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createMemoryHistory, createRouter, type Router } from 'vue-router'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import StatsView from './StatsView.vue'
import RequestDetails from '../components/RequestDetails.vue'
import StatTile from '../components/StatTile.vue'
import { usageBounds } from '../utils/usage'

const backend = vi.hoisted(() => ({ get: vi.fn(), post: vi.fn(), delete: vi.fn() }))
vi.mock('../api/client', () => ({ api: backend, describeError: (error: Error) => error.message }))
const totals = { success: 1, failure: 0, priced: 1, unpriced: 0, amount_nano: 120000, known_amount_nano: 120000, partially_priced: 0, usage: { totalTokenCount: 110 } }
const row = { id: 'request-one', model: 'example', requested_model: 'alias', endpoint: 'gemini', account: 'a', status: 'succeeded', stream: false,
  created_at: new Date(2026, 9, 29, 10).getTime(), ended_at: new Date(2026, 9, 29, 10, 0, 1).getTime(), duration_ms: 1000,
  generation_completed: true, delivery_completed: true, http_status: 200, error: '', finish_reason: 'STOP', usage: { totalTokenCount: 110 },
  cost: { status: 'unpriced', reason: 'missing_price', amount_nano: null, known_amount_nano: null, unpriced_attempts: 1, currency: 'USD' }, attempt_count: 1 }
let wrapper: VueWrapper | undefined, router: Router
const requestCalls = () => backend.get.mock.calls.filter(([path]) => path === '/usage/requests')
const button = (text: string) => wrapper!.findAll('button').find(item => item.text() === text)!
async function show(url = '/stats') {
  router = createRouter({ history: createMemoryHistory(), routes: [{ path: '/stats', component: StatsView }] })
  await router.push(url); await router.isReady()
  wrapper = mount(StatsView, { global: { plugins: [router], stubs: { Teleport: true } } })
  await flushPromises(); return wrapper
}
beforeEach(() => {
  vi.useFakeTimers(); vi.setSystemTime(new Date(2026, 9, 29, 12))
  Object.defineProperty(document, 'hidden', { configurable: true, value: false })
  backend.get.mockReset().mockImplementation((path: string) => {
    if (path === '/usage/summary') return Promise.resolve({ records: [{ ...totals, account: 'a', model: 'example' }], totals, legacy_records: [], scope: 'lifetime', since: 1, detail_since: 1 })
    if (path === '/usage/requests') return Promise.resolve({ records: [row], total: 70, snapshot: 80, next_cursor: 30, since: 1, detail_since: 1 })
    if (path.startsWith('/usage/requests/')) return Promise.resolve({ ...row, settings: {}, attempts: [] })
    if (path === '/accounts') return Promise.resolve({ accounts: [{ id: 'a', name: 'Account A' }] })
    return Promise.resolve({ sources: [], refreshing: false, error: '' })
  })
  backend.post.mockReset(); backend.delete.mockReset()
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined; vi.useRealTimers() })

it('opens lifetime overview and puts the total cost in its KPI cards', async () => {
  const view = await show()
  const query = backend.get.mock.calls.find(([path]) => path === '/usage/summary')![1].query
  expect(query.from).toBeUndefined()
  expect(query.scope).toBe('lifetime')
  expect(view.findAllComponents(StatTile)).toHaveLength(4)
  expect(view.text()).toContain('累计已知费用')
  expect(view.text()).toContain('$0.00012')
  await button('请求记录').trigger('click'); await flushPromises()
  expect(requestCalls().at(-1)![1].query).toMatchObject(usageBounds('today', new Date()))
  expect(view.findAllComponents(StatTile)).toHaveLength(0)
  expect(view.text()).toContain('暂无可估算金额')
})
it('carries filters between records and overview and preserves a detail return', async () => {
  const view = await show('/stats?tab=requests&range=week&model=example&account=a')
  await view.get('.request-link').trigger('click'); await flushPromises()
  expect(view.findComponent(RequestDetails).props('record')?.id).toBe('request-one')
  const prior = router.currentRoute.value.fullPath
  await button('关闭').trigger('click')
  expect(router.currentRoute.value.fullPath).toBe(prior)
  await button('查看此范围统计').trigger('click'); await flushPromises()
  expect(router.currentRoute.value.query).toMatchObject({ tab: 'overview', range: 'week', model: 'example', account: 'a' })
  expect(backend.get.mock.calls.filter(([path]) => path === '/usage/summary').at(-1)![1].query).toMatchObject({ model: 'example', account: 'a', ...usageBounds('week', new Date()) })
})
it('keeps a pagination snapshot until explicit refresh, including when returning to page one', async () => {
  await show('/stats?tab=requests')
  await button('下一页').trigger('click'); await flushPromises()
  expect(requestCalls().at(-1)![1].query).toMatchObject({ before: '30', snapshot: '80' })
  const count = requestCalls().length
  await vi.advanceTimersByTimeAsync(10000)
  expect(requestCalls()).toHaveLength(count)
  await button('上一页').trigger('click'); await flushPromises()
  expect(requestCalls().at(-1)![1].query.snapshot).toBe('80')
  await button('刷新').trigger('click'); await flushPromises()
  expect(requestCalls().at(-1)![1].query.snapshot).toBeUndefined()
})
it('pauses refresh for details and hidden pages and updates today after midnight', async () => {
  const view = await show('/stats?tab=requests')
  await view.get('.request-link').trigger('click'); await flushPromises()
  const count = requestCalls().length
  await vi.advanceTimersByTimeAsync(10000)
  expect(requestCalls()).toHaveLength(count)
  await button('关闭').trigger('click')
  Object.defineProperty(document, 'hidden', { configurable: true, value: true })
  await vi.advanceTimersByTimeAsync(10000)
  expect(requestCalls()).toHaveLength(count)
  vi.setSystemTime(new Date(2026, 9, 30, 0, 1))
  Object.defineProperty(document, 'hidden', { configurable: true, value: false })
  document.dispatchEvent(new Event('visibilitychange')); await flushPromises()
  expect(requestCalls().at(-1)![1].query).toMatchObject(usageBounds('today', new Date()))
})
it('does not overlap automatic history loads', async () => {
  await show('/stats?tab=requests')
  let finish: (value: unknown) => void = () => {}
  backend.get.mockImplementation((path: string) => path === '/usage/requests'
    ? new Promise(resolve => { finish = resolve }) : Promise.resolve({ sources: [], refreshing: false }))
  await vi.advanceTimersByTimeAsync(10000)
  const count = requestCalls().length
  await vi.advanceTimersByTimeAsync(30000)
  expect(requestCalls()).toHaveLength(count)
  finish({ records: [], total: 0, snapshot: 0, next_cursor: null, since: 1, detail_since: 1 }); await flushPromises()
})
