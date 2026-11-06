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
  expect(view.text()).toContain('1 轮未完整计价')
  expect(view.text()).toContain('传输 完成 · HTTP 200')
  expect(view.text()).toContain('首次权限错误，随后再次执行')
  expect(view.findAll('.generation-setting').map(item => item.text())).toEqual(['最大输出 Tokens65,535', '显示思考开启'])
  view.unmount()
  expect(estimateDollars({ ...cost, known_amount_nano: 0 })).toBe('已知 $0')
  expect(estimateDollars({ ...cost, known_amount_nano: null })).toBe('暂无可估算金额')
  expect(pricingCoverage({ priced: 3, partially_priced: 2, unpriced: 4 })).toBe('完整 3 次 · 部分 2 次 · 未知 2 次')
})

it('shows a recovered continuation with its retained failure and unknown fee', () => {
  const priced = { status: 'priced' as const, reason: '', currency: 'USD' as const, amount_nano: 120000 }
  const unknown = { ...priced, status: 'unpriced' as const, reason: 'incomplete_usage', amount_nano: null }
  const record: RequestDetail = { id: 'synthetic-retry', endpoint: 'gemini', model: 'example', requested_model: 'example', account: 'a', stream: true,
    status: 'succeeded', http_status: 200, generation_completed: true, delivery_completed: true, error: '', finish_reason: 'STOP',
    created_at: 1, ended_at: 5, queue_ms: 0, first_token_ms: 1, generation_ms: 4, duration_ms: 4, initial_prompt_tokens: 100,
    usage: { promptTokenCount: 300 }, settings: {},
    cost: { ...unknown, reason: 'incomplete_attempts', known_amount_nano: 240000, unpriced_attempts: 1 },
    attempts: [
      { kind: 'initial', account: 'a', started_at: 1, ended_at: 2, completed: true, finish_reason: 'MAX_TOKENS', error: '', usage: {}, cost: priced },
      { kind: 'continuation', account: 'a', started_at: 2, ended_at: 3, completed: false, finish_reason: '', error: 'permission_denied', usage: {}, cost: unknown },
      { kind: 'continuation_retry', account: 'a', started_at: 4, ended_at: 5, completed: true, finish_reason: 'STOP', error: '', usage: {}, cost: priced },
    ] }
  const view = mount(RequestDetails, { props: { record, open: true, error: '', loading: false, accountName: id => id }, global: { stubs: { Teleport: true } } })
  expect(view.text()).toContain('成功')
  expect(view.text()).toContain('共 3 次执行 · 1 次自动续接 · 1 次续接重试')
  expect(view.findAll('h3').map(item => item.text())).toEqual(['第 1 次执行 · 首轮 · a', '第 2 次执行 · 自动续接 · a', '第 3 次执行 · 续接重试 · a'])
  expect(view.text()).toContain('permission_denied')
  expect(view.text()).toContain('已知 $0.00024')
  expect(view.text()).toContain('未完整计价')
  expect(view.text()).toContain('生成 完成 · 传输 完成')
  view.unmount()
})
