import { session, signOut } from '../stores/session'
import type { ApiErrorBody, Operation } from './types'

export const API_PREFIX = '/api/admin/v1'

export class ApiError extends Error {
  constructor(
    message: string,
    /** HTTP status; 0 when the request never reached the backend. */
    readonly status: number,
    /** Backend reason code such as `admin_authentication` or `restart_required`. */
    readonly reason = '',
  ) {
    super(message)
    this.name = 'ApiError'
  }
}

const REASONS: Record<string, string> = {
  admin_disabled: '后端未启用管理 API：请在 config.json 中设置 admin_key 后重启或执行 config reload。',
  admin_origin: '后端拒绝了此来源：请把控制台地址加入 admin_origins，或改用同源代理方式访问。',
  admin_authentication: '管理密钥无效。',
  restart_required: '配置中包含需要重启才能生效的变更。',
  queue_full: '任务队列已满，请稍后再试。',
  operations_full: '保留的维护任务过多，请稍后再试。',
  no_available_account: '没有可用的账号（均已禁用、过期或处于冷却中）。',
  account_not_found: '账号不存在。',
  operation_missing: '任务不存在或已过期。',
  log_truncated: '日志文件已被截断，将从头重新读取。',
  log_missing: '日志源不存在。',
  identity_unverified: '无法确认 Cookie 对应的账号身份。',
  cookie_not_authenticated: 'Cookie 未登录或已失效。',
  cookie_region_blocked: '当前地区无法使用 AI Studio。',
  cookie_terms_required: 'AI Studio 要求该账号同意条款，ais2api 不会代为同意：请先用此账号在普通浏览器中登录 AI Studio 并同意，再重新导入。',
  terms_required: 'AI Studio 要求该账号同意条款，ais2api 不会代为同意：请用此账号在普通浏览器中登录 AI Studio 并同意，然后清除该账号的冷却。',
  cookie_browser_not_configured: '导入需要已配置的浏览器，请运行 ais2api doctor 检查。',
  cookie_browser_launch: '导入用浏览器启动失败，请运行 ais2api doctor 检查。',
  cookie_browser_timeout: '与浏览器通信超时，请检查浏览器及网络。',
  cookie_import_limit: '导入内容超出数量或大小限制。',
  invalid_credentials: '导入内容格式无效。',
  memory_budget: '后端请求内存预算已耗尽，请稍后再试。',
  backend_unreachable: '代理无法连接到 ais2api 后端。',
  keys_managed_in_config: '该密钥由配置文件管理。',
  admin_password_change_required: '当前仍在使用默认管理密码，请先修改密码。',
  admin_default_password_remote:
    '默认管理密码 admin 只能在本机或局域网内使用。请在本机或局域网内登录并修改密码，或在 config.json 中直接设置 admin_key。',
  admin_password_invalid: '新密码不符合要求：需 8–4096 个可见 ASCII 字符（不含空格），且不能是 admin 或与当前密码相同。',
  invalid_password: '请求格式无效：只需提供新密码。',
}

export function describeError(error: unknown): string {
  if (error instanceof ApiError) {
    const known = REASONS[error.reason]
    if (known) return known
    if (error.status === 0) return error.message
    return error.message ? `${error.message}（HTTP ${error.status}${error.reason ? ` · ${error.reason}` : ''}）` : `HTTP ${error.status}`
  }
  if (error instanceof DOMException && error.name === 'AbortError') return '请求已取消'
  return error instanceof Error ? error.message : String(error)
}

export function apiUrl(path: string, query?: Record<string, string | number | undefined>): string {
  const url = `${session.baseUrl}${API_PREFIX}${path}`
  if (!query) return url
  const params = new URLSearchParams()
  for (const [key, value] of Object.entries(query)) if (value !== undefined && value !== '') params.set(key, String(value))
  const text = params.toString()
  return text ? `${url}?${text}` : url
}

export interface RequestOptions {
  query?: Record<string, string | number | undefined>
  body?: unknown
  signal?: AbortSignal
  headers?: Record<string, string>
  /** Use a different admin key (sign-in verification). */
  adminKey?: string
  baseUrl?: string
}

