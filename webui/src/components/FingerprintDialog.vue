<script setup lang="ts">
// Per-account browser identity: a permanent seed, from which both the
// CloakBrowser fingerprint and the Firefox identity derive, the browser backend
// that presents it, plus timezone and language, which follow this computer
// unless overridden. Changes apply the next time the account is used.
import { computed, ref, watch } from 'vue'
import FDialog from './FDialog.vue'
import FIcon from './FIcon.vue'
import FInfoBar from './FInfoBar.vue'
import ProgressRing from './ProgressRing.vue'
import { api, describeError, enc } from '../api/client'
import type { Account, AccountBackend, FingerprintView } from '../api/types'
import { notify } from '../stores/ui'
import { backendIdentity } from '../utils/accounts'

const props = defineProps<{ account: Account | null }>()
const open = defineModel<boolean>('open', { default: false })
const emit = defineEmits<{ changed: [] }>()

const view = ref<FingerprintView | null>(null)
const loading = ref(false)
const saving = ref(false)
const error = ref('')
const timezoneMode = ref<'auto' | 'custom'>('auto')
const timezone = ref('')
const localeMode = ref<'auto' | 'custom'>('auto')
const locale = ref('')
const backend = ref<AccountBackend>('cloakbrowser')
const confirmingRegenerate = ref(false)

// SpiderMonkey is only offered by a build that includes it.
const BACKENDS = computed<{ value: AccountBackend; label: string; hint: string; disabled?: boolean }[]>(() => [
  { value: 'cloakbrowser', label: 'CloakBrowser', hint: 'Chrome 身份。已弃用，将在以后的版本中移除' },
  { value: 'auto', label: '自动', hint: 'Firefox 身份，由 ais2api 选择 Firefox 后端（目前即 Camoufox）' },
  { value: 'camoufox', label: 'Camoufox', hint: 'Firefox 身份，固定使用 Camoufox' },
  view.value?.spidermonkey_available
    ? {
        value: 'spidermonkey',
        label: 'SpiderMonkey（实验性）',
        hint: '实验性原型：不启动浏览器，在本进程内运行 Google 的 WAA 程序。沿用 Camoufox 生成的 Firefox 身份，与 Camoufox 一样声称 Firefox 158；默认构建以 curl-impersonate 发出 Firefox 的 TLS 与 HTTP/2 特征。仅供测试账号使用',
      }
    : { value: 'spidermonkey', label: 'SpiderMonkey', hint: '此版本未包含 SpiderMonkey 后端', disabled: true },
])

// Same rules as the service (IANA zone names, BCP 47 tags).
const TIMEZONE = /^[A-Za-z][A-Za-z0-9/_+-]{0,63}$/
const LOCALE = /^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$/
const timezones: string[] = (() => {
  try {
    return (Intl as unknown as { supportedValuesOf?: (key: string) => string[] }).supportedValuesOf?.('timeZone') ?? []
  } catch {
    return []
  }
})()
const COMMON_LOCALES = ['zh-CN', 'zh-TW', 'zh-HK', 'en-US', 'en-GB', 'ja-JP', 'ko-KR', 'de-DE', 'fr-FR', 'es-ES', 'pt-BR', 'ru-RU']

const path = computed(() => (props.account ? `/accounts/${enc(props.account.id)}/fingerprint` : ''))

function adopt(next: FingerprintView) {
  view.value = next
  timezoneMode.value = next.timezone ? 'custom' : 'auto'
  timezone.value = next.timezone || next.host_timezone
  localeMode.value = next.locale ? 'custom' : 'auto'
  locale.value = next.locale || next.host_locale
  backend.value = next.backend
}

watch(open, async (value) => {
  if (!value || !props.account) return
  error.value = ''
  confirmingRegenerate.value = false
  view.value = null
  loading.value = true
  try {
    adopt(await api.get<FingerprintView>(path.value))
  } catch (e) {
    error.value = describeError(e)
  } finally {
    loading.value = false
  }
}, { immediate: true })

const timezoneValid = computed(() => timezoneMode.value === 'auto' || (TIMEZONE.test(timezone.value) && !timezone.value.includes('//')))
const localeValid = computed(() => localeMode.value === 'auto' || LOCALE.test(locale.value))
const desired = computed(() => ({
  timezone: timezoneMode.value === 'auto' ? '' : timezone.value.trim(),
  locale: localeMode.value === 'auto' ? '' : locale.value.trim(),
}))
const regionDirty = computed(() => !!view.value && (desired.value.timezone !== view.value.timezone || desired.value.locale !== view.value.locale))
const backendDirty = computed(() => !!view.value && backend.value !== view.value.backend)
const dirty = computed(() => regionDirty.value || backendDirty.value)
// Chrome and Firefox identities are different devices to Google.
const identityChange = computed(() => !!view.value && backendDirty.value && backendIdentity(backend.value) !== view.value.identity)

