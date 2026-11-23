import { describe, expect, it } from 'vitest'
import {
  accountBackendSummary,
  accountState,
  cooldownReason,
  exhaustedModels,
  formatTokensWan,
  quotaCell,
  quotaResetText,
  quotaSourceText,
  tierBadge,
} from './accounts'
import { formatRelative, formatTime, formatTimestamp } from './format'

// The backend stores INT64_MAX as the deadline of permission cooldowns; after
// JSON parsing it is far outside the JavaScript Date range.
const FOREVER = JSON.parse('9223372036854775807') as number
const NOW = 1_800_000_000

describe('indefinite cooldown deadlines', () => {
  it('formats without throwing', () => {
    expect(formatTimestamp(FOREVER)).toBe('无限期')
    expect(formatTime(FOREVER)).toBe('无限期')
    expect(formatRelative(FOREVER, NOW)).toBe('无限期')
  })

  it('marks a global permission cooldown as needing manual recovery', () => {
    const state = accountState({ id: 'a', failures: [{ model: '*', until: FOREVER, count: 1 }] }, NOW)
    expect(state).toMatchObject({ tone: 'critical', available: false, text: '权限受限' })
  })

  it('keeps timed quota cooldowns as ordinary cooldowns', () => {
    expect(accountState({ id: 'a', failures: [{ model: '*', until: NOW + 3600, count: 0 }] }, NOW)).toMatchObject({
      tone: 'caution',
      available: false,
    })
    expect(accountState({ id: 'a', failures: [{ model: 'gemini-pro', until: NOW + 60, count: 0 }] }, NOW)).toMatchObject({
      available: true,
      text: '1 个模型冷却',
    })
    expect(accountState({ id: 'a', failures: [{ model: '*', until: NOW - 1, count: 2 }] }, NOW).available).toBe(true)
  })

  it('tells the owner to accept pending AI Studio terms', () => {
    const failures = [{ model: '*', until: NOW + 3600, count: 1, kind: 'terms_required' }]
    expect(accountState({ id: 'a', failures }, NOW)).toMatchObject({ tone: 'critical', available: false, text: '需同意条款' })
    expect(cooldownReason('terms_required')).toBe('需同意条款')
    expect(cooldownReason('something_new')).toBe('something_new')
  })
})

describe('accountBackendSummary', () => {
  it('shows effective backend and temporary 6-hour switch state for auto accounts', () => {
    expect(accountBackendSummary({ id: 'a', backend: 'camoufox', effective_backend: 'camoufox' }, NOW).text).toBe('Camoufox')
    expect(accountBackendSummary({ id: 'a', backend: 'auto', effective_backend: 'camoufox' }, NOW).text).toBe('自动（Firefox）（Camoufox）')
    const switched = accountBackendSummary(
      { id: 'a', backend: 'auto', effective_backend: 'spidermonkey', backend_switched_until: NOW + 3600 },
      NOW,
    )
    expect(switched.text).toBe('自动（Firefox） → SpiderMonkey')
    expect(switched.title).toContain('已临时切换至 SpiderMonkey')
    expect(
      accountBackendSummary(
        { id: 'a', backend: 'auto', effective_backend: 'camoufox', backend_switched_until: NOW - 1 },
        NOW,
      ).text,
    ).toBe('自动（Firefox）（Camoufox）')
  })
})

describe('subscription tier badge', () => {
  it('names confirmed tiers and notes the plus allowance', () => {
    expect(tierBadge({ id: 'a', benefit_tier: 'ultra' })).toMatchObject({ text: 'Ultra', known: true })
    expect(tierBadge({ id: 'a', benefit_tier: 'plus' }).title).toContain('额度与 Free 相同')
    expect(tierBadge({ id: 'a', benefit_tier: 'free' }).text).toBe('Free')
  })

  it('stays unknown until a page confirms the tier', () => {
    expect(tierBadge({ id: 'a' })).toMatchObject({ text: '等级未知', known: false })
    expect(tierBadge({ id: 'a', benefit_tier: '' }).known).toBe(false)
  })
})

describe('daily quota formatting', () => {
  it('shows tokens in units of 万 and cells as used / limit', () => {
    expect(formatTokensWan(6)).toBe('6')
    expect(formatTokensWan(12_345)).toBe('1.2 万')
    expect(formatTokensWan(15_000_000)).toBe('1,500 万')
    expect(quotaCell(4, 50)).toBe('4 / 50')
    expect(quotaCell(4, null)).toBe('4')
    expect(quotaCell(150_000, 5_000_000, formatTokensWan)).toBe('15 万 / 500 万')
  })

  it('lists spent models and the next reset', () => {
    const account = {
      id: 'a',
      daily_usage: [
        { model: 'm-a', requests: 50, tokens: 0, request_limit: 50, token_limit: 1000, exhausted: true },
        { model: 'm-b', requests: 1, tokens: 0, request_limit: null, token_limit: null, exhausted: false },
      ],
    }
    expect(exhaustedModels(account).map((row) => row.model)).toEqual(['m-a'])
    expect(exhaustedModels({ id: 'b' })).toEqual([])
    expect(quotaResetText(undefined, NOW)).toBe('')
    expect(quotaResetText(NOW + 7200, NOW)).toBe(`下次重置 ${formatTimestamp(NOW + 7200)}（2 小时后）`)
  })

  it('describes where the quota table came from', () => {
    const status = {
      refreshing: false,
      error: '',
      source: { url: '', fetched_at: 0, attempted_at: 0, error: '', published_at: '2026-11-23T00:00:00Z', models: 10, embedded: true },
      models: {},
    }
    expect(quotaSourceText(null)).toBe('正在读取额度数据…')
    expect(quotaSourceText(status)).toBe(
      `数据版本 ${formatTimestamp(Date.parse('2026-11-23T00:00:00Z') / 1000)} · 10 个模型 · 内置快照 · 尚未联网检查`,
    )
    expect(quotaSourceText({ ...status, source: { ...status.source, embedded: false, fetched_at: NOW, attempted_at: NOW } })).toContain(
      `GitHub 获取于 ${formatTimestamp(NOW)} · 上次检查 ${formatTimestamp(NOW)}`,
    )
  })
})
