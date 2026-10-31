<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import FToggle from '../components/FToggle.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import SecretInput from '../components/SecretInput.vue'
import { ApiError, api, describeError } from '../api/client'
import type { Config, ConfigSaved, ConfigView, Limits } from '../api/types'
import type { IconName } from '../icons'
import { session, signIn } from '../stores/session'
import { confirm, copyText, notify, notifyError } from '../stores/ui'
import { formatBytes } from '../utils/format'

type ScalarKey = 'bind' | 'port' | 'proxy' | 'browser_proxy_relay' | 'browser_remote_allow_origins' | 'browser_site_isolation' | 'data_dir' | 'browser_executable' | 'headless' | 'ignore_unsupported_parameters' | 'search_model_drop_functions' | 'browser_idle_seconds' | 'cookie_import_max_accounts' | 'cookie_import_timeout_seconds'

interface FieldDef {
  key: ScalarKey | `limits.${keyof Limits}`
  label: string
  desc: string
  type: 'text' | 'number' | 'bool'
  restart?: boolean
  min?: number
  max?: number
  unit?: 'bytes' | 's'
  placeholder?: string
}

const U32 = 4294967295
const groups: { title: string; icon: IconName; fields: FieldDef[] }[] = [
  {
    title: 'API 兼容性',
    icon: 'globe',
    fields: [
      { key: 'ignore_unsupported_parameters', label: '忽略不支持的参数', desc: '默认关闭。开启后，数据 API 会丢弃上游明确不支持的可选参数；保存后立即生效。', type: 'bool' },
      { key: 'search_model_drop_functions', label: '搜索模型舍弃自定义函数', desc: '默认开启。上游不允许 Google 搜索与自定义函数同时使用：开启时，*-search 模型收到自定义函数会舍弃函数、保留搜索；关闭时保留客户端传来的工具，不再自动加入搜索。保存后立即生效。', type: 'bool' },
    ],
  },
  {
    title: '网络',
    icon: 'globe',
    fields: [
      { key: 'bind', label: '监听地址', desc: '默认 127.0.0.1；改为 0.0.0.0 会对局域网开放。', type: 'text', restart: true },
      { key: 'port', label: '端口', desc: '默认 18451。', type: 'number', restart: true, min: 1, max: 65535 },
      { key: 'proxy', label: '上游代理', desc: '浏览器访问 Google 以及服务端下载请求中的远程图片时使用的代理，例如 http://127.0.0.1:7890。凭据显示为 ***，修改时需完整重填。浏览器不能直接使用带用户名密码的代理，需要开启下面的「本机转发代理」。', type: 'text', restart: true, placeholder: '不使用代理' },
      { key: 'browser_proxy_relay', label: '本机转发代理', desc: '默认关闭。开启后浏览器改为连接 ais2api 在本机开的转发代理，由它向上游代理认证，支持 http:// 和 socks5:// 代理，带不带用户名密码都可以；凭据只留在 ais2api 进程里。转发只允许访问 443 端口。', type: 'bool', restart: true },
    ],
  },
  {
    title: '浏览器与存储',
    icon: 'desktop',
    fields: [
      { key: 'headless', label: '无头模式', desc: '关闭后会显示浏览器窗口，便于排查登录问题。', type: 'bool', restart: true },
      { key: 'browser_executable', label: '浏览器路径', desc: '留空则自动使用程序旁的 browser-runtime。', type: 'text', restart: true, placeholder: '自动检测' },
      { key: 'data_dir', label: '数据目录', desc: '数据库、日志、浏览器资料所在目录。相对路径以配置文件所在目录为基准。', type: 'text', restart: true },
      { key: 'browser_site_isolation', label: '站点隔离', desc: '默认开启。关闭后不同站点不再分进程隔离，可在小内存主机上节省渲染进程内存，但隔离性变弱。', type: 'bool', restart: true },
      { key: 'browser_remote_allow_origins', label: '允许任意来源连接 DevTools', desc: '默认关闭。开启后以 --remote-allow-origins=* 启动浏览器，任意来源的网页都可以连接本机的 DevTools 端口。ais2api 自身不需要，仅供外部 CDP 工具使用。', type: 'bool', restart: true },
      { key: 'browser_idle_seconds', label: '空闲时回收浏览器', desc: '激进的内存优化，0 表示关闭（默认）。连续这么多秒没有任何请求时关闭浏览器以释放内存，关闭前会写回 Cookie；之后的第一个请求需要重新启动浏览器、打开页面并引导，会明显变慢。可设为 10–86400 秒，建议不低于 600。保存后立即生效。', type: 'number', min: 0, max: 86400, unit: 's' },
    ],
  },
  {
    title: '队列与超时',
    icon: 'timer',
    fields: [
      { key: 'limits.queue_size', label: '队列长度', desc: '同时排队等待的最大请求数。', type: 'number', min: 1, max: 4096 },
      { key: 'limits.queue_seconds', label: '排队超时', desc: '请求在队列中等待的最长时间。', type: 'number', min: 1, max: U32, unit: 's' },
      { key: 'limits.generation_seconds', label: '生成超时', desc: '单次生成的最长时间。', type: 'number', min: 1, max: U32, unit: 's', restart: true },
      { key: 'limits.stream_idle_seconds', label: '流空闲超时', desc: '流式响应两次输出之间允许的最长间隔。', type: 'number', min: 1, max: U32, unit: 's', restart: true },
      { key: 'limits.max_attempts', label: '最大尝试次数', desc: '失败时切换账号重试的次数上限。', type: 'number', min: 1, max: 16 },
    ],
  },
  {
    title: '传输限额',
    icon: 'database',
    fields: [
      { key: 'limits.request_bytes', label: '请求体上限', desc: '单个 HTTP 请求体的最大字节数。', type: 'number', min: 1, max: U32, unit: 'bytes' },
      { key: 'limits.object_bytes', label: '对象上限', desc: '内联媒体与非流式响应的单对象上限。', type: 'number', min: 1, max: U32, unit: 'bytes', restart: true },
      { key: 'limits.relay_bytes', label: '中继块上限', desc: '浏览器桥接每次转发的数据块大小。', type: 'number', min: 1, max: U32, unit: 'bytes', restart: true },
    ],
  },
  {
    title: 'Cookie 导入',
    icon: 'arrow_upload',
    fields: [
      { key: 'cookie_import_max_accounts', label: '单次最多账号数', desc: '未指定 authuser 时依次探测的账号索引数量。', type: 'number', min: 1, max: 32 },
      { key: 'cookie_import_timeout_seconds', label: '导入超时', desc: '单次导入验证的最长时间。', type: 'number', min: 1, max: 240, unit: 's' },
    ],
  },
]

