<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import EmptyState from '../components/EmptyState.vue'
import FDialog from '../components/FDialog.vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import FMenu, { type MenuItem } from '../components/FMenu.vue'
import FingerprintDialog from '../components/FingerprintDialog.vue'
import ImportAccountDialog from '../components/ImportAccountDialog.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import { api, describeError, enc, runOperation } from '../api/client'
import type { Account, ProbeResult, Rotation } from '../api/types'
import { refreshStatus, service } from '../stores/service'
import { confirm, notify, notifyError } from '../stores/ui'
import { accountLabel, accountState, backendLabel, cloakbrowserNotice, cooldownReason, now } from '../utils/accounts'
import { formatRelative, formatTimestamp, isIndefinite } from '../utils/format'
import { usePolling } from '../utils/media'

const rotation = ref<Rotation | null>(null)
const deprecation = computed(() => cloakbrowserNotice(service.status?.deprecations))
const loading = ref(true)
const loadError = ref('')
const busy = reactive<Record<string, string>>({})
const globalBusy = ref<string | null>(null)
const probes = reactive<Record<string, ProbeResult>>({})
const expanded = reactive<Record<string, boolean>>({})
const query = ref('')
const filter = ref<'all' | 'available' | 'unavailable'>('all')
const importOpen = ref(false)
const clock = ref(now())

async function load() {
  try {
    rotation.value = await api.get<Rotation>('/accounts/active')
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
    clock.value = now()
  }
}
onMounted(load)
usePolling(load, 15000)

const accounts = computed(() => rotation.value?.accounts ?? [])
const filtered = computed(() => {
  const q = query.value.trim().toLowerCase()
  return accounts.value.filter((account) => {
    const state = accountState(account, clock.value)
    if (filter.value === 'available' && !state.available) return false
    if (filter.value === 'unavailable' && state.available) return false
    if (!q) return true
    return [account.name, account.email, account.id, account.auth_user].some((v) => v?.toLowerCase().includes(q))
  })
})
const counts = computed(() => {
  const available = accounts.value.filter((a) => accountState(a, clock.value).available).length
  return { all: accounts.value.length, available, unavailable: accounts.value.length - available }
})

const initials = (account: Account) => (accountLabel(account).trim()[0] ?? '?').toUpperCase()

async function run(id: string, label: string, task: () => Promise<unknown>, success?: string) {
  busy[id] = label
  try {
    await task()
    if (success) notify('success', success)
    await load()
    refreshStatus()
  } catch (e) {
    notifyError(`${label}失败`, e)
  } finally {
    delete busy[id]
  }
}

const activate = (a: Account) => run(a.id, '设为首选', () => api.post(`/accounts/${enc(a.id)}/activate`), `已将「${accountLabel(a)}」设为首选账号`)
const toggleDisabled = (a: Account) =>
  run(a.id, a.disabled ? '启用' : '禁用', () => api.patch(`/accounts/${enc(a.id)}`, { disabled: !a.disabled }), a.disabled ? '账号已启用' : '账号已禁用')
const reset = (a: Account) => run(a.id, '重置状态', () => api.post(`/accounts/${enc(a.id)}/reset`), '已重置失败与禁用状态')
const clearCooldown = (a: Account, model?: string) =>
  run(a.id, '清除冷却', () => api.post('/rotation/clear-cooldown', model ? { id: a.id, model } : { id: a.id }), model ? `已清除 ${model === '*' ? '全局' : model} 冷却` : '已清除全部冷却')

function probe(a: Account, kind: 'probe' | 'detect-email') {
  return run(a.id, kind === 'probe' ? '探测' : '检测邮箱', async () => {
    const result = await runOperation<{ accounts?: ProbeResult[] }>(`/accounts/${enc(a.id)}/${kind}`)
    const entry = result.accounts?.find((r) => r.account_id === a.id) ?? result.accounts?.[0]
    if (entry) probes[a.id] = entry
    if (entry?.status === 'ready') notify('success', '身份已确认', entry.email)
    else notify('warning', '未能确认身份', probeText(entry))
  })
}

async function remove(a: Account) {
  const ok = await confirm({
    title: '删除账号',
    message: `将从数据库中永久删除「${accountLabel(a)}」及其凭据。此操作无法撤销。`,
    confirmText: '删除',
    danger: true,
  })
  if (ok) await run(a.id, '删除', () => api.delete(`/accounts/${enc(a.id)}`), '账号已删除')
}

