import type { BrowserDiagnostics } from '../api/types'

// Labels for the browser adapter state reported by GET /diagnostics → browser.
// Unknown values fall back to the raw identifier so new backend states still show.
const PHASES: Record<string, string> = {
  idle: '空闲',
  active: '处理中',
  browser_start: '启动浏览器',
  account_identity: '确认账号身份',
  bootstrap_prepare: '引导：准备页面',
  bootstrap_run: '引导：运行',
  bootstrap_capture: '引导：捕获请求模板',
  bootstrap_finish: '引导：收尾',
  ready: '就绪',
  snapshot_ticket: '获取请求快照',
  request_authorization: '请求授权',
  generation_upload: '上传请求内容',
  generation_start: '开始生成',
  generation_read: '读取生成输出',
  generation_complete: '生成完成',
}

const STATUSES: Record<string, { text: string; tone: 'success' | 'caution' | 'critical' | 'neutral' }> = {
  not_started: { text: '未启动', tone: 'neutral' },
  closed: { text: '已关闭', tone: 'neutral' },
  idle_closed: { text: '空闲已回收', tone: 'neutral' },
  busy: { text: '忙碌', tone: 'neutral' },
  browser_ready: { text: '浏览器已连接', tone: 'neutral' },
  account_ready: { text: '账号已就绪', tone: 'neutral' },
  gateway_ready: { text: '适配器就绪', tone: 'success' },
  bootstrap_snapshot_missing: { text: '未找到快照服务', tone: 'critical' },
  bootstrap_template_missing: { text: '未捕获请求模板', tone: 'critical' },
}

export const browserPhase = (phase: string | undefined) => (phase ? (PHASES[phase] ?? phase) : '—')

export function browserStatus(status: string | undefined) {
  if (!status) return { text: '—', tone: 'neutral' as const }
  return STATUSES[status] ?? { text: status, tone: status.endsWith('_missing') ? ('critical' as const) : ('neutral' as const) }
}

const COOKIE_REFRESH: Record<string, string> = {
  saved: '已写回最新 Cookie',
  unchanged: '无变化',
  failed: '写回失败（沿用已保存的 Cookie）',
  not_attempted: '尚未执行',
}

const PROXY_MODES: Record<string, string> = { none: '不使用代理', direct: '浏览器直连代理', relay: '本机转发代理' }
export const proxyModeLabel = (mode: string | undefined) => (mode ? (PROXY_MODES[mode] ?? mode) : '—')

export const cookieRefreshLabel = (value: string | undefined) => (value ? (COOKIE_REFRESH[value] ?? value) : '—')

export function modelDiscoveryLabel(value: BrowserDiagnostics['modelDiscovery']) {
  if (value === 'live_rpc') return '已完成实时发现'
  if (value === 'not_loaded') return '尚未加载'
  return value ?? '—'
}
