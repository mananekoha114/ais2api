<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import StatTile from '../components/StatTile.vue'
import SuccessMeter from '../components/SuccessMeter.vue'
import { api, describeError, runOperation } from '../api/client'
import type { Account, KeyList, Model, ProbeResult, Rotation, StatRecord } from '../api/types'
import { refreshStatus, service } from '../stores/service'
import { notify, notifyError } from '../stores/ui'
import { accountLabel, accountState } from '../utils/accounts'
import { formatCompact, formatDuration, formatNumber, usageTotals } from '../utils/format'
import { usePolling } from '../utils/media'

const rotation = ref<Rotation | null>(null)
const stats = ref<StatRecord[]>([])
const keys = ref<KeyList | null>(null)
const models = ref<Model[]>([])
const loading = ref(true)
const loadError = ref('')
const busy = ref<string | null>(null)

async function load() {
  try {
    const [r, s, k, m] = await Promise.all([
      api.get<Rotation>('/accounts/active'),
      api.get<{ records: StatRecord[] }>('/stats'),
      api.get<KeyList>('/keys'),
      api.get<{ models: Model[] }>('/models'),
      refreshStatus(),
    ])
    rotation.value = r
    stats.value = s.records ?? []
    keys.value = k
    models.value = m.models ?? []
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)
usePolling(load, 10000)

const accounts = computed<Account[]>(() => rotation.value?.accounts ?? [])
const byId = computed(() => new Map(accounts.value.map((a) => [a.id, a])))
const available = computed(() => accounts.value.filter((a) => accountState(a).available).length)
const status = computed(() => service.status)

const totals = computed(() => {
  let success = 0
  let failure = 0
  let tokens = 0
  for (const record of stats.value) {
    success += record.success
    failure += record.failure
    tokens += usageTotals(record.usage).total
  }
  return { success, failure, tokens, requests: success + failure }
})

const perAccount = computed(() => {
  const map = new Map<string, { success: number; failure: number }>()
  for (const record of stats.value) {
    const entry = map.get(record.account) ?? { success: 0, failure: 0 }
    entry.success += record.success
    entry.failure += record.failure
    map.set(record.account, entry)
  }
  return map
})

const keyCount = computed(() => keys.value?.keys.length ?? 0)

const readiness = computed(() => {
  const issues: string[] = []
  if (keys.value && keyCount.value === 0) issues.push('尚未配置任何 API 密钥')
  if (rotation.value && available.value === 0) issues.push(accounts.value.length ? '没有可用账号（禁用、过期或冷却中）' : '尚未导入账号')
  // The remaining readiness prerequisite is a usable browser executable.
  if (!issues.length) issues.push('浏览器不可用或未配置，请在服务器上运行 ais2api doctor 检查')
  return issues
})

async function action(name: string, task: () => Promise<unknown>, success: string) {
  busy.value = name
  try {
    await task()
    notify('success', success)
    await load()
  } catch (e) {
    notifyError('操作失败', e)
  } finally {
    busy.value = null
  }
}

const probeAll = () =>
  action(
    'probe',
    async () => {
      const result = await runOperation<{ accounts: ProbeResult[] }>('/accounts/probe')
      const failed = (result.accounts ?? []).filter((a) => a.status === 'failed' || a.error).length
      if (failed) notify('warning', `${failed} 个账号探测失败`, '可在“账号”页面查看详情。')
    },
    '账号探测完成',
  )
const rotateNext = () => action('rotate', () => api.post('/rotation/next'), '已切换到下一个可用账号')
const refreshModels = () => action('models', () => runOperation('/models/refresh'), '模型列表已刷新')
const reloadConfig = () => action('reload', () => api.post('/config/reload'), '配置已重新加载')
</script>

<template>
  <div>
    <PageHeader title="概览" subtitle="网关运行状态、账号健康与用量概况。">
      <button class="btn" :disabled="loading" @click="load">
        <FIcon name="arrow_clockwise" />
        刷新
      </button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />
      <FInfoBar v-if="status && !status.ready" severity="warning" title="服务尚未就绪">
        <span>{{ readiness.join('；') }}。</span>
      </FInfoBar>
      <FInfoBar v-if="status && status.logging_healthy === false" severity="warning" title="日志写入积压" :message="`待写入 ${status.pending_logs ?? 0} 条，HTTP 接入会暂停直至积压消化。`" />

      <section class="card-grid kpis" style="--min: 200px" aria-label="关键指标">
        <StatTile
          label="服务状态"
          :value="status ? (status.ready ? '就绪' : '未就绪') : '—'"
          icon="pulse"
          :caption="status ? `运行 ${formatDuration(status.uptime_seconds)}` : undefined"
          :status="
            status
              ? status.ready
                ? { tone: 'success', text: status.active ? '正在生成' : '空闲', icon: 'checkmark_circle' }
                : { tone: 'caution', text: '检查前提', icon: 'warning' }
              : undefined
          "
        />
        <StatTile
          label="可用账号"
          :value="rotation ? `${available} / ${accounts.length}` : '—'"
          icon="people"
          to="/accounts"
          :caption="accounts.length - available ? `${accounts.length - available} 个不可用` : '全部可用'"
        />
        <StatTile
          label="排队 · 连接"
          :value="status ? `${status.queued ?? 0} · ${status.connections ?? 0}` : '—'"
          icon="timer"
          caption="生成队列与活动 HTTP 连接"
        />
        <StatTile
          label="累计请求"
          :value="formatCompact(totals.requests)"
          icon="data_bar_vertical"
          to="/stats"
          :caption="totals.requests ? `成功 ${formatNumber(totals.success)} · 失败 ${formatNumber(totals.failure)}` : '暂无记录'"
        />
        <StatTile label="累计 Tokens" :value="formatCompact(totals.tokens)" icon="sparkle" to="/stats" caption="上游返回的用量元数据" />
      </section>

      <div class="columns">
        <section class="card pad stack" style="--gap: 12px" aria-labelledby="routing-title">
          <div class="row">
            <h2 id="routing-title" class="t-body-strong grow">账号调度</h2>
            <RouterLink to="/accounts" class="t-caption">管理账号</RouterLink>
          </div>
          <dl class="kv">
            <dt>首选账号</dt>
            <dd class="truncate">
              <FIcon name="star" filled class="star" v-if="rotation?.preferred_account_id" />
              {{ accountLabel(byId.get(rotation?.preferred_account_id ?? ''), rotation?.preferred_account_id ?? '未设置') }}
            </dd>
            <dt>下一个候选</dt>
            <dd class="truncate">{{ accountLabel(byId.get(rotation?.next_account_id ?? ''), rotation?.next_account_id ?? '无可用账号') }}</dd>
            <dt>正在生成</dt>
            <dd class="truncate">
              <template v-if="status?.active_generation_account_id">
                <span class="dot success" /> {{ accountLabel(byId.get(status.active_generation_account_id), status.active_generation_account_id) }}
              </template>
              <span v-else class="t-secondary">无进行中的生成</span>
            </dd>
          </dl>
          <p class="t-caption t-tertiary">首选账号与实际生成账号是不同概念；就绪仅表示本地前提满足，不代表 Google 生成已验证。</p>
        </section>

        <section class="card pad stack" style="--gap: 8px" aria-labelledby="actions-title">
          <h2 id="actions-title" class="t-body-strong">快捷操作</h2>
          <button class="btn quick" :disabled="!!busy || !accounts.length" @click="probeAll">
            <ProgressRing v-if="busy === 'probe'" /><FIcon v-else name="scan" />
            <span class="grow">探测全部账号</span>
            <span class="t-caption t-tertiary">启动浏览器</span>
          </button>
          <button class="btn quick" :disabled="!!busy || !available" @click="rotateNext">
            <ProgressRing v-if="busy === 'rotate'" /><FIcon v-else name="arrow_next" />
            <span class="grow">切换到下一个账号</span>
          </button>
          <button class="btn quick" :disabled="!!busy" @click="refreshModels">
            <ProgressRing v-if="busy === 'models'" /><FIcon v-else name="cube" />
            <span class="grow">刷新模型列表</span>
            <span class="t-caption t-tertiary">{{ models.length }} 个已缓存</span>
          </button>
          <button class="btn quick" :disabled="!!busy" @click="reloadConfig">
            <ProgressRing v-if="busy === 'reload'" /><FIcon v-else name="arrow_sync" />
            <span class="grow">重新加载配置文件</span>
          </button>
        </section>
      </div>

      <section class="card" aria-labelledby="health-title">
        <div class="row section-head">
          <h2 id="health-title" class="t-body-strong grow">账号健康</h2>
          <span class="t-caption t-secondary">成功率按全部模型合计</span>
        </div>
        <div v-if="loading" class="pad-x stack" style="--gap: 8px; padding-bottom: 16px">
          <div v-for="i in 3" :key="i" class="skeleton" style="height: 40px" />
        </div>
        <p v-else-if="!accounts.length" class="pad-x t-secondary" style="padding-bottom: 16px">尚未导入账号。<RouterLink to="/accounts">去导入</RouterLink></p>
        <ul v-else class="health-list">
          <li v-for="account in accounts" :key="account.id">
            <div class="who">
              <span class="truncate t-body-strong">
                {{ accountLabel(account) }}
                <FIcon v-if="account.id === rotation?.preferred_account_id" name="star" filled class="star" label="首选" />
              </span>
              <span class="t-caption t-secondary truncate">{{ account.email || account.id }}</span>
            </div>
            <span class="badge" :class="accountState(account).tone !== 'neutral' ? accountState(account).tone : ''">
              <FIcon :name="accountState(account).icon" filled />{{ accountState(account).text }}
            </span>
            <SuccessMeter class="meter" :success="perAccount.get(account.id)?.success ?? 0" :failure="perAccount.get(account.id)?.failure ?? 0" />
          </li>
        </ul>
      </section>

      <p v-if="status" class="t-caption t-tertiary footer-meta">
        ais2api {{ status.version }}{{ status.build_mode === 'dev' ? '（开发版）' : '' }} · 日志 {{ status.logging_healthy ? '正常' : '积压' }} · 状态每 5 秒刷新
      </p>
    </div>
  </div>
</template>

<style scoped>
.columns {
  display: grid;
  grid-template-columns: minmax(0, 1.4fr) minmax(0, 1fr);
  gap: 12px;
}
@media (max-width: 900px) {
  .columns {
    grid-template-columns: minmax(0, 1fr);
  }
}
.kv {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  gap: 10px 24px;
  margin: 0;
}
.kv dt {
  color: var(--text-secondary);
}
.kv dd {
  margin: 0;
  display: flex;
  align-items: center;
  gap: 6px;
}
.star {
  color: var(--caution);
}
.quick {
  justify-content: flex-start;
  text-align: left;
  min-height: 40px;
}
.section-head {
  padding: 16px 16px 8px;
}
.pad-x {
  padding-left: 16px;
  padding-right: 16px;
}
.health-list {
  list-style: none;
  margin: 0;
  padding: 0 0 4px;
}
.health-list li {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto minmax(140px, 220px);
  align-items: center;
  gap: 16px;
  padding: 10px 16px;
  border-top: 1px solid var(--divider);
}
.who {
  display: flex;
  flex-direction: column;
  min-width: 0;
}
.who > span:first-child {
  display: flex;
  align-items: center;
  gap: 6px;
}
@media (max-width: 640px) {
  .health-list li {
    grid-template-columns: minmax(0, 1fr) auto;
  }
  .health-list .meter {
    grid-column: 1 / -1;
  }
}
.footer-meta {
  text-align: center;
  margin-top: 4px;
}
</style>