async function probeAll() {
  globalBusy.value = 'probe'
  try {
    const result = await runOperation<{ accounts?: ProbeResult[] }>('/accounts/probe')
    let failed = 0
    for (const entry of result.accounts ?? []) {
      probes[entry.account_id] = entry
      if (entry.status !== 'ready') failed++
    }
    notify(failed ? 'warning' : 'success', failed ? `探测完成，${failed} 个账号未通过` : '全部账号身份已确认')
    await load()
  } catch (e) {
    notifyError('探测失败', e)
  } finally {
    globalBusy.value = null
  }
}

async function rotateNext() {
  globalBusy.value = 'rotate'
  try {
    const result = await api.post<{ preferred_account_id?: string }>('/rotation/next')
    await load()
    notify('success', '已切换首选账号', accountLabel(accounts.value.find((a) => a.id === result.preferred_account_id), result.preferred_account_id))
  } catch (e) {
    notifyError('切换失败', e)
  } finally {
    globalBusy.value = null
  }
}

function probeText(entry: ProbeResult | undefined): string {
  if (!entry) return '没有返回结果'
  if (entry.error) return entry.error.message ?? entry.error.code ?? '探测失败'
  if (entry.login) return '需要重新登录（Cookie 已失效）'
  if (entry.regionBlocked) return '当前地区无法使用 AI Studio'
  if (entry.identityAmbiguous) return '页面上存在多个账号，无法确定身份'
  if (entry.status === 'ready') return `已确认：${entry.email}`
  return '身份未确认（页面尚未加载出邮箱）'
}

// ---------- Browser fingerprint dialog ----------
const fingerprintTarget = ref<Account | null>(null)
const fingerprintOpen = computed({
  get: () => !!fingerprintTarget.value,
  set: (v) => !v && (fingerprintTarget.value = null),
})

// ---------- Rename dialog ----------
const renameTarget = ref<Account | null>(null)
const renameValue = ref('')
const renameOpen = computed({
  get: () => !!renameTarget.value,
  set: (v) => !v && (renameTarget.value = null),
})
function openRename(a: Account) {
  renameTarget.value = a
  renameValue.value = a.name ?? ''
}
async function submitRename() {
  const a = renameTarget.value
  if (!a) return
  renameTarget.value = null
  await run(a.id, '重命名', () => api.patch(`/accounts/${enc(a.id)}`, { name: renameValue.value.trim() }), '名称已更新')
}

function menuFor(a: Account): MenuItem[] {
  return [
    { label: '重命名', icon: 'edit', action: () => openRename(a) },
    { label: a.disabled ? '启用账号' : '禁用账号', icon: a.disabled ? 'person_available' : 'person_prohibited', action: () => toggleDisabled(a) },
    { divider: true },
    { label: '探测登录状态', icon: 'scan', action: () => probe(a, 'probe') },
    { label: '检测并更新邮箱', icon: 'mail', action: () => probe(a, 'detect-email') },
    { label: '浏览器指纹…', icon: 'desktop', action: () => (fingerprintTarget.value = a) },
    { divider: true },
    { label: '清除全部冷却', icon: 'timer', disabled: !(a.failures?.length ?? 0), action: () => clearCooldown(a) },
    { label: '重置失败与禁用状态', icon: 'arrow_reset', action: () => reset(a) },
    { divider: true },
    { label: '删除账号', icon: 'delete', danger: true, action: () => remove(a) },
  ]
}
</script>