const view = ref<ConfigView | null>(null)
const base = ref<Config>({})
const form = reactive<Record<string, unknown>>({})
const aliases = ref<{ from: string; to: string }[]>([])
const origins = ref<string[]>([])
const configuredModelsText = ref('')
const modelDefaultsText = ref('')
const secrets = reactive({ admin_key: '', cookie_import_key: '', clearCookieKey: false })
const loading = ref(true)
const loadError = ref('')
const saving = ref(false)
const validation = ref<{ ok: boolean; message: string } | null>(null)

function read(config: Config, key: string): unknown {
  if (key.startsWith('limits.')) return config.limits?.[key.slice(7) as keyof Limits]
  return config[key as keyof Config]
}

const pretty = (value: unknown) => JSON.stringify(value ?? null, null, 2)

function fill(config: Config) {
  base.value = config
  for (const group of groups) for (const field of group.fields) form[field.key] = read(config, field.key) ?? (field.type === 'bool' ? false : '')
  aliases.value = Object.entries(config.model_aliases ?? {}).map(([from, to]) => ({ from, to }))
  origins.value = [...(config.admin_origins ?? [])]
  configuredModelsText.value = pretty(config.configured_models ?? [])
  modelDefaultsText.value = pretty(config.model_defaults ?? {})
  secrets.admin_key = ''
  secrets.cookie_import_key = ''
  secrets.clearCookieKey = false
  validation.value = null
}

