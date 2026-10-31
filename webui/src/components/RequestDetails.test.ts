// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
import { mount } from '@vue/test-utils'
import { expect, it } from 'vitest'
import type { RequestDetail } from '../api/types'
import RequestDetails from './RequestDetails.vue'
import { estimateDollars, generationSettings, pricingCoverage } from '../utils/usage'

it('formats recorded settings without losing zero, false or small numeric values', () => {
  const settings = generationSettings({ maxOutputTokens: 65535, temperature: 0.00001, seed: 0,
    thinkingConfig: { includeThoughts: false, thinkingLevel: 'HIGH' } })
  expect(settings.map(item => [item.label, item.value])).toEqual([
    ['最大输出 Tokens', '65,535'], ['温度', '0.00001'], ['随机种子', '0'], ['显示思考', '关闭'], ['思考等级', '高'],
  ])
})
it('shows a failed generation independently of its known cost and completed delivery', () => {
  const cost = { status: 'unpriced' as const, reason: 'incomplete_attempts', currency: 'USD' as const,
    amount_nano: null, known_amount_nano: 172080000, unpriced_attempts: 1 }
  const record: RequestDetail = { id: 'synthetic', endpoint: 'chat_completions', model: 'example', requested_model: 'example', account: 'a', stream: true,
    status: 'failed', http_status: 200, generation_completed: false, delivery_completed: true, error: 'content_filter', finish_reason: 'PROHIBITED_CONTENT',
    created_at: 1, ended_at: 2, queue_ms: 0, first_token_ms: 1, generation_ms: 1, duration_ms: 1, usage: {}, cost,
    settings: { maxOutputTokens: 65535, thinkingConfig: { includeThoughts: true } },
    attempts: [{ account: 'a', started_at: 1, ended_at: 2, completed: false, finish_reason: '', error: 'permission_denied', usage: {}, cost },
      { account: 'a', started_at: 2, ended_at: 3, completed: true, finish_reason: 'PROHIBITED_CONTENT', error: '', usage: {},
        cost: { status: 'priced', reason: '', currency: 'USD', amount_nano: 172080000 } }] }
  const view = mount(RequestDetails, { props: { record, open: true, error: '', loading: false, accountName: id => id }, global: { stubs: { Teleport: true } } })
  expect(view.text()).toContain('失败 · 上游内容拦截')
  expect(view.text()).toContain('已知 $0.17208')
  expect(view.text()).toContain('1 次尝试未完整计价')
  expect(view.text()).toContain('传输 完成 · HTTP 200')
  expect(view.text()).toContain('首次权限错误，随后再次执行')
  expect(view.findAll('.generation-setting').map(item => item.text())).toEqual(['最大输出 Tokens65,535', '显示思考开启'])
  view.unmount()
  expect(estimateDollars({ ...cost, known_amount_nano: 0 })).toBe('已知 $0')
  expect(estimateDollars({ ...cost, known_amount_nano: null })).toBe('暂无可估算金额')
  expect(pricingCoverage({ priced: 3, partially_priced: 2, unpriced: 4 })).toBe('完整 3 次 · 部分 2 次 · 未知 2 次')
})
