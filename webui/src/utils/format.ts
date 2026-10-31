const compact = new Intl.NumberFormat('zh-CN', { notation: 'compact', maximumFractionDigits: 1 })
const plain = new Intl.NumberFormat('zh-CN')

export const formatNumber = (value: number | undefined | null) => (value == null ? '—' : plain.format(value))

/** 1,284 / 1.3万 style, used for stat-tile values. */
export const formatCompact = (value: number | undefined | null) =>
  value == null ? '—' : Math.abs(value) < 10000 ? plain.format(value) : compact.format(value)

export function formatBytes(bytes: number | undefined | null): string {
  if (bytes == null) return '—'
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit++
  }
  return `${unit === 0 ? value : value.toFixed(value < 10 ? 2 : 1)} ${units[unit]}`
}

export function formatDuration(seconds: number | undefined | null): string {
  if (seconds == null) return '—'
  const s = Math.max(0, Math.floor(seconds))
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  const m = Math.floor((s % 3600) / 60)
  if (d) return `${d} 天 ${h} 小时`
  if (h) return `${h} 小时 ${m} 分`
  if (m) return `${m} 分 ${s % 60} 秒`
  return `${s} 秒`
}

const dateTime = new Intl.DateTimeFormat('zh-CN', {
  year: 'numeric',
  month: '2-digit',
  day: '2-digit',
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  hour12: false,
})
const timeOnly = new Intl.DateTimeFormat('zh-CN', { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false })

/**
 * Deadlines beyond year 9999 mean "no expiry": the backend stores INT64_MAX
 * for permission cooldowns, which is also outside the JavaScript Date range
 * (formatting such a Date throws a RangeError).
 */
export const isIndefinite = (unix: number) => unix >= 253402300800

/** Unix seconds → local date-time. */
export const formatTimestamp = (unix: number | undefined | null) =>
  !unix ? '—' : isIndefinite(unix) ? '无限期' : dateTime.format(new Date(unix * 1000))
export const formatTime = (unix: number | undefined | null) =>
  !unix ? '—' : isIndefinite(unix) ? '无限期' : timeOnly.format(new Date(unix * 1000))

export function formatRelative(unix: number | undefined | null, now = Date.now() / 1000): string {
  if (!unix) return '—'
  if (isIndefinite(unix)) return '无限期'
  const diff = unix - now
  const abs = Math.abs(diff)
  const text = abs < 60 ? `${Math.round(abs)} 秒` : abs < 3600 ? `${Math.round(abs / 60)} 分钟` : abs < 86400 ? `${Math.round(abs / 3600)} 小时` : `${Math.round(abs / 86400)} 天`
  return diff >= 0 ? `${text}后` : `${text}前`
}

export const percent = (value: number, total: number) => (total > 0 ? value / total : 0)
export const formatPercent = (ratio: number) => `${(ratio * 100).toFixed(ratio >= 0.995 || ratio === 0 ? 0 : 1)}%`

/** Strips the `models/` prefix used by the Gemini model list. */
export const modelId = (name: string | undefined) => (name ?? '').replace(/^models\//, '')

/** Sum token-like counters from accumulated usage metadata. */
export function usageTotals(usage: Record<string, unknown> | undefined) {
  const read = (key: string) => (typeof usage?.[key] === 'number' ? (usage[key] as number) : 0)
  const prompt = read('promptTokenCount') || read('prompt_tokens') || read('input_tokens')
  const output = read('candidatesTokenCount') || read('completion_tokens') || read('output_tokens')
  const thoughts = read('thoughtsTokenCount')
  const total = read('totalTokenCount') || read('total_tokens') || prompt + output + read('toolUsePromptTokenCount')
  return { prompt, output, thoughts, total }
}