async function save() {
  if (!dirty.value || !timezoneValid.value || !localeValid.value) return
  saving.value = true
  error.value = ''
  try {
    const switching = backendDirty.value
    const body: Record<string, unknown> = {}
    if (regionDirty.value) Object.assign(body, { timezone: desired.value.timezone || null, locale: desired.value.locale || null })
    if (switching) Object.assign(body, { backend: backend.value, confirm_identity_change: identityChange.value })
    adopt(await api.patch<FingerprintView>(path.value, body))
    notify('success', switching ? '浏览器指纹与后端已保存' : '浏览器时区与语言已保存', '下次使用该账号时生效。')
    emit('changed')
    open.value = false
  } catch (e) {
    error.value = describeError(e)
  } finally {
    saving.value = false
  }
}

async function regenerate() {
  saving.value = true
  error.value = ''
  try {
    adopt(await api.post<FingerprintView>(`${path.value}/regenerate`))
    confirmingRegenerate.value = false
    notify('success', '已生成新的浏览器指纹', '下次使用该账号时浏览器会以新指纹重启。')
    emit('changed')
  } catch (e) {
    error.value = describeError(e)
  } finally {
    saving.value = false
  }
}

const platformLabel = (platform: string | undefined) =>
  platform === 'macos' ? 'macOS' : platform === 'windows' ? 'Windows' : platform === 'linux' ? 'Linux' : platform ?? '—'
const identityLabel = (identity: string | undefined) => (identity === 'firefox' ? 'Firefox' : identity === 'chrome' ? 'Chrome' : '—')
</script>