<template>
  <div>
    <PageHeader title="账号" subtitle="管理 AI Studio 账号凭据、首选顺序与冷却状态。凭据本身不会返回到浏览器。">
      <button class="btn accent" @click="importOpen = true"><FIcon name="add" />导入账号</button>
      <button class="btn" :disabled="!!globalBusy || !accounts.length" @click="probeAll">
        <ProgressRing v-if="globalBusy === 'probe'" /><FIcon v-else name="scan" />探测全部
      </button>
      <button class="btn" :disabled="!!globalBusy || !counts.available" @click="rotateNext">
        <ProgressRing v-if="globalBusy === 'rotate'" /><FIcon v-else name="arrow_next" />下一个
      </button>
      <button class="btn subtle icon-only" aria-label="刷新" title="刷新" @click="load"><FIcon name="arrow_clockwise" /></button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />
      <FInfoBar v-if="deprecation" severity="warning" title="CloakBrowser 已弃用" :message="deprecation" />
      <FInfoBar v-if="globalBusy === 'probe'" severity="info" title="正在探测">
        后端正在通过浏览器逐个确认账号身份，可能需要数分钟。
      </FInfoBar>

      <div class="toolbar">
        <div class="input-group search">
          <span class="lead"><FIcon name="search" /></span>
          <input v-model="query" class="input has-lead" type="search" placeholder="搜索名称、邮箱或 ID" aria-label="搜索账号" />
        </div>
        <div class="segmented" role="group" aria-label="筛选">
          <button :aria-pressed="filter === 'all'" @click="filter = 'all'">全部 {{ counts.all }}</button>
          <button :aria-pressed="filter === 'available'" @click="filter = 'available'">可用 {{ counts.available }}</button>
          <button :aria-pressed="filter === 'unavailable'" @click="filter = 'unavailable'">不可用 {{ counts.unavailable }}</button>
        </div>
      </div>

      <div v-if="loading" class="list">
        <div v-for="i in 3" :key="i" class="skeleton" style="height: 76px; border-radius: 8px" />
      </div>

      <div v-else-if="!accounts.length" class="card">
        <EmptyState icon="people" title="还没有账号" message="导入 AI Studio 的 Cookie 或 storage state 后，网关才能代理生成请求。">
          <button class="btn accent" @click="importOpen = true"><FIcon name="add" />导入账号</button>
        </EmptyState>
      </div>

      <div v-else-if="!filtered.length" class="card">
        <EmptyState icon="filter_dismiss" title="没有匹配的账号" />
      </div>

      <ul v-else class="list accounts" aria-label="账号列表">
        <li v-for="account in filtered" :key="account.id" class="account card" :class="{ dim: account.disabled }">
          <div class="main">
            <div class="avatar" aria-hidden="true">{{ initials(account) }}</div>
            <div class="info">
              <div class="title-row">
                <strong class="truncate">{{ accountLabel(account) }}</strong>
                <span v-if="account.id === rotation?.preferred_account_id" class="badge accent"><FIcon name="star" filled />首选</span>
                <span v-if="account.id === service.status?.active_generation_account_id" class="badge success"><FIcon name="record" filled />生成中</span>
              </div>
              <div class="meta t-caption t-secondary">
                <span v-if="account.email" class="truncate"><FIcon name="mail" :size="12" /> {{ account.email }}</span>
                <span v-if="account.auth_user !== undefined && account.auth_user !== ''">authuser={{ account.auth_user }}</span>
                <span class="mono id truncate" :title="account.id">{{ account.id }}</span>
                <span
                  v-if="account.fingerprint"
                  class="mono"
                  :title="`浏览器指纹种子 ${account.fingerprint.seed}；时区 ${account.fingerprint.timezone || '跟随本机'}；语言 ${account.fingerprint.locale || '跟随本机'}`"
                >
                  <FIcon name="desktop" :size="12" /> #{{ account.fingerprint.seed }}
                </span>
                <span v-if="account.backend" :title="`浏览器后端：${backendLabel(account.backend)}`">{{ backendLabel(account.backend) }}</span>
              </div>
            </div>
            <span class="badge state" :class="accountState(account, clock).tone !== 'neutral' ? accountState(account, clock).tone : ''">
              <FIcon :name="accountState(account, clock).icon" filled />{{ accountState(account, clock).text }}
            </span>
            <div class="actions">
              <ProgressRing v-if="busy[account.id]" :label="busy[account.id]" />
              <button
                v-if="account.id !== rotation?.preferred_account_id"
                class="btn prefer"
                :disabled="!!busy[account.id] || account.disabled"
                @click="activate(account)"
              >
                <FIcon name="star" />设为首选
              </button>
              <button
                v-if="account.failures?.length"
                class="btn subtle icon-only"
                :aria-expanded="!!expanded[account.id]"
                :aria-label="expanded[account.id] ? '收起冷却详情' : '展开冷却详情'"
                :title="expanded[account.id] ? '收起冷却详情' : '冷却详情'"
                @click="expanded[account.id] = !expanded[account.id]"
              >
                <FIcon :name="expanded[account.id] ? 'chevron_down' : 'chevron_right'" />
              </button>
              <FMenu :items="menuFor(account)" />
            </div>
          </div>

          <div v-if="probes[account.id]" class="probe" :class="probes[account.id].status === 'ready' ? 'ok' : 'bad'">
            <FIcon :name="probes[account.id].status === 'ready' ? 'checkmark_circle' : 'warning'" filled />
            <span class="grow">探测结果：{{ probeText(probes[account.id]) }}</span>
            <button class="btn subtle icon-only" aria-label="关闭探测结果" @click="delete probes[account.id]"><FIcon name="dismiss" /></button>
          </div>

          <div v-if="expanded[account.id] && account.failures?.length" class="cooldowns">
            <table class="table">
              <thead>
                <tr>
                  <th>模型</th>
                  <th>原因</th>
                  <th>计数</th>
                  <th>冷却至</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                <tr v-for="f in account.failures" :key="f.model">
                  <td class="mono break">{{ f.model === '*' ? '全局（所有模型）' : f.model }}</td>
                  <td>{{ cooldownReason(f.kind) }}</td>
                  <td class="num">{{ f.count }}</td>
                  <td>
                    <span v-if="isIndefinite(f.until)" class="badge critical" title="权限类失败不会自动解除">
                      <FIcon name="person_prohibited" filled />需手动清除
                    </span>
                    <span v-else :class="f.until > clock ? '' : 't-tertiary'" :title="formatTimestamp(f.until)">
                      {{ f.until > clock ? formatRelative(f.until, clock) : '已结束' }}
                    </span>
                  </td>
                  <td class="num">
                    <button class="btn subtle" :disabled="!!busy[account.id]" @click="clearCooldown(account, f.model)">清除</button>
                  </td>
                </tr>
              </tbody>
            </table>
            <p class="cooldown-hint t-caption t-secondary">
              额度类限制（每分钟、每日、按模型）会持久记录：同一太平洋时间日内重复触发时冷却逐级延长，按上游重试提示最长可达一天，凭据刷新后仍然保留；清除冷却会同时清除额度计数。权限类失败不会自动解除。“需同意条款”表示 AI Studio 要求账号同意条款，ais2api 只会自动继续欢迎对话框：请用此账号在普通浏览器中登录 AI Studio 并同意，再清除冷却。
            </p>
          </div>
        </li>
      </ul>
    </div>

    <ImportAccountDialog v-model:open="importOpen" @imported="load" />
    <FingerprintDialog v-model:open="fingerprintOpen" :account="fingerprintTarget" @changed="load" />

    <FDialog v-model:open="renameOpen" title="重命名账号" :width="400">
      <div class="field">
        <label for="rename-input">名称</label>
        <input id="rename-input" v-model="renameValue" class="input" maxlength="128" autofocus @keydown.enter="submitRename" />
        <span class="hint">留空则显示邮箱或 ID。</span>
      </div>
      <template #footer>
        <button class="btn accent" @click="submitRename">保存</button>
        <button class="btn" @click="renameTarget = null">取消</button>
      </template>
    </FDialog>
  </div>