async function load() {
  try {
    view.value = await api.get<ConfigView>('/config')
    // Later patches build on the pending configuration when one is saved.
    fill(view.value.saved_config ?? view.value.config)
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)

// ---------- Patch construction ----------
const jsonErrors = computed(() => {
  const errors: Record<string, string> = {}
  try {
    if (!Array.isArray(JSON.parse(configuredModelsText.value))) errors.configured_models = '必须是数组'
  } catch {
    errors.configured_models = 'JSON 格式无效'
  }
  try {
    const v = JSON.parse(modelDefaultsText.value)
    if (!v || typeof v !== 'object' || Array.isArray(v)) errors.model_defaults = '必须是对象'
  } catch {
    errors.model_defaults = 'JSON 格式无效'
  }
  return errors
})

function fieldError(field: FieldDef): string {
  const value = form[field.key]
  if (field.type !== 'number') return field.key === 'bind' && !String(value).trim() ? '不能为空' : ''
  if (value === '' || typeof value !== 'number' || !Number.isInteger(value)) return '请输入整数'
  if (field.min !== undefined && value < field.min) return `不能小于 ${field.min}`
  if (field.max !== undefined && value > field.max) return `不能大于 ${field.max}`
  if (field.key === 'browser_idle_seconds' && value > 0 && value < 10) return '0 表示关闭，开启时不能小于 10'
  return ''
}

const ORIGIN = /^https?:\/\/(?:\[[0-9A-Fa-f:]+\]|[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9.])?)(?::[0-9]{1,5})?$/
const originErrors = computed(() => origins.value.map((o) => (o && !ORIGIN.test(o) ? '格式应为 http(s)://主机[:端口]，不含路径' : '')))
const secretPattern = /^[!-~]*$/

const hasErrors = computed(
  () =>
    groups.some((g) => g.fields.some((f) => fieldError(f))) ||
    Object.keys(jsonErrors.value).length > 0 ||
    originErrors.value.some(Boolean) ||
    aliases.value.some((a) => (a.from.trim() && !a.to.trim()) || (!a.from.trim() && a.to.trim())) ||
    !secretPattern.test(secrets.admin_key) ||
    !secretPattern.test(secrets.cookie_import_key),
)

const patch = computed<Config>(() => {
  const out: Record<string, unknown> = {}
  const limits: Record<string, unknown> = {}
  for (const group of groups)
    for (const field of group.fields) {
      const value = form[field.key]
      if (value === read(base.value, field.key)) continue
      if (field.key.startsWith('limits.')) limits[field.key.slice(7)] = value
      else out[field.key] = value
    }
  if (Object.keys(limits).length) out.limits = limits

  const aliasMap = Object.fromEntries(aliases.value.filter((a) => a.from.trim()).map((a) => [a.from.trim(), a.to.trim()]))
  if (JSON.stringify(aliasMap) !== JSON.stringify(base.value.model_aliases ?? {})) out.model_aliases = aliasMap

  const originList = origins.value.map((o) => o.trim()).filter(Boolean)
  if (JSON.stringify(originList) !== JSON.stringify(base.value.admin_origins ?? [])) out.admin_origins = originList

  if (!jsonErrors.value.configured_models) {
    const value = JSON.parse(configuredModelsText.value)
    if (JSON.stringify(value) !== JSON.stringify(base.value.configured_models ?? [])) out.configured_models = value
  }
  if (!jsonErrors.value.model_defaults) {
    const value = JSON.parse(modelDefaultsText.value)
    if (JSON.stringify(value) !== JSON.stringify(base.value.model_defaults ?? {})) out.model_defaults = value
  }

  if (secrets.admin_key) out.admin_key = secrets.admin_key
  if (secrets.clearCookieKey) out.cookie_import_key = ''
  else if (secrets.cookie_import_key) out.cookie_import_key = secrets.cookie_import_key
  return out as Config
})

const changeCount = computed(() => {
  const p = patch.value as Record<string, unknown>
  return Object.keys(p).reduce((n, k) => n + (k === 'limits' ? Object.keys(p.limits as object).length : 1), 0)
})

const restartKeys = new Set(groups.flatMap((g) => g.fields.filter((f) => f.restart).map((f) => f.key)))

const pendingDiff = computed(() => {
  if (!view.value?.saved_config) return []
  const diffs: string[] = []
  for (const group of groups)
    for (const field of group.fields)
      if (JSON.stringify(read(view.value.config, field.key)) !== JSON.stringify(read(view.value.saved_config, field.key))) diffs.push(field.label)
  return diffs
})

// ---------- Actions ----------
async function validate() {
  try {
    const result = await api.post<{ valid: boolean; restart_required?: boolean }>('/config/validate', patch.value)
    validation.value = { ok: true, message: result.restart_required ? '校验通过。保存后需要重启服务才能生效。' : '校验通过，保存后立即生效。' }
  } catch (e) {
    validation.value = { ok: false, message: describeError(e) }
  }
}

