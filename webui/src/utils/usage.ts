// Copyright (c) 2026 ais2api contributors
// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
import type { RequestDetail, RequestEstimate, RequestRecord, RequestStatus, UsageTotal } from '../api/types'
import { formatNumber } from './format'

export const ranges = { all: '累计', today: '今天', yesterday: '昨天', week: '近 7 天', month: '本月', custom: '自定义' } as const
export type UsageRange = keyof typeof ranges
export const requestStatuses: Record<RequestStatus, string> = {
  queued: '排队中', running: '生成中', finishing: '传输中', succeeded: '成功', failed: '失败', cancelled: '已取消', interrupted: '已中断',
}
export const activeRequest = (status: RequestStatus) => ['queued', 'running', 'finishing'].includes(status)
export const dollars = (nano: number | null | undefined) => nano == null ? '未完整计价'
  : nano === 0 ? '$0' : `$${(nano / 1e9).toFixed(9).replace(/0+$/, '').replace(/\.$/, '')}`
export const rateDollars = (pico: number | null) => pico == null ? '缺价' : `$${pico / 1e6} / 百万 Tokens`
export const costReasons: Record<string, string> = {
  pending: '等待请求结束', missing_usage: '缺少完整用量', invalid_usage: '用量格式无效', inconsistent_usage: '用量明细与总量不一致',
  incomplete_usage: '本次尝试未完整结束', missing_price: '没有对应参考价格', unsupported_charge_dimensions: '包含尚不能完整估算的收费项目',
  unsupported_price_tier: '暂不支持此价格阶梯', incomplete_attempts: '部分尝试未完整计价', service_interrupted: '服务中断，用量可能不完整',
}
export const costKinds: Record<string, string> = { input: '非缓存输入', cache_read: '缓存读取', output: '输出', reasoning: '思考' }

export const finishReasons: Record<string, string> = {
  STOP: '正常结束', MAX_TOKENS: '达到输出上限', SAFETY: '上游内容拦截', RECITATION: '上游引用内容拦截',
  BLOCKLIST: '上游词表拦截', PROHIBITED_CONTENT: '上游内容拦截', SPII: '上游隐私内容拦截',
  IMAGE_SAFETY: '上游图片内容拦截', IMAGE_PROHIBITED_CONTENT: '上游图片内容拦截', IMAGE_RECITATION: '上游图片引用拦截',
  LANGUAGE: '不支持的语言', MALFORMED_FUNCTION_CALL: '工具调用格式错误', UNEXPECTED_TOOL_CALL: '意外的工具调用',
  TOO_MANY_TOOL_CALLS: '工具调用次数过多', MISSING_THOUGHT_SIGNATURE: '缺少思考签名', NO_IMAGE: '未生成预期图片',
  IMAGE_OTHER: '图片生成异常', OTHER: '其他结束原因',
}
export const finishLabel = (reason: string) => finishReasons[reason] ?? reason
export const requestStatusLabel = (record: Pick<RequestRecord, 'status' | 'error'>) =>
  record.status === 'failed' && record.error === 'content_filter' ? '失败 · 上游内容拦截' : requestStatuses[record.status]
export const estimateDollars = (cost: RequestEstimate) => cost.status === 'priced' ? dollars(cost.amount_nano)
  : cost.known_amount_nano == null ? '暂无可估算金额' : `已知 ${dollars(cost.known_amount_nano)}`
export const estimateCoverage = (cost: RequestEstimate) => cost.status === 'priced' ? '完整计价'
  : cost.known_amount_nano == null ? costReasons[cost.reason] ?? cost.reason
    : cost.unpriced_attempts ? `部分计价 · ${formatNumber(cost.unpriced_attempts)} 次尝试未完整计价`
      : `部分计价 · ${costReasons[cost.reason] ?? cost.reason}`
export const pricingCoverage = (total: Pick<UsageTotal, 'priced' | 'partially_priced' | 'unpriced'>) =>
  `完整 ${formatNumber(total.priced)} 次 · 部分 ${formatNumber(total.partially_priced)} 次 · 未知 ${formatNumber(total.unpriced - total.partially_priced)} 次`
const settingNames: Record<string, string> = {
  temperature: '温度', topP: '核采样概率', topK: '采样候选数', candidateCount: '候选回答数', maxOutputTokens: '最大输出 Tokens',
  seed: '随机种子', presencePenalty: '存在惩罚', frequencyPenalty: '频率惩罚', responseMimeType: '输出格式',
  thinkingBudget: '思考预算 Tokens', includeThoughts: '显示思考', thinkingLevel: '思考等级',
}
const settingValues: Record<string, string> = { LOW: '低', MEDIUM: '中', HIGH: '高', MINIMAL: '最低', 'text/plain': '文本', 'application/json': 'JSON' }
export function generationSettings(settings: RequestDetail['settings']) {
  const number = new Intl.NumberFormat(undefined, { maximumFractionDigits: 20 })
  return Object.entries(settings).flatMap(([key, value]) => {
    const entries = typeof value === 'object' ? Object.entries(value).map(([nested, item]) => [`${key}.${nested}`, nested, item] as const) : [[key, key, value] as const]
    return entries.map(([id, name, item]) => ({ key: id, label: settingNames[name] ?? name,
      value: typeof item === 'boolean' ? item ? '开启' : '关闭' : typeof item === 'number' ? number.format(item) : settingValues[item] ?? item }))
  })
}
const attemptErrors: Record<string, string> = { permission_denied: '权限错误', upstream_error: '上游错误', cancelled: '请求取消',
  client_disconnected: '客户端断开', browser_incompatible: '浏览器会话异常', transient_rpc_route: '上游路由异常',
  upstream_no_content: '上游未返回内容', session_expired: '会话过期' }
export function attemptsSummary(attempts: RequestDetail['attempts']) {
  const first = attempts[0]?.error
  return `共 ${attempts.length} 次尝试` + (attempts.length > 1 && first ? ` · 首次${attemptErrors[first] ?? first}，随后再次执行` : '')
}

function localDate(value: string): Date {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value)) throw new Error('请选择完整的起止日期。')
  const [year, month, day] = value.split('-').map(Number)
  const date = new Date(year, month - 1, day)
  if (date.getFullYear() !== year || date.getMonth() !== month - 1 || date.getDate() !== day) throw new Error('日期无效。')
  return date
}
/** Local calendar arithmetic keeps days correct across daylight-saving changes. */
export function usageBounds(range: UsageRange, now = new Date(), start = '', end = ''): { from?: number; to?: number } {
  if (range === 'all') return {}
  const today = new Date(now.getFullYear(), now.getMonth(), now.getDate())
  let from = new Date(today), to = new Date(today)
  to.setDate(to.getDate() + 1)
  if (range === 'yesterday') { from.setDate(from.getDate() - 1); to = today }
  if (range === 'week') from.setDate(from.getDate() - 6)
  if (range === 'month') from.setDate(1)
  if (range === 'custom') { from = localDate(start); to = localDate(end); to.setDate(to.getDate() + 1) }
  if (from >= to) throw new Error('结束日期不能早于开始日期。')
  return { from: from.getTime(), to: to.getTime() }
}
