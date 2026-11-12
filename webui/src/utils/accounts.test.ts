import { describe, expect, it } from 'vitest'
import { accountState, cloakbrowserNotice, cooldownReason } from './accounts'
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

describe('cloakbrowserNotice', () => {
  it('is empty when nothing uses CloakBrowser', () => {
    expect(cloakbrowserNotice(undefined)).toBe('')
    expect(cloakbrowserNotice([])).toBe('')
    expect(cloakbrowserNotice([{ code: 'something_else' }])).toBe('')
  })
  it('names the step for the default backend, for stored accounts, or both', () => {
    const byDefault = cloakbrowserNotice([{ code: 'cloakbrowser_backend', default_backend: true, accounts: false }])
    expect(byDefault).toContain('浏览器后端改为 Camoufox')
    expect(byDefault).not.toContain('accounts backend')
    const accounts = cloakbrowserNotice([{ code: 'cloakbrowser_backend', default_backend: false, accounts: true }])
    expect(accounts).toContain('accounts backend --all auto --yes')
    expect(accounts).not.toContain('浏览器后端改为 Camoufox')
    const both = cloakbrowserNotice([{ code: 'cloakbrowser_backend', default_backend: true, accounts: true }])
    expect(both).toContain('浏览器后端改为 Camoufox')
    expect(both).toContain('accounts backend --all auto --yes')
  })
})
