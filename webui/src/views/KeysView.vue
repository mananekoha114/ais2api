<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import EmptyState from '../components/EmptyState.vue'
import FDialog from '../components/FDialog.vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import FMenu from '../components/FMenu.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import SecretInput from '../components/SecretInput.vue'
import { api, describeError, enc } from '../api/client'
import type { ConfigSaved, ConfigView, Key, KeyCreated, KeyList } from '../api/types'
import type { IconName } from '../icons'
import { session } from '../stores/session'
import { confirm, copyText, notify, notifyError } from '../stores/ui'
import { formatTimestamp } from '../utils/format'

const list = ref<KeyList | null>(null)
const loading = ref(true)
const loadError = ref('')
const busy = ref(false)
/**
 * The service holds a saved configuration that only applies after a restart.
 * Config-key changes made meanwhile are merged into that pending file and do
 * not affect the running process. Read from GET /config so the state survives
 * page reloads; secrets are redacted there, so no key values are involved.
 */
const pendingRestart = ref(false)

async function loadPending() {
  try {
    pendingRestart.value = (await api.get<ConfigView>('/config')).restart_required
  } catch {
    /* keep the last known state; the key list error is reported separately */
  }
}

async function load() {
  try {
    const [keys] = await Promise.all([api.get<KeyList>('/keys'), loadPending()])
    list.value = keys
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)

const managed = computed(() => list.value?.keys.filter((k) => k.source === 'managed') ?? [])
const configKeySet = computed(() => !!list.value?.config_key_configured)

function randomKey() {
  const bytes = crypto.getRandomValues(new Uint8Array(24))
  return `sk-${Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('')}`
}

async function guarded(task: () => Promise<unknown>, success: string) {
  busy.value = true
  try {
    await task()
    notify('success', success)
    await load()
  } catch (e) {
    notifyError('操作失败', e)
  } finally {
    busy.value = false
  }
}

// ---------- Create managed key ----------
const createOpen = ref(false)
const createName = ref('')
const created = ref<KeyCreated | null>(null)
function openCreate() {
  createName.value = ''
  created.value = null
  createOpen.value = true
}
async function submitCreate() {
  busy.value = true
  try {
    created.value = await api.post<KeyCreated>('/keys', { name: createName.value.trim() || '未命名密钥' })
    await load()
  } catch (e) {
    notifyError('创建失败', e)
  } finally {
    busy.value = false
  }
}

// ---------- Rename ----------
const renameKey = ref<Key | null>(null)
const renameValue = ref('')
const renameOpen = computed({ get: () => !!renameKey.value, set: (v) => !v && (renameKey.value = null) })
function openRename(key: Key) {
  renameKey.value = key
  renameValue.value = key.name ?? ''
}
async function submitRename() {
  const key = renameKey.value
  if (!key) return
  renameKey.value = null
  await guarded(() => api.patch(`/keys/${enc(key.id)}`, { name: renameValue.value.trim() }), '名称已更新')
}

async function revoke(key: Key) {
  const ok = await confirm({
    title: '撤销密钥',
    message: `撤销后，使用「${key.name || key.id}」的客户端将立即无法访问生成接口。`,
    confirmText: '撤销',
    danger: true,
  })
  if (ok) await guarded(() => api.delete(`/keys/${enc(key.id)}`), '密钥已撤销')
}

// ---------- Config key ----------
// PUT/DELETE /keys/config return ConfigSaved. HTTP 200 only means "saved";
// `applied` says whether the running service already uses the new value.
const configOpen = ref(false)
const configValue = ref('')
const configValid = computed(() => /^[!-~]{1,4096}$/.test(configValue.value))
/** Outcome of the last rotation, shown in the dialog until it is closed. */
const rotation = ref<{ value: string; applied: boolean } | null>(null)

// Forget the entered/shown secret once the dialog closes.
watch(configOpen, (open) => {
  if (!open) {
    rotation.value = null
    configValue.value = ''
  }
})

async function openConfigKey() {
  configValue.value = randomKey()
  rotation.value = null
  configOpen.value = true
  await loadPending()
}

const isApplied = (saved: ConfigSaved) => saved.applied && !saved.restart_required

function announce(saved: ConfigSaved, action: '更新' | '清除') {
  if (isApplied(saved)) {
    notify('success', `配置密钥已${action}并生效`, action === '清除' ? '使用旧配置密钥的客户端已无法访问。' : '旧配置密钥已失效。')
  } else {
    notify(
      'warning',
      `配置密钥已保存，重启后生效`,
      '服务有待重启的配置，本次更改尚未应用；重启前旧配置密钥仍然有效。',
      9000,
    )
  }
}

async function submitConfigKey() {
  const value = configValue.value
  busy.value = true
  try {
    const saved = await api.put<ConfigSaved>('/keys/config', { api_key: value })
    rotation.value = { value, applied: isApplied(saved) }
    announce(saved, '更新')
  } catch (e) {
    notifyError('更新配置密钥失败', e)
  } finally {
    busy.value = false
  }
  await load()
}

async function clearConfigKey() {
  await loadPending()
  const ok = await confirm({
    title: '清除配置密钥',
    message: pendingRestart.value
      ? '服务当前有已保存但尚未生效的配置。清除会写入配置文件，但在重启服务前不会生效，旧配置密钥在此期间仍可访问。面板创建的密钥不受影响。'
      : '将把 config.json 中的 api_key 置空。若服务确认已生效，使用该密钥的客户端将无法再访问；实际是否立即生效以服务返回的状态为准。面板创建的密钥不受影响。',
    confirmText: '清除',
    danger: true,
  })
  if (!ok) return
  busy.value = true
  try {
    announce(await api.delete<ConfigSaved>('/keys/config'), '清除')
  } catch (e) {
    notifyError('清除配置密钥失败', e)
  } finally {
    busy.value = false
  }
  await load()
}

const clients = computed<{ name: string; hint: string; url: string; icon: IconName }[]>(() => [
  {
    name: 'OpenAI 兼容（Cherry Studio 等）',
    hint: 'Base URL；支持 Chat Completions 与 Responses，模型从列表选择。',
    url: `${endpoint.value}/v1`,
    icon: 'bot',
  },
  {
    name: 'Claude Code 与 Anthropic 兼容客户端',
    hint: 'ANTHROPIC_BASE_URL；密钥填 ANTHROPIC_API_KEY，接口为 /v1/messages。',
    url: endpoint.value,
    icon: 'bot',
  },
  {
    name: 'RikkaHub',
    hint: 'Google 提供商的 Base URL。',
    url: `${endpoint.value}/v1beta`,
    icon: 'sparkle',
  },
  {
    name: 'SillyTavern',
    hint: '选择 Google AI Studio，反向代理填此地址（也可用 …/v1beta/），代理密码填 API 密钥。',
    url: endpoint.value,
    icon: 'branch',
  },
  {
    name: 'Gemini 原生接口',
    hint: 'generateContent / streamGenerateContent 的 Base URL。',
    url: `${endpoint.value}/v1beta`,
    icon: 'code',
  },
])

const endpoint = computed(() => session.baseUrl || (import.meta.env.DEV ? __DEV_BACKEND_ORIGIN__ : location.origin))
</script>

<template>
  <div>
    <PageHeader title="API 密钥" subtitle="生成接口（OpenAI / Anthropic / Gemini 兼容）使用的密钥。与管理密钥相互独立。">
      <button class="btn accent" @click="openCreate"><FIcon name="add" />创建密钥</button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />
      <FInfoBar
        v-if="list && !list.configured"
        severity="warning"
        title="尚无可用的 API 密钥"
        message="在配置或创建至少一个密钥之前，生成和模型接口会拒绝所有请求。"
      />
      <FInfoBar v-if="pendingRestart" severity="warning" title="有已保存但尚未生效的配置">
        <span>
          下方配置文件密钥的状态反映当前运行中的服务。在重启服务之前，对配置密钥的轮换或清除只会写入配置文件，旧值仍然有效。
          <RouterLink to="/config">查看服务配置</RouterLink>
        </span>
      </FInfoBar>

      <section class="settings-group">
        <h3>配置文件密钥</h3>
        <div class="setting">
          <FIcon name="lock_closed" :size="20" />
          <div class="setting-text">
            <div>config.json · api_key</div>
            <div class="desc">保存在配置文件中的单个密钥，以明文存储。状态为当前运行中服务的生效状态。</div>
          </div>
          <div class="setting-control">
            <span v-if="loading" class="skeleton" style="width: 64px; height: 22px" />
            <span v-else class="badge" :class="configKeySet ? 'success' : 'caution'">
              <FIcon :name="configKeySet ? 'checkmark_circle' : 'warning'" filled />{{ configKeySet ? '已生效' : '未配置' }}
            </span>
            <span v-if="!loading && pendingRestart" class="badge outline" title="重启服务后才会应用已保存的配置">
              <FIcon name="timer" />待重启
            </span>
            <button class="btn" :disabled="busy" @click="openConfigKey">{{ configKeySet ? '轮换' : '设置' }}</button>
            <button v-if="configKeySet" class="btn subtle icon-only" aria-label="清除配置密钥" title="清除" :disabled="busy" @click="clearConfigKey">
              <FIcon name="delete" />
            </button>
          </div>
        </div>
      </section>

      <section class="settings-group">
        <h3>面板创建的密钥</h3>
        <div v-if="loading" class="skeleton" style="height: 68px; border-radius: 8px" />
        <div v-else-if="!managed.length" class="card">
          <EmptyState icon="key_multiple" title="还没有创建密钥" message="为不同的客户端分别创建密钥，便于单独撤销。">
            <button class="btn" @click="openCreate"><FIcon name="add" />创建密钥</button>
          </EmptyState>
        </div>
        <template v-else>
          <div v-for="key in managed" :key="key.id" class="setting">
            <FIcon name="key_multiple" :size="20" />
            <div class="setting-text">
              <div class="truncate">{{ key.name || '未命名密钥' }}</div>
              <div class="desc">
                <span class="mono">{{ key.id }}</span> · 创建于 {{ formatTimestamp(key.created) }}
              </div>
            </div>
            <div class="setting-control">
              <FMenu
                :items="[
                  { label: '重命名', icon: 'edit', action: () => openRename(key) },
                  { divider: true },
                  { label: '撤销密钥', icon: 'delete', danger: true, action: () => revoke(key) },
                ]"
              />
            </div>
          </div>
        </template>
      </section>

      <section class="settings-group">
        <h3>客户端接入</h3>
        <div v-for="client in clients" :key="client.name" class="setting client">
          <FIcon :name="client.icon" :size="20" class="t-secondary" />
          <div class="setting-text">
            <div>{{ client.name }}</div>
            <div class="desc">{{ client.hint }}</div>
          </div>
          <div class="setting-control">
            <button class="url mono" :title="`复制 ${client.url}`" @click="copyText(client.url, '地址已复制')">
              <span class="break">{{ client.url }}</span><FIcon name="copy" :size="14" />
            </button>
          </div>
        </div>
        <div class="card pad stack" style="--gap: 8px">
          <p class="t-secondary t-caption">
            密钥可通过 <code>Authorization: Bearer</code>、<code>x-goog-api-key</code>、<code>x-api-key</code> 或 <code>?key=</code> 传递。移动设备需使用可访问的服务地址。
          </p>
          <pre class="code-block">curl {{ endpoint }}/v1/chat/completions \
  -H "Authorization: Bearer YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"model":"MODEL_ID","messages":[{"role":"user","content":"hello"}],"stream":true}'</pre>
        </div>
      </section>
    </div>

    <!-- Create -->
    <FDialog v-model:open="createOpen" :title="created ? '密钥已创建' : '创建密钥'" :width="480" :dismissible="!busy">
      <template v-if="!created">
        <div class="field">
          <label for="key-name">名称</label>
          <input id="key-name" v-model="createName" class="input" maxlength="128" placeholder="例如：SillyTavern" autofocus @keydown.enter="submitCreate" />
          <span class="hint">仅用于区分不同客户端。</span>
        </div>
      </template>
      <template v-else>
        <FInfoBar severity="warning" message="这是唯一一次显示完整密钥，关闭后无法再次查看。" />
        <div class="field">
          <label>{{ created.name }}</label>
          <div class="secret mono break">{{ created.secret }}</div>
        </div>
      </template>
      <template #footer>
        <template v-if="!created">
          <button class="btn accent" :disabled="busy" @click="submitCreate"><ProgressRing v-if="busy" />创建</button>
          <button class="btn" @click="createOpen = false">取消</button>
        </template>
        <template v-else>
          <button class="btn accent" @click="copyText(created.secret, '密钥已复制')"><FIcon name="copy" />复制</button>
          <button class="btn" @click="createOpen = false">完成</button>
        </template>
      </template>
    </FDialog>

    <!-- Rename -->
    <FDialog v-model:open="renameOpen" title="重命名密钥" :width="400">
      <div class="field">
        <label for="key-rename">名称</label>
        <input id="key-rename" v-model="renameValue" class="input" maxlength="128" autofocus @keydown.enter="submitRename" />
      </div>
      <template #footer>
        <button class="btn accent" @click="submitRename">保存</button>
        <button class="btn" @click="renameKey = null">取消</button>
      </template>
    </FDialog>

    <!-- Config key -->
    <FDialog
      v-model:open="configOpen"
      :title="rotation ? (rotation.applied ? '配置密钥已生效' : '配置密钥已保存，尚未生效') : configKeySet ? '轮换配置密钥' : '设置配置密钥'"
      :width="520"
      :dismissible="!busy"
    >
      <template v-if="!rotation">
        <p class="t-secondary">
          新密钥会写入 config.json。已为你生成一个随机密钥，也可以输入自己的值（最多 4096 个可见 ASCII 字符，不含空白）。
        </p>
        <FInfoBar
          v-if="pendingRestart"
          severity="warning"
          message="服务有待重启的配置：本次更改会保存，但在重启服务前不会生效，旧密钥继续可用，新密钥暂时会被拒绝。"
        />
        <div class="field">
          <label for="config-key">新密钥</label>
          <SecretInput id="config-key" v-model="configValue" :invalid="!configValid" />
          <button class="hyperlink" type="button" @click="configValue = randomKey()"><FIcon name="arrow_sync" :size="14" />重新生成</button>
        </div>
      </template>
      <template v-else>
        <FInfoBar v-if="rotation.applied" severity="success" message="服务已使用新密钥，旧配置密钥已失效。请更新使用它的客户端。" />
        <FInfoBar
          v-else
          severity="warning"
          title="重启后生效"
          message="新密钥已写入配置文件，但当前服务仍使用旧密钥：重启前旧密钥继续有效，新密钥会返回 401。请在重启服务后再更新客户端配置。"
        />
        <div class="field">
          <label>{{ rotation.applied ? '新密钥' : '新密钥（尚未生效）' }}</label>
          <div class="secret mono break">{{ rotation.value }}</div>
          <span class="hint">关闭此对话框后将无法再次查看完整密钥。</span>
        </div>
      </template>
      <template #footer>
        <template v-if="!rotation">
          <button class="btn accent" :disabled="!configValid || busy" @click="submitConfigKey"><ProgressRing v-if="busy" />保存</button>
          <button class="btn" :disabled="busy" @click="configOpen = false">取消</button>
        </template>
        <template v-else>
          <button class="btn accent" @click="copyText(rotation.value, rotation.applied ? '新密钥已复制' : '新密钥已复制（重启后生效）')">
            <FIcon name="copy" />复制
          </button>
          <button class="btn" @click="configOpen = false">完成</button>
        </template>
      </template>
    </FDialog>
  </div>
</template>

<style scoped>
.url {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 4px 8px;
  border: 1px solid var(--control-stroke);
  border-radius: var(--radius-control);
  background: var(--control-alt-secondary);
  color: var(--text-primary);
  text-align: left;
  cursor: pointer;
}
.url:hover {
  background: var(--subtle-secondary);
}
@media (max-width: 640px) {
  .client .setting-control {
    flex-basis: 100%;
    justify-content: flex-start;
  }
}
.secret {
  padding: 10px 12px;
  border-radius: var(--radius-control);
  background: var(--control-alt-secondary);
  border: 1px solid var(--card-stroke);
  user-select: all;
}
@media (max-width: 640px) {
  .setting {
    flex-wrap: wrap;
  }
  .setting .setting-text {
    flex-basis: calc(100% - 40px);
  }
  .setting .setting-control {
    margin-left: auto;
  }
}
</style>