async function save() {
  if (secrets.admin_key) {
    const ok = await confirm({
      title: '更换管理密钥',
      message: '新的管理密钥生效后，所有使用旧密钥的会话（包括其他设备）都会失效。请确认已妥善保存新密钥。',
      confirmText: '保存',
    })
    if (!ok) return
  }
  saving.value = true
  const newAdminKey = secrets.admin_key
  try {
    const result = await api.patch<ConfigSaved>('/config', patch.value)
    if (newAdminKey && result.applied) signIn(session.baseUrl, newAdminKey, session.remember)
    notify(
      result.restart_required ? 'warning' : 'success',
      result.restart_required ? '已保存，需重启后生效' : '配置已保存并生效',
      result.restart_required ? '包含监听、浏览器、存储或传输限额的变更；在重启前其余更改也不会应用。' : undefined,
      result.restart_required ? 9000 : 4000,
    )
    await load()
  } catch (e) {
    notifyError('保存失败', e)
  } finally {
    saving.value = false
  }
}

async function reloadFromDisk() {
  try {
    await api.post('/config/reload')
    notify('success', '已从配置文件重新加载')
    await load()
  } catch (e) {
    if (e instanceof ApiError && e.reason === 'restart_required') notify('warning', '无法热重载', '配置文件中包含需要重启才能生效的变更。')
    else notifyError('重新加载失败', e)
  }
}

async function discard() {
  if (await confirm({ title: '放弃更改', message: `将放弃 ${changeCount.value} 项未保存的更改。`, confirmText: '放弃' })) fill(base.value)
}

const consoleOrigin = location.origin
const bytesHint = (field: FieldDef) => (field.unit === 'bytes' && typeof form[field.key] === 'number' ? formatBytes(form[field.key] as number) : '')
</script>

