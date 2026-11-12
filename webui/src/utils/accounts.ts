import type { Account, Deprecation } from '../api/types'
import type { IconName } from '../icons'
import { isIndefinite } from './format'

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
  cloakbrowser: 'CloakBrowser',
  auto: '自动（Firefox）',
  camoufox: 'Camoufox',
  spidermonkey: 'SpiderMonkey',
}

/** Display name of a stored account backend. */
export const backendLabel = (backend: string | undefined) => (backend ? backendNames[backend] ?? backend : '—')
/** CloakBrowser presents Chrome; every other backend a Firefox identity. */
export const backendIdentity = (backend: string) => (backend === 'cloakbrowser' ? 'chrome' : 'firefox')

/** What to do about the deprecated CloakBrowser backend; empty when the installation no longer uses it. */
export function cloakbrowserNotice(deprecations: Deprecation[] | undefined): string {
  const entry = deprecations?.find((d) => d.code === 'cloakbrowser_backend')
  if (!entry) return ''
  const steps: string[] = []
  if (entry.default_backend) steps.push('在“配置”中把浏览器后端改为 Camoufox，新账号才不会再用它')
  if (entry.accounts)
    steps.push('在账号的指纹对话框中把后端改为“自动”，或运行 ais2api accounts backend --all auto --yes（Google 会把换了身份的账号看作一台新设备）')
  return `CloakBrowser 后端将在以后的版本中移除。${steps.join('；')}。`
}

/** Human-readable reason of a stored cooldown kind. */
export const cooldownReason = (kind: string | undefined) => (kind ? cooldownKinds[kind] ?? kind : '—')

export const accountLabel = (account: Account | undefined, fallbackId?: string | null) =>
  account ? account.name || account.email || account.id : fallbackId || '—'