async function parseError(response: Response): Promise<ApiError> {
  let body: ApiErrorBody | undefined
  try {
    body = (await response.json()) as ApiErrorBody
  } catch {
    /* non-JSON error */
  }
  const error = body?.error
  const reason = error?.reason ?? (typeof error?.code === 'string' ? error.code : '') ?? ''
  return new ApiError(error?.message || response.statusText || '请求失败', response.status, reason)
}

export async function rawRequest(method: string, path: string, options: RequestOptions = {}): Promise<Response> {
  const headers: Record<string, string> = {
    Accept: 'application/json',
    Authorization: `Bearer ${options.adminKey ?? session.adminKey}`,
    ...options.headers,
  }
  let body: string | undefined
  if (options.body !== undefined) {
    headers['Content-Type'] = 'application/json'
    body = JSON.stringify(options.body)
  }
  let url = apiUrl(path, options.query)
  if (options.baseUrl !== undefined) url = `${options.baseUrl}${url.slice(session.baseUrl.length)}`
  let response: Response
  try {
    response = await fetch(url, { method, headers, body, signal: options.signal, cache: 'no-store', credentials: 'omit' })
  } catch (error) {
    if (error instanceof DOMException && error.name === 'AbortError') throw error
    throw new ApiError('无法连接到后端：请确认 ais2api 正在运行、地址正确，且跨域访问已在 admin_origins 中放行。', 0, 'network')
  }
  if (!response.ok) {
    const error = await parseError(response)
    // A rejected key while signed in ends the session (key rotated or revoked).
    if (response.status === 401 && options.adminKey === undefined && session.adminKey) signOut('管理密钥已失效，请重新登录。')
    // The default password only unlocks the password change; App routes there.
    if (error.reason === 'admin_password_change_required' && session.adminKey) session.mustChangePassword = true
    throw error
  }
  return response
}

export async function request<T>(method: string, path: string, options: RequestOptions = {}): Promise<T> {
  const response = await rawRequest(method, path, options)
  const text = await response.text()
  return (text ? JSON.parse(text) : {}) as T
}

export const api = {
  get: <T>(path: string, options?: RequestOptions) => request<T>('GET', path, options),
  post: <T>(path: string, body?: unknown, options?: RequestOptions) => request<T>('POST', path, { ...options, body }),
  patch: <T>(path: string, body?: unknown, options?: RequestOptions) => request<T>('PATCH', path, { ...options, body }),
  put: <T>(path: string, body?: unknown, options?: RequestOptions) => request<T>('PUT', path, { ...options, body }),
  delete: <T>(path: string, options?: RequestOptions) => request<T>('DELETE', path, options),
}

export const enc = encodeURIComponent

const sleep = (ms: number, signal?: AbortSignal) =>
  new Promise<void>((resolve, reject) => {
    const timer = setTimeout(resolve, ms)
    signal?.addEventListener('abort', () => {
      clearTimeout(timer)
      reject(new DOMException('Aborted', 'AbortError'))
    })
  })

/**
 * Starts a browser maintenance operation (HTTP 202) and polls it to completion.
 * The retained result is released afterwards so the backend's 32-slot table
 * does not fill up.
 */
export async function runOperation<R = Record<string, unknown>>(
  path: string,
  options: { signal?: AbortSignal; timeoutMs?: number; onPoll?: (elapsedMs: number) => void } = {},
): Promise<R> {
  const started = await api.post<Operation<R>>(path, undefined, { signal: options.signal })
  const id = started.operation_id
  const begin = Date.now()
  const timeout = options.timeoutMs ?? 10 * 60 * 1000
  let delay = 600
  try {
    for (;;) {
      await sleep(delay, options.signal)
      delay = Math.min(delay * 1.4, 2500)
      const state = await api.get<Operation<R>>(`/operations/${enc(id)}`, { signal: options.signal })
      options.onPoll?.(Date.now() - begin)
      if (state.status === 'done') return (state.result ?? {}) as R
      if (state.status === 'failed') {
        const e = state.error ?? {}
        throw new ApiError(e.message || '后台任务失败', e.status ?? 500, e.code ?? '')
      }
      if (Date.now() - begin > timeout) throw new ApiError('等待后台任务超时', 504, 'operation_timeout')
    }
  } finally {
    api.delete(`/operations/${enc(id)}`).catch(() => {})
  }
}
