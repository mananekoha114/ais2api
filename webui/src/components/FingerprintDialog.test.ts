import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import FingerprintDialog from './FingerprintDialog.vue'
import type { FingerprintView } from '../api/types'
import { session } from '../stores/session'
import { toasts } from '../stores/ui'

let stored: FingerprintView
let calls: { method: string; path: string; body?: unknown }[]
let failNext: { status: number; reason: string } | null

const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } })

function fakeFetch(input: RequestInfo | URL, init: RequestInit = {}): Promise<Response> {
  const path = new URL(String(input), 'http://console.test').pathname.replace('/api/admin/v1', '')
  const method = init.method ?? 'GET'
  const body = init.body ? JSON.parse(String(init.body)) : undefined
  calls.push({ method, path, body })
  if (failNext && method !== 'GET') {
    const { status, reason } = failNext
    failNext = null
    return Promise.resolve(json({ error: { code: status, message: 'synthetic', status: 'INVALID_ARGUMENT', reason } }, status))
  }
  if (method === 'PATCH' && 'timezone' in body) {
    stored = {
      ...stored,
      timezone: body.timezone ?? '',
      locale: body.locale ?? '',
      effective_timezone: body.timezone ?? stored.host_timezone,
      effective_locale: body.locale ?? stored.host_locale,
    }
  }
  if (method === 'PATCH' && body.backend) {
    const firefox = body.backend !== 'cloakbrowser'
    stored = { ...stored, backend: body.backend, effective_backend: firefox ? 'camoufox' : 'cloakbrowser', identity: firefox ? 'firefox' : 'chrome' }
  }
  if (method === 'POST' && path.endsWith('/regenerate')) stored = { ...stored, seed: stored.seed + 1 }
  return Promise.resolve(json(stored))
}

let wrapper: VueWrapper | undefined
async function mountDialog() {
  wrapper = mount(FingerprintDialog, {
    attachTo: document.body,
    props: { account: { id: 'acc_one', name: 'One' }, open: true, 'onUpdate:open': (v: boolean) => wrapper?.setProps({ open: v }) },
  })
  await flushPromises()
  return wrapper
}
const body = () => document.body.textContent ?? ''
const button = (text: string) => {
  const found = [...document.body.querySelectorAll<HTMLButtonElement>('button')].find((b) => b.textContent?.trim().includes(text))
  if (!found) throw new Error(`button not found: ${text}`)
  return found
}
const segment = (group: string, text: string) =>
  [...document.body.querySelectorAll<HTMLButtonElement>(`[aria-label="${group}"] button`)].find((b) => b.textContent?.trim() === text)!
async function type(selector: string, value: string) {
  const input = document.body.querySelector<HTMLInputElement>(selector)!
  input.value = value
  input.dispatchEvent(new Event('input'))
  await flushPromises()
}

beforeEach(() => {
  stored = {
    seed: 54321,
    platform: 'windows',
    timezone: '',
    locale: '',
    effective_timezone: 'Asia/Shanghai',
    effective_locale: 'zh-CN',
    host_timezone: 'Asia/Shanghai',
    host_locale: 'zh-CN',
    backend: 'cloakbrowser',
    effective_backend: 'cloakbrowser',
    identity: 'chrome',
    firefox_identity_saved: false,
  }
  calls = []
  failNext = null
  toasts.splice(0)
  session.adminKey = 'synthetic-admin'
  session.baseUrl = ''
  vi.stubGlobal('fetch', vi.fn(fakeFetch))
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  document.body.innerHTML = ''
  vi.unstubAllGlobals()
})

