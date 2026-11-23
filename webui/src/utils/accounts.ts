import type { Account, BenefitTier, QuotaStatus } from '../api/types'
import type { IconName } from '../icons'
import { formatNumber, formatRelative, formatTimestamp, isIndefinite } from './format'

export interface AccountState {
  tone: 'success' | 'caution' | 'critical' | 'neutral'
  text: string
  icon: IconName
  /** Can be picked for generation right now. */
  available: boolean
}

export const now = () => Math.floor(Date.now() / 1000)

/** Cooldowns that are still running (`until` in the future). */
export const activeCooldowns = (account: Account, at = now()) => (account.failures ?? []).filter((f) => f.until > at)

export function accountState(account: Account, at = now()): AccountState {
  if (account.disabled) return { tone: 'neutral', text: '已禁用', icon: 'person_prohibited', available: false }
  if (account.expired) return { tone: 'critical', text: '登录过期', icon: 'error_circle', available: false }
  const cooling = activeCooldowns(account, at)
  const global = cooling.find((f) => f.model === '*')
  // Permission failures never expire on their own; they need a manual clear/reset.
  if (global && isIndefinite(global.until)) return { tone: 'critical', text: '权限受限', icon: 'person_prohibited', available: false }
  // Only the account's owner can accept AI Studio's terms in a normal browser.
  if (global?.kind === 'terms_required') return { tone: 'critical', text: '需同意条款', icon: 'error_circle', available: false }
  if (global) return { tone: 'caution', text: '冷却中', icon: 'timer', available: false }
  if (cooling.length) return { tone: 'caution', text: `${cooling.length} 个模型冷却`, icon: 'timer', available: true }
  return { tone: 'success', text: '可用', icon: 'checkmark_circle', available: true }
}

const cooldownKinds: Record<string, string> = {
  auth: '认证失败',
  login_expired: '登录失效',
  permission: '权限受限（403）',
  '403': '权限受限（403）',
  permission_temporary: '临时 403',
  terms_required: '需同意条款',
  rate_limit: '限流',
  '429': '限流',
  rate_limit_minute: '每分钟额度',
  rate_limit_day: '每日额度',
  rate_limit_model: '模型额度',
}

const backendNames: Record<string, string> = {
  auto: '自动（Firefox）',
  camoufox: 'Camoufox',
  spidermonkey: 'SpiderMonkey',
}

/** Display name of a stored account backend. */
export const backendLabel = (backend: string | undefined) => (backend ? backendNames[backend] ?? backend : '—')
/** Every active backend presents a Firefox identity. */
export const backendIdentity = (_backend: string) => 'firefox'

/** Human-readable backend summary for an account row, including any active 6-hour failover switch. */
export function accountBackendSummary(account: Account, at = now()): { text: string; title: string } {
  if (!account.backend) return { text: '—', title: '' }
  const stored = backendLabel(account.backend)
  if (account.backend !== 'auto' || !account.effective_backend) {
    return { text: stored, title: `浏览器后端：${stored}` }
  }
  const active = backendLabel(account.effective_backend)
  if (account.backend_switched_until && account.backend_switched_until > at) {
    return {
      text: `${stored} → ${active}`,
      title: `浏览器后端：${stored}（已临时切换至 ${active}）`,
    }
  }
  return { text: `${stored}（${active}）`, title: `浏览器后端：${stored}，当前生效：${active}` }
}

const tierNames: Record<BenefitTier, string> = { free: 'Free', plus: 'Plus', pro: 'Pro', ultra: 'Ultra' }

/** Subscription tier badge of an account row; unknown until a page confirms the tier. */
export function tierBadge(account: Account): { text: string; known: boolean; title: string } {
  const tier = account.benefit_tier
  if (!tier || !(tier in tierNames)) return { text: '等级未知', known: false, title: '订阅等级尚未确认：账号被使用或检测一次后显示' }
  const note = tier === 'plus' ? '；额度与 Free 相同' : ''
  return { text: tierNames[tier], known: true, title: `Google AI 订阅等级：${tierNames[tier]}${note}` }
}

/** Human-readable reason of a stored cooldown kind. */
export const cooldownReason = (kind: string | undefined) => (kind ? cooldownKinds[kind] ?? kind : '—')

export const accountLabel = (account: Account | undefined, fallbackId?: string | null) =>
  account ? account.name || account.email || account.id : fallbackId || '—'

/** Token counts of the daily quota table in units of 万 (10,000). */
export function formatTokensWan(tokens: number): string {
  if (tokens < 10000) return formatNumber(tokens)
  return `${formatNumber(Math.round(tokens / 1000) / 10)} 万`
}

/** "used / limit" of a daily quota cell; without a known limit only the use. */
export const quotaCell = (used: number, limit: number | null | undefined, format: (value: number) => string = formatNumber) =>
  limit ? `${format(used)} / ${format(limit)}` : format(used)

/** Models whose daily allowance is spent today. */
export const exhaustedModels = (account: Account) => (account.daily_usage ?? []).filter((row) => row.exhausted)

/** When the daily allowances reset next. */
export const quotaResetText = (resetsAt: number | undefined, at = now()) =>
  resetsAt ? `下次重置 ${formatTimestamp(resetsAt)}（${formatRelative(resetsAt, at)}）` : ''

/**
 * The quota table's current problem: reading it failed, the service could not
 * load it, or the last download failed (`source.error`, such as
 * `quota_http_404`); the stored table stays in use meanwhile.
 */
export const quotaProblem = (status: QuotaStatus | null, requestError = '') =>
  requestError || status?.error || status?.source.error || ''

/** Where the quota table came from and when it was last checked. */
export function quotaSourceText(status: QuotaStatus | null): string {
  if (!status) return '正在读取额度数据…'
  const { source } = status
  return [
    source.published_at ? `数据版本 ${formatTimestamp(Date.parse(source.published_at) / 1000)}` : '暂无额度数据',
    `${source.models} 个模型`,
    source.embedded ? '内置快照' : `GitHub 获取于 ${formatTimestamp(source.fetched_at)}`,
    source.attempted_at ? `上次检查 ${formatTimestamp(source.attempted_at)}` : '尚未联网检查',
  ].join(' · ')
}