<template>
  <div class="config-page" :class="{ dirty: changeCount > 0 }">
    <PageHeader title="服务配置" subtitle="编辑 ais2api 的 config.json。保存前会校验完整配置；部分项需重启服务才能生效。">
      <button class="btn" @click="reloadFromDisk"><FIcon name="arrow_sync" />从文件重新加载</button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />
      <FInfoBar v-if="view?.restart_required" severity="warning" title="有已保存但尚未生效的配置">
        <span>
          需要重启服务（<code>ais2api stop</code> 后 <code>ais2api start</code>）才能应用。
          <template v-if="pendingDiff.length">涉及：{{ pendingDiff.join('、') }}。</template>
          下方表单显示的是待生效的配置。
        </span>
      </FInfoBar>

      <div v-if="loading" class="stack" style="--gap: 4px">
        <div v-for="i in 6" :key="i" class="skeleton" style="height: 68px; border-radius: 8px" />
      </div>

      <template v-else-if="view">
        <div class="setting file">
          <FIcon name="document_text" :size="20" />
          <div class="setting-text">
            <div>配置文件</div>
            <div class="desc mono break">{{ view.config_file }}</div>
          </div>
          <div class="setting-control">
            <button class="btn subtle icon-only" aria-label="复制路径" title="复制路径" @click="copyText(view.config_file)"><FIcon name="copy" /></button>
          </div>
        </div>

        <section v-for="group in groups" :key="group.title" class="settings-group">
          <h3>{{ group.title }}</h3>
          <div v-for="field in group.fields" :key="field.key" class="setting" :class="{ stacked: field.type === 'text' }">
            <FIcon :name="group.icon" :size="20" class="t-secondary" />
            <div class="setting-text">
              <label :for="`f-${field.key}`" class="label-row">
                {{ field.label }}
                <span v-if="restartKeys.has(field.key)" class="badge outline">需重启</span>
                <span v-if="form[field.key] !== read(base, field.key)" class="badge accent">已修改</span>
              </label>
              <div class="desc">{{ field.desc }}</div>
            </div>
            <div class="setting-control">
              <FToggle v-if="field.type === 'bool'" :model-value="!!form[field.key]" :label="field.label" @update:model-value="form[field.key] = $event" />
              <div v-else class="input-wrap">
                <input
                  :id="`f-${field.key}`"
                  v-model.trim="form[field.key]"
                  class="input"
                  :class="{ mono: field.type === 'text' }"
                  :type="field.type === 'number' ? 'number' : 'text'"
                  :inputmode="field.type === 'number' ? 'numeric' : undefined"
                  :min="field.min"
                  :max="field.max"
                  :placeholder="field.placeholder"
                  :aria-invalid="!!fieldError(field) || undefined"
                  autocomplete="off"
                  spellcheck="false"
                />
                <span v-if="fieldError(field)" class="error-text t-caption">{{ fieldError(field) }}</span>
                <span v-else-if="field.unit" class="t-caption t-tertiary unit">{{ field.unit === 's' ? '秒' : bytesHint(field) }}</span>
              </div>
            </div>
          </div>
        </section>

        <section class="settings-group">
          <h3>访问控制</h3>
          <div class="setting stacked">
            <FIcon name="shield" :size="20" class="t-secondary" />
            <div class="setting-text">
              <label for="f-admin-key" class="label-row">管理密钥 admin_key</label>
              <div class="desc">留空表示保持不变。更换后当前会话会自动改用新密钥。</div>
            </div>
            <div class="setting-control">
              <SecretInput id="f-admin-key" v-model="secrets.admin_key" placeholder="已隐藏（保持不变）" :invalid="!secretPattern.test(secrets.admin_key)" />
            </div>
          </div>
          <div class="setting stacked">
            <FIcon name="arrow_upload" :size="20" class="t-secondary" />
            <div class="setting-text">
              <label for="f-cookie-key" class="label-row">Cookie 导入密钥 cookie_import_key</label>
              <div class="desc">供 userscript 推送 Cookie 使用，不授予生成权限。留空表示保持不变。</div>
            </div>
            <div class="setting-control column">
              <SecretInput
                id="f-cookie-key"
                v-model="secrets.cookie_import_key"
                :placeholder="secrets.clearCookieKey ? '将被清空（禁用导入）' : '已隐藏（保持不变）'"
                :invalid="!secretPattern.test(secrets.cookie_import_key)"
              />
              <label class="check">
                <input v-model="secrets.clearCookieKey" type="checkbox" />
                <span>清空此密钥，禁用 userscript 导入</span>
              </label>
            </div>
          </div>
          <div class="setting">
            <FIcon name="key_multiple" :size="20" class="t-secondary" />
            <div class="setting-text">
              <div class="label-row">生成 API 密钥 api_key</div>
              <div class="desc">在「API 密钥」页面设置、轮换或清除。</div>
            </div>
            <div class="setting-control"><RouterLink to="/keys" class="btn">前往 API 密钥</RouterLink></div>
          </div>
          <div class="setting stacked">
            <FIcon name="globe" :size="20" class="t-secondary" />
            <div class="setting-text">
              <div class="label-row">允许的管理来源 admin_origins</div>
              <div class="desc">跨域访问管理 API 的完整来源，例如 http://localhost:5173。为空时只允许同源访问；内置控制台和开发服务器代理均可同源访问。</div>
            </div>
            <div class="setting-control column">
              <div v-for="(_, index) in origins" :key="index" class="list-row">
                <input
                  v-model.trim="origins[index]"
                  class="input mono"
                  placeholder="https://example.com:8443"
                  :aria-label="`来源 ${index + 1}`"
                  :aria-invalid="!!originErrors[index] || undefined"
                />
                <button class="btn subtle icon-only" :aria-label="`移除来源 ${index + 1}`" @click="origins.splice(index, 1)"><FIcon name="delete" /></button>
                <span v-if="originErrors[index]" class="error-text t-caption full">{{ originErrors[index] }}</span>
              </div>
              <div class="row wrap">
                <button class="btn" @click="origins.push('')"><FIcon name="add" />添加来源</button>
                <button v-if="!origins.includes(consoleOrigin)" class="btn subtle" @click="origins.push(consoleOrigin)">添加当前控制台 {{ consoleOrigin }}</button>
              </div>
            </div>
          </div>
        </section>

        <section class="settings-group">
          <h3>模型</h3>
          <div class="setting stacked">
            <FIcon name="branch" :size="20" class="t-secondary" />
            <div class="setting-text">
              <div class="label-row">模型别名 model_aliases</div>
              <div class="desc">把客户端请求的模型名映射到实际模型。整体替换保存。</div>
            </div>
            <div class="setting-control column">
              <div v-for="(alias, index) in aliases" :key="index" class="list-row alias">
                <input v-model="alias.from" class="input mono" placeholder="请求名，如 gpt-4o" :aria-label="`别名 ${index + 1} 请求名`" />
                <FIcon name="arrow_next" class="t-tertiary arrow" />
                <input v-model="alias.to" class="input mono" placeholder="实际模型 ID" :aria-label="`别名 ${index + 1} 实际模型`" />
                <button class="btn subtle icon-only" :aria-label="`移除别名 ${index + 1}`" @click="aliases.splice(index, 1)"><FIcon name="delete" /></button>
              </div>
              <div><button class="btn" @click="aliases.push({ from: '', to: '' })"><FIcon name="add" />添加别名</button></div>
            </div>
          </div>
          <div class="setting stacked">
            <FIcon name="cube" :size="20" class="t-secondary" />
            <div class="setting-text">
              <label for="f-configured-models" class="label-row">显式配置的模型 configured_models</label>
              <div class="desc">没有已保存列表时使用，支持模型 ID 字符串或对象。</div>
            </div>
            <div class="setting-control column">
              <textarea id="f-configured-models" v-model="configuredModelsText" class="textarea mono" rows="5" spellcheck="false" :aria-invalid="!!jsonErrors.configured_models || undefined" />
              <span v-if="jsonErrors.configured_models" class="error-text t-caption">{{ jsonErrors.configured_models }}</span>
            </div>
          </div>
          <div class="setting stacked">
            <FIcon name="options" :size="20" class="t-secondary" />
            <div class="setting-text">
              <label for="f-model-defaults" class="label-row">模型默认参数 model_defaults</label>
              <div class="desc">按模型设置的默认生成参数（JSON 对象）。</div>
            </div>
            <div class="setting-control column">
              <textarea id="f-model-defaults" v-model="modelDefaultsText" class="textarea mono" rows="5" spellcheck="false" :aria-invalid="!!jsonErrors.model_defaults || undefined" />
              <span v-if="jsonErrors.model_defaults" class="error-text t-caption">{{ jsonErrors.model_defaults }}</span>
            </div>
          </div>
        </section>
      </template>
    </div>

    <Transition name="bar">
      <div v-if="changeCount > 0" class="save-bar" role="region" aria-label="未保存的更改">
        <div class="save-inner">
          <div class="grow save-text">
            <strong>{{ changeCount }} 项未保存的更改</strong>
            <span v-if="validation" class="t-caption" :class="validation.ok ? 't-success' : 't-critical'">{{ validation.message }}</span>
            <span v-else-if="hasErrors" class="t-caption t-critical">请先修正标红的字段</span>
          </div>
          <button class="btn subtle" :disabled="saving" @click="discard">放弃</button>
          <button class="btn" :disabled="saving || hasErrors" @click="validate">校验</button>
          <button class="btn accent" :disabled="saving || hasErrors" @click="save"><ProgressRing v-if="saving" class="on-accent" />保存</button>
        </div>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.config-page.dirty {
  padding-bottom: 72px;
}
.label-row {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 6px 8px;
}
.input-wrap {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  gap: 2px;
}
.input-wrap .input[type='number'] {
  width: 180px;
  text-align: right;
}
.unit,
.input-wrap .error-text {
  min-height: 16px;
}
.setting.stacked .input-wrap {
  align-items: stretch;
}
.setting.stacked .input-wrap .input {
  width: 100%;
}
.setting-control.column {
  flex-direction: column;
  align-items: stretch;
  gap: 8px;
}
.list-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}
.list-row .input {
  flex: 1 1 200px;
  width: auto;
}
.list-row .full {
  flex-basis: 100%;
}
.alias .arrow {
  flex: none;
}
.save-bar {
  position: fixed;
  z-index: 25;
  left: 0;
  right: 0;
  bottom: 0;
  padding: 0 16px calc(12px + var(--safe-bottom));
  pointer-events: none;
}
.save-inner {
  pointer-events: auto;
  max-width: 880px;
  margin: 0 auto;
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
  padding: 10px 12px 10px 16px;
  border-radius: var(--radius-overlay);
  border: 1px solid var(--surface-stroke);
  background: var(--acrylic);
  backdrop-filter: blur(30px) saturate(150%);
  -webkit-backdrop-filter: blur(30px) saturate(150%);
  box-shadow: var(--shadow-flyout);
}
.save-text {
  display: flex;
  flex-direction: column;
  min-width: 160px;
}
.on-accent {
  color: var(--text-on-accent);
}
.bar-enter-active {
  transition:
    opacity var(--motion-normal) linear,
    transform var(--motion-slow) var(--ease-decelerate);
}
.bar-leave-active {
  transition: opacity var(--motion-fast) linear;
}
.bar-enter-from {
  opacity: 0;
  transform: translateY(24px);
}
.bar-leave-to {
  opacity: 0;
}
@media (max-width: 640px) {
  .setting {
    flex-wrap: wrap;
  }
  .setting .setting-text {
    flex-basis: calc(100% - 40px);
  }
  .setting .setting-control {
    flex-basis: 100%;
    justify-content: flex-start;
  }
  .input-wrap,
  .input-wrap .input[type='number'] {
    width: 100%;
    align-items: stretch;
    text-align: left;
  }
  .config-page.dirty {
    padding-bottom: 120px;
  }
}
</style>