<template>
  <FDialog v-model:open="open" title="浏览器指纹" :width="540" :dismissible="!saving">
    <p class="t-secondary">
      每个账号拥有固定的浏览器指纹，让 Google 始终看到同一台设备。浏览器后端决定它以 Chrome 还是 Firefox 身份出现；时区和语言默认跟随本机地区设置。
    </p>
    <FInfoBar v-if="error" severity="error" :message="error" />
    <div v-if="loading" class="stack" style="--gap: 8px">
      <div v-for="i in 3" :key="i" class="skeleton" style="height: 56px" />
    </div>

    <template v-else-if="view">
      <div class="identity card">
        <div>
          <span class="t-caption t-secondary">指纹种子</span>
          <strong class="mono">{{ view.seed }}</strong>
        </div>
        <div>
          <span class="t-caption t-secondary">身份</span>
          <strong>{{ identityLabel(view.identity) }} · {{ platformLabel(view.platform) }}</strong>
        </div>
        <div>
          <span class="t-caption t-secondary">当前生效</span>
          <strong class="mono truncate" :title="`${view.effective_timezone} · ${view.effective_locale}`">
            {{ view.effective_timezone }} · {{ view.effective_locale }}
          </strong>
        </div>
      </div>

      <fieldset class="field">
        <legend class="label">浏览器后端</legend>
        <div class="segmented" role="radiogroup" aria-label="浏览器后端">
          <button
            v-for="option in BACKENDS"
            :key="option.value"
            type="button"
            role="radio"
            :title="option.hint"
            :disabled="option.disabled || saving"
            :aria-checked="backend === option.value"
            :aria-pressed="backend === option.value"
            @click="backend = option.value"
          >
            {{ option.label }}
          </button>
        </div>
        <FInfoBar
          v-if="identityChange"
          severity="warning"
          :title="backendIdentity(backend) === 'firefox' ? '换成 Firefox 身份？' : '换回 Chrome 身份？'"
          message="Google 会把该账号视为在一台新设备上登录，可能触发额外的安全验证。保存即确认切换。"
        />
        <span v-else class="hint">{{ BACKENDS.find((option) => option.value === backend)?.hint }}</span>
      </fieldset>

      <fieldset class="field">
        <legend class="label">时区</legend>
        <div class="segmented" role="radiogroup" aria-label="时区来源">
          <button type="button" role="radio" :aria-checked="timezoneMode === 'auto'" :aria-pressed="timezoneMode === 'auto'" @click="timezoneMode = 'auto'">
            跟随本机
          </button>
          <button type="button" role="radio" :aria-checked="timezoneMode === 'custom'" :aria-pressed="timezoneMode === 'custom'" @click="timezoneMode = 'custom'">
            自定义
          </button>
        </div>
        <span v-if="timezoneMode === 'auto'" class="hint">本机时区：<code>{{ view.host_timezone }}</code></span>
        <template v-else>
          <input
            id="fp-timezone"
            v-model.trim="timezone"
            class="input mono"
            list="fp-timezones"
            placeholder="Asia/Shanghai"
            aria-label="自定义时区"
            :aria-invalid="!timezoneValid || undefined"
            autocomplete="off"
            spellcheck="false"
          />
          <datalist id="fp-timezones">
            <option v-for="zone in timezones" :key="zone" :value="zone" />
          </datalist>
          <span v-if="!timezoneValid" class="error-text">请输入 IANA 时区名，例如 Asia/Shanghai</span>
          <span v-else class="hint">建议与代理出口所在地区一致。</span>
        </template>
      </fieldset>

      <fieldset class="field">
        <legend class="label">语言</legend>
        <div class="segmented" role="radiogroup" aria-label="语言来源">
          <button type="button" role="radio" :aria-checked="localeMode === 'auto'" :aria-pressed="localeMode === 'auto'" @click="localeMode = 'auto'">
            跟随本机
          </button>
          <button type="button" role="radio" :aria-checked="localeMode === 'custom'" :aria-pressed="localeMode === 'custom'" @click="localeMode = 'custom'">
            自定义
          </button>
        </div>
        <span v-if="localeMode === 'auto'" class="hint">本机语言：<code>{{ view.host_locale }}</code></span>
        <template v-else>
          <input
            id="fp-locale"
            v-model.trim="locale"
            class="input mono"
            list="fp-locales"
            placeholder="zh-CN"
            aria-label="自定义语言"
            :aria-invalid="!localeValid || undefined"
            autocomplete="off"
            spellcheck="false"
          />
          <datalist id="fp-locales">
            <option v-for="tag in COMMON_LOCALES" :key="tag" :value="tag" />
          </datalist>
          <span v-if="!localeValid" class="error-text">请输入语言标签，例如 zh-CN、en-US</span>
          <span v-else class="hint">同时决定页面语言与 Accept-Language 请求头。</span>
        </template>
      </fieldset>

      <div class="regenerate card">
        <template v-if="!confirmingRegenerate">
          <div class="grow">
            <strong>重新生成指纹</strong>
            <p class="t-caption t-secondary">换成全新的设备特征。仅在账号疑似因设备特征受限时使用。</p>
          </div>
          <button type="button" class="btn" :disabled="saving" @click="confirmingRegenerate = true">
            <FIcon name="arrow_sync" />重新生成
          </button>
        </template>
        <template v-else>
          <FInfoBar
            severity="warning"
            title="确认重新生成？"
            message="Google 会把该账号视为在一台新设备上登录，可能触发额外的安全验证。此操作无法撤销。"
          />
          <div class="row confirm-row">
            <button type="button" class="btn danger" :disabled="saving" @click="regenerate">
              <ProgressRing v-if="saving" />确认重新生成
            </button>
            <button type="button" class="btn" :disabled="saving" @click="confirmingRegenerate = false">取消</button>
          </div>
        </template>
      </div>
    </template>

    <template #footer>
      <button class="btn accent" :disabled="!dirty || !timezoneValid || !localeValid || saving" @click="save">
        <ProgressRing v-if="saving && !confirmingRegenerate" class="on-accent" />{{ identityChange ? '确认切换并保存' : '保存' }}
      </button>
      <button class="btn" :disabled="saving" @click="open = false">关闭</button>
    </template>
  </FDialog>
</template>

<style scoped>
.identity {
  display: grid;
  grid-template-columns: auto auto minmax(0, 1fr);
  gap: 8px 24px;
  padding: 12px 16px;
}
.identity > div {
  display: flex;
  flex-direction: column;
  min-width: 0;
}
fieldset.field {
  margin: 0;
  padding: 0;
  border: 0;
}
fieldset .label {
  padding: 0;
  margin-bottom: 6px;
}
.segmented {
  align-self: flex-start;
}
.regenerate {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px 16px;
  padding: 12px 16px;
}
.regenerate p {
  margin-top: 2px;
}
.regenerate :deep(.infobar) {
  flex-basis: 100%;
}
.confirm-row {
  flex-basis: 100%;
  justify-content: flex-end;
}
.on-accent {
  color: var(--text-on-accent);
}
@media (max-width: 640px) {
  .identity {
    grid-template-columns: 1fr 1fr;
  }
  .identity > div:last-child {
    grid-column: 1 / -1;
  }
}
</style>