</template>

<style scoped>
.toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 8px 12px;
  align-items: center;
}
.search {
  flex: 1 1 260px;
  max-width: 420px;
}
.accounts {
  list-style: none;
  margin: 0;
  padding: 0;
}
.account {
  display: flex;
  flex-direction: column;
}
.account.dim .info,
.account.dim .avatar {
  opacity: 0.6;
}
.main {
  display: flex;
  align-items: center;
  gap: 12px 16px;
  padding: 12px 12px 12px 16px;
  min-height: 72px;
}
.avatar {
  flex: none;
  display: grid;
  place-items: center;
  width: 40px;
  height: 40px;
  border-radius: 50%;
  background: var(--accent-track);
  color: var(--accent-text);
  font-weight: 600;
}
.info {
  flex: 1 1 auto;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.title-row {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}
.meta {
  display: flex;
  flex-wrap: wrap;
  gap: 2px 16px;
  min-width: 0;
}
.meta span {
  display: inline-flex;
  align-items: center;
  gap: 4px;
}
.id {
  font-size: 11.5px;
  max-width: 280px;
}
.state {
  flex: none;
}
.actions {
  flex: none;
  display: flex;
  align-items: center;
  gap: 4px;
}
.probe {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 0 12px 12px;
  padding: 4px 4px 4px 12px;
  border-radius: var(--radius-control);
  background: var(--success-bg);
  color: var(--text-primary);
}
.probe .icon {
  color: var(--success);
}
.probe.bad {
  background: var(--caution-bg);
}
.probe.bad .icon {
  color: var(--caution);
}
.cooldown-hint {
  padding: 8px 12px 10px;
  border-top: 1px solid var(--divider);
}
.cooldowns {
  margin: 0 12px 12px;
  border: 1px solid var(--card-stroke);
  border-radius: var(--radius-overlay);
  background: var(--card-secondary);
  overflow-x: auto;
}
@media (max-width: 760px) {
  .main {
    flex-wrap: wrap;
  }
  .info {
    flex-basis: calc(100% - 56px);
  }
  .state {
    margin-left: 56px;
  }
  .actions {
    margin-left: auto;
  }
}
@media (max-width: 400px) {
  .prefer {
    padding: 0;
    width: 40px;
    font-size: 0;
    gap: 0;
  }
}
</style>
