// Interaction tests for the generation upstream settings: the form mirrors the
// service's build_app_url and site-isolation rules before anything is saved.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import ConfigView from './ConfigView.vue'
import { session } from '../stores/session'
import { toasts } from '../stores/ui'

const baseConfig = {
  bind: '127.0.0.1',
  port: 18451,
  headless: true,
  proxy: '',
  data_dir: 'data',
  browser_executable: '',
  browser_proxy_relay: false,
  browser_remote_allow_origins: false,
  ignore_unsupported_parameters: false,
  search_model_drop_functions: true,
  browser_site_isolation: true,
  browser_idle_seconds: 0,
  cookie_import_max_accounts: 8,
  cookie_import_timeout_seconds: 180,
  upstream: 'makersuite',
  build_app_url: '',
  limits: {
    request_bytes: 33554432,
    object_bytes: 67108864,
    relay_bytes: 1048576,
    queue_size: 8,
    queue_seconds: 30,
    generation_seconds: 240,
    stream_idle_seconds: 240,
    max_attempts: 3,
  },
  model_aliases: {},
  configured_models: [],
  model_defaults: {},
  admin_origins: [],
}

let calls: { method: string; path: string; body?: unknown }[]

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } })

function fakeFetch(input: RequestInfo | URL, init: RequestInit = {}): Promise<Response> {
  const path = new URL(String(input), 'http://console.test').pathname.replace('/api/admin/v1', '')
  const method = init.method ?? 'GET'
  calls.push({ method, path, body: init.body ? JSON.parse(String(init.body)) : undefined })
  if (method === 'GET' && path === '/config')
    return Promise.resolve(json({ config: baseConfig, config_file: 'config.json', restart_required: false }))
  if (method === 'PATCH' && path === '/config') return Promise.resolve(json({ saved: true, applied: false, restart_required: true }))
  return Promise.resolve(json({ error: { code: 404, message: 'unexpected route', status: 'NOT_FOUND', reason: 'route_not_found' } }, 404))
}

let wrapper: VueWrapper | undefined

async function mountView() {
  wrapper = mount(ConfigView, { attachTo: document.body, global: { stubs: { RouterLink: { template: '<a><slot /></a>' } } } })
  await flushPromises()
  return wrapper
}

const button = (text: string) => {
  const found = [...document.body.querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent?.trim() === text)
  if (!found) throw new Error(`button not found: ${text}`)
  return found
}
const row = (id: string) => document.getElementById(id)!.closest('.setting')!
const errorText = (id: string) => row(id).querySelector('.error-text')?.textContent ?? ''

beforeEach(() => {
  calls = []
  toasts.splice(0)
  session.adminKey = 'synthetic-admin-key'
  session.baseUrl = ''
  vi.stubGlobal('fetch', vi.fn(fakeFetch))
})

afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  document.body.innerHTML = ''
  vi.unstubAllGlobals()
})

describe('generation upstream settings', () => {
  it('shows the current upstream with both choices and marks them as needing a restart', async () => {
    await mountView()
    const select = document.getElementById('f-upstream') as HTMLSelectElement
    expect(select.value).toBe('makersuite')
    expect([...select.options].map((o) => o.value)).toEqual(['makersuite', 'build_app'])
    expect(row('f-upstream').textContent).toContain('需重启')
    expect(row('f-build_app_url').textContent).toContain('需重启')
  })

  it('requires a valid Build App link before the build_app upstream can be saved', async () => {
    const view = await mountView()
    await view.get('#f-upstream').setValue('build_app')
    expect(errorText('f-build_app_url')).toContain('必填')
    expect(button('保存').disabled).toBe(true)

    await view.get('#f-build_app_url').setValue('https://ai.studio/apps/abc?authuser=1')
    expect(errorText('f-build_app_url')).toContain('不含查询参数')
    expect(button('保存').disabled).toBe(true)

    await view.get('#f-build_app_url').setValue('https://ai.studio/apps/0f1e2d3c-4b5a')
    expect(errorText('f-build_app_url')).toBe('')
    button('保存').click()
    await flushPromises()
    const saved = calls.find((c) => c.method === 'PATCH' && c.path === '/config')
    expect(saved?.body).toEqual({ upstream: 'build_app', build_app_url: 'https://ai.studio/apps/0f1e2d3c-4b5a' })
    expect(toasts.map((t) => t.title)).toContain('已保存，需重启后生效')
  })

  it('keeps the build_app upstream from being combined with disabled site isolation', async () => {
    const view = await mountView()
    await view.get('#f-upstream').setValue('build_app')
    await view.get('#f-build_app_url').setValue('https://aistudio.google.com/apps/drive/abc_DEF-1')
    expect(errorText('f-upstream')).toBe('')
    await view.get('input[role="switch"][aria-label="站点隔离"]').setValue(false)
    expect(errorText('f-upstream')).toContain('站点隔离')
    expect(button('保存').disabled).toBe(true)
  })

  it('accepts a stored link while the default upstream stays selected', async () => {
    const view = await mountView()
    await view.get('#f-build_app_url').setValue('https://ai.studio/apps/abc')
    expect(errorText('f-build_app_url')).toBe('')
    expect(button('保存').disabled).toBe(false)
  })
})
