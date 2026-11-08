// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import DiagnosticsView from './DiagnosticsView.vue'
import { toasts } from '../stores/ui'

const backend = vi.hoisted(() => ({ get: vi.fn(), post: vi.fn(), download: vi.fn() }))
vi.mock('../api/client', () => ({
  api: { get: backend.get, post: backend.post },
  describeError: (error: Error) => error.message,
}))
vi.mock('../stores/ui', async (original) => ({ ...(await original<typeof import('../stores/ui')>()), downloadFile: backend.download }))

const count = (total: number, last_7_days: number, today: number) => ({ total, last_7_days, today, last_at: 1792646460 })
const diagnostics = {
  version: '0.1.0',
  status: { running: true },
  logs: { sources: [] },
  events: {
    since: 1792000000,
    backends: {
      camoufox: { permission_denied: count(5, 3, 1), session_rebuild: count(2, 2, 0), novel_event: count(1, 1, 1) },
      cloakbrowser: { login_redirect: count(4, 0, 0) },
    },
  },
}
let wrapper: VueWrapper | undefined
async function show() {
  wrapper = mount(DiagnosticsView, { global: { stubs: { RouterLink: true } } })
  await flushPromises()
  return wrapper
}

beforeEach(() => {
  backend.get.mockReset().mockImplementation((path: string) => Promise.resolve(path === '/diagnostics' ? diagnostics : {}))
  backend.post.mockReset()
  backend.download.mockReset()
  toasts.splice(0)
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

it('lists every recorded event per backend with its recent counts', async () => {
  const view = await show()
  const table = view.findAll('table').find(t => t.text().includes('Camoufox'))!
  expect(table.findAll('th').map(th => th.text())).toEqual(['事件', 'Camoufox', 'CloakBrowser'])
  const rows = table.findAll('tbody tr').map(row => row.findAll('td').map(td => td.text()))
  expect(rows.map(row => row[0])).toEqual(['403（Code 7）', '跳转登录页', '就地重建会话', 'novel_event'])
  expect(rows[0][1]).toContain('5')
  expect(rows[0][1]).toContain('近 7 天 3 · 今天 1')
  expect(rows[0][2]).toBe('0')
  expect(rows[1][2]).toContain('近 7 天 0 · 今天 0')
})

it('says when nothing has been counted yet', async () => {
  backend.get.mockImplementation((path: string) => Promise.resolve(path === '/diagnostics' ? { ...diagnostics, events: { since: 1, backends: {} } } : {}))
  const view = await show()
  expect(view.text()).toContain('还没有记录到任何事件')
})

it('downloads the safe report and says what it leaves out', async () => {
  backend.post.mockResolvedValue({ report: 'ais2api-diagnostics', format: 1 })
  const view = await show()
  await view.findAll('button').find(b => b.text().includes('导出报告'))!.trigger('click')
  await flushPromises()
  expect(backend.post).toHaveBeenCalledWith('/diagnostics/export')
  expect(backend.download.mock.calls[0][1]).toContain('"ais2api-diagnostics"')
  expect(toasts.at(-1)?.message).toContain('不含 Cookie、邮箱、账号 ID')
})