describe('browser fingerprint dialog', () => {
  it('shows the permanent seed, platform and host defaults', async () => {
    await mountDialog()
    expect(calls[0]).toMatchObject({ method: 'GET', path: '/accounts/acc_one/fingerprint' })
    expect(body()).toContain('54321')
    expect(body()).toContain('Chrome · Windows')
    expect(body()).toContain('本机时区：Asia/Shanghai')
    expect(body()).toContain('本机语言：zh-CN')
    expect(button('保存').disabled).toBe(true)
  })

  it('saves a custom timezone and keeps the language following the host', async () => {
    await mountDialog()
    segment('时区来源', '自定义').click()
    await flushPromises()
    await type('#fp-timezone', 'Asia/Tokyo')
    expect(button('保存').disabled).toBe(false)
    button('保存').click()
    await flushPromises()
    expect(calls.at(-1)).toMatchObject({ method: 'PATCH', path: '/accounts/acc_one/fingerprint', body: { timezone: 'Asia/Tokyo', locale: null } })
    expect(wrapper!.emitted('changed')).toHaveLength(1)
    expect((wrapper!.props() as { open?: boolean }).open).toBe(false)
    expect(toasts.map((t) => t.title)).toContain('浏览器时区与语言已保存')
  })

  it('refuses invalid values before contacting the service', async () => {
    await mountDialog()
    segment('语言来源', '自定义').click()
    await flushPromises()
    await type('#fp-locale', 'en_US')
    expect(body()).toContain('请输入语言标签')
    expect(button('保存').disabled).toBe(true)
    segment('时区来源', '自定义').click()
    await flushPromises()
    await type('#fp-timezone', 'Bad Zone')
    expect(body()).toContain('请输入 IANA 时区名')
    expect(calls.filter((c) => c.method !== 'GET')).toHaveLength(0)
  })

  it('returns overridden values to the host defaults with null', async () => {
    stored = { ...stored, timezone: 'Asia/Tokyo', locale: 'ja-JP', effective_timezone: 'Asia/Tokyo', effective_locale: 'ja-JP' }
    await mountDialog()
    expect((document.body.querySelector<HTMLInputElement>('#fp-timezone'))!.value).toBe('Asia/Tokyo')
    segment('时区来源', '跟随本机').click()
    segment('语言来源', '跟随本机').click()
    await flushPromises()
    button('保存').click()
    await flushPromises()
    expect(calls.at(-1)?.body).toEqual({ timezone: null, locale: null })
  })

  it('regenerates only after an explicit confirmation', async () => {
    await mountDialog()
    button('重新生成').click()
    await flushPromises()
    expect(body()).toContain('确认重新生成？')
    expect(calls.some((c) => c.method === 'POST')).toBe(false)
    button('取消').click()
    await flushPromises()
    expect(body()).not.toContain('确认重新生成？')
    button('重新生成').click()
    await flushPromises()
    button('确认重新生成').click()
    await flushPromises()
    expect(calls.at(-1)).toMatchObject({ method: 'POST', path: '/accounts/acc_one/fingerprint/regenerate' })
    expect(body()).toContain('54322')
    expect(wrapper!.emitted('changed')).toHaveLength(1)
  })

  it('moves the account to a Firefox identity only with the warning shown', async () => {
    await mountDialog()
    expect(segment('浏览器后端', 'CloakBrowser').getAttribute('aria-checked')).toBe('true')
    expect(segment('浏览器后端', 'SpiderMonkey').disabled).toBe(true)
    segment('浏览器后端', 'Camoufox').click()
    await flushPromises()
    expect(body()).toContain('换成 Firefox 身份？')
    button('确认切换并保存').click()
    await flushPromises()
    expect(calls.at(-1)).toMatchObject({ method: 'PATCH', body: { backend: 'camoufox', confirm_identity_change: true } })
    expect(calls.at(-1)?.body).not.toHaveProperty('timezone')
    expect(toasts.map((t) => t.title)).toContain('浏览器指纹与后端已保存')
  })

  it('switches between Firefox backends without a warning', async () => {
    stored = { ...stored, backend: 'camoufox', effective_backend: 'camoufox', identity: 'firefox', firefox_identity_saved: true }
    await mountDialog()
    expect(body()).toContain('Firefox · Windows')
    segment('浏览器后端', '自动').click()
    await flushPromises()
    expect(body()).not.toContain('Firefox 身份？')
    button('保存').click()
    await flushPromises()
    expect(calls.at(-1)?.body).toEqual({ backend: 'auto', confirm_identity_change: false })
  })

  it('shows the service error and stays open when saving fails', async () => {
    await mountDialog()
    segment('时区来源', '自定义').click()
    await flushPromises()
    await type('#fp-timezone', 'Asia/Tokyo')
    failNext = { status: 400, reason: 'invalid_timezone' }
    button('保存').click()
    await flushPromises()
    expect(body()).toContain('synthetic')
    expect((wrapper!.props() as { open?: boolean }).open).toBe(true)
    expect(wrapper!.emitted('changed')).toBeUndefined()
  })
})
