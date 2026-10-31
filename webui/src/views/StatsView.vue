<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter, type LocationQueryRaw } from 'vue-router'
import EmptyState from '../components/EmptyState.vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import PageHeader from '../components/PageHeader.vue'
import RequestDetails from '../components/RequestDetails.vue'
import StatTile from '../components/StatTile.vue'
import SuccessMeter from '../components/SuccessMeter.vue'
import { api, describeError } from '../api/client'
import type { Account, PriceStatus, RequestDetail, RequestPage, UsageGroup, UsageSummary } from '../api/types'
import { confirm, downloadFile, notify, notifyError } from '../stores/ui'
import { accountLabel } from '../utils/accounts'
import { formatCompact, formatNumber, formatPercent, formatTimestamp, usageTotals } from '../utils/format'
import { activeRequest, dollars, estimateDollars, pricingCoverage, ranges, requestStatusLabel, requestStatuses, usageBounds, type UsageRange } from '../utils/usage'

const route = useRoute(), router = useRouter()
const queryText = (key: string) => typeof route.query[key] === 'string' ? route.query[key] as string : ''
const tab = computed(() => queryText('tab') === 'requests' ? 'requests' : 'overview')
const summary = ref<UsageSummary | null>(null), page = ref<RequestPage | null>(null)
const accounts = ref<Account[]>([]), prices = ref<PriceStatus | null>(null)
const loading = ref(false), loadError = ref(''), priceError = ref('')
const group = ref<'model' | 'account' | 'detail'>('model')
const range = ref<UsageRange>('all'), start = ref(''), end = ref(''), model = ref(''), account = ref(''), status = ref(''), requestId = ref('')
const clock = ref(new Date())
const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone
const detailOpen = ref(false), detail = ref<RequestDetail | null>(null), detailError = ref(''), detailLoading = ref(false)
let selectedId = '', controller: AbortController | undefined, detailController: AbortController | undefined
let interval: ReturnType<typeof setInterval> | undefined
const accountName = (id: string) => id ? accountLabel(accounts.value.find(a => a.id === id), id) : '未分配账号'
const timestamp = (ms: number) => formatTimestamp(ms / 1000)
const cursors = computed(() => queryText('cursors').split(',').filter(value => /^\d+$/.test(value)))
const isFirstPage = computed(() => !queryText('before'))
const isLatestPage = computed(() => isFirstPage.value && !queryText('snapshot'))
const appliedRange = computed<UsageRange>(() => {
  const value = queryText('range')
  return Object.hasOwn(ranges, value) ? value as UsageRange : tab.value === 'requests' ? 'today' : 'all'
})
function filters() {
  const bounds = usageBounds(appliedRange.value, clock.value, queryText('start'), queryText('end'))
  if (queryText('snapshot') && queryText('from') && queryText('to')) {
    bounds.from = Number(queryText('from')); bounds.to = Number(queryText('to'))
  }
  return { ...bounds, model: queryText('model') || undefined, account: queryText('account') || undefined,
    status: queryText('status') || undefined, id: queryText('id') || undefined }
}
async function load() {
  controller?.abort()
  const current = new AbortController(); controller = current
  loading.value = true
  try {
    const query = filters()
    if (tab.value === 'requests') {
      const result = await api.get<RequestPage>('/usage/requests', { signal: current.signal, query: { ...query, limit: 50,
        before: queryText('before') || undefined, snapshot: queryText('snapshot') || undefined } })
      if (!current.signal.aborted) page.value = result
    } else {
      const scope = appliedRange.value === 'all' && !query.id && !query.status ? 'lifetime' : 'filtered'
      const result = await api.get<UsageSummary>('/usage/summary', { signal: current.signal, query: { ...query, scope } })
      if (!current.signal.aborted) summary.value = result
    }
    if (!current.signal.aborted) loadError.value = ''
  } catch (error) { if (!current.signal.aborted) loadError.value = describeError(error) }
  finally { if (controller === current) loading.value = false }
}
function baseQuery(): LocationQueryRaw {
  const q = { ...route.query }
  for (const key of ['before', 'snapshot', 'cursors', 'from', 'to']) delete q[key]
  return q
}
function apply() {
  try { usageBounds(range.value, new Date(), start.value, end.value) }
  catch (error) { loadError.value = describeError(error); return }
  void router.push({ query: { tab: tab.value, range: range.value, start: range.value === 'custom' ? start.value : undefined,
    end: range.value === 'custom' ? end.value : undefined, model: model.value || undefined,
    account: account.value || undefined, status: status.value || undefined, id: requestId.value || undefined } })
}
function switchTab(next: 'overview' | 'requests', carry = false, row?: Row) {
  const query: LocationQueryRaw = carry ? baseQuery() : { range: next === 'requests' ? 'today' : 'all' }
  query.tab = next
  if (row) { query.model = row.model || undefined; query.account = row.account || undefined }
  void router.push({ query })
}
function refresh() {
  clock.value = new Date()
  if (!isLatestPage.value) void router.replace({ query: baseQuery() })
  else void load()
}
function nextPage() {
  if (page.value?.next_cursor == null) return
  const bounds = filters()
  void router.push({ query: { ...route.query, before: String(page.value.next_cursor), snapshot: String(page.value.snapshot),
    cursors: [...cursors.value, queryText('before') || '0'].join(','), from: bounds.from, to: bounds.to } })
}
function previousPage() {
  const trail = [...cursors.value], before = trail.pop()
  if (!before || before === '0') void router.push({ query: { ...route.query, before: undefined, cursors: undefined } })
  else void router.push({ query: { ...route.query, before, cursors: trail.join(',') } })
}
watch(() => route.fullPath, () => {
  range.value = appliedRange.value; start.value = queryText('start'); end.value = queryText('end')
  model.value = queryText('model'); account.value = queryText('account'); status.value = queryText('status'); requestId.value = queryText('id')
  clock.value = new Date(); void load()
}, { immediate: true })

async function loadPrices() {
  try { prices.value = await api.get<PriceStatus>('/pricing'); priceError.value = '' }
  catch (error) { priceError.value = describeError(error) }
}
async function refreshPrices() {
  try { prices.value = await api.post<PriceStatus>('/pricing/refresh', {}); priceError.value = '' }
  catch (error) { priceError.value = describeError(error) }
}
async function loadDetail() {
  detailController?.abort()
  const current = new AbortController(); detailController = current; detailLoading.value = true
  try {
    const result = await api.get<RequestDetail>(`/usage/requests/${encodeURIComponent(selectedId)}`, { signal: current.signal })
    if (!current.signal.aborted) { detail.value = result; detailError.value = '' }
  } catch (error) { if (!current.signal.aborted) detailError.value = describeError(error) }
  finally { if (detailController === current) detailLoading.value = false }
}
function openDetail(id: string) {
  selectedId = id; detail.value = null; detailError.value = ''; detailOpen.value = true; void loadDetail()
}
watch(detailOpen, value => { if (!value) detailController?.abort() })
let ticking = false
async function tick() {
  if (document.hidden || ticking) return
  ticking = true
  try {
    if (detailOpen.value) {
      if (detail.value && activeRequest(detail.value.status) && !detailLoading.value) await loadDetail()
    } else if (tab.value === 'requests' && appliedRange.value === 'today' && isLatestPage.value && !loading.value) {
      clock.value = new Date(); await load()
    }
    if (prices.value?.refreshing) await loadPrices()
  } finally { ticking = false }
}
const visible = () => { if (!document.hidden) void tick() }
onMounted(async () => {
  interval = setInterval(() => { void tick() }, 10000)
  document.addEventListener('visibilitychange', visible)
  await Promise.all([loadPrices(), (async () => {
    try { accounts.value = (await api.get<{ accounts: Account[] }>('/accounts')).accounts }
    catch (error) { loadError.value = describeError(error) }
  })()])
})
onBeforeUnmount(() => { controller?.abort(); detailController?.abort(); clearInterval(interval); document.removeEventListener('visibilitychange', visible) })

interface Row { key: string; label: string; sub: string; model: string; account: string; success: number; failure: number; total: number; amount: number; unpriced: number; partial: number }
const rows = computed<Row[]>(() => {
  const result = new Map<string, Row>()
  for (const r of summary.value?.records ?? []) {
    const key = group.value === 'model' ? r.model : group.value === 'account' ? r.account : `${r.account}\u0000${r.model}`
    const row = result.get(key) ?? { key, label: group.value === 'account' ? accountName(r.account) : r.model || '未解析模型',
      sub: group.value === 'detail' ? accountName(r.account) : '', model: group.value === 'account' ? '' : r.model,
      account: group.value === 'model' ? '' : r.account || ':unassigned', success: 0, failure: 0, total: 0, amount: 0, unpriced: 0, partial: 0 }
    row.success += r.success; row.failure += r.failure; row.total += usageTotals(r.usage).total; row.amount += r.known_amount_nano; row.unpriced += r.unpriced; row.partial += r.partially_priced
    result.set(key, row)
  }
  return [...result.values()].sort((a, b) => b.success + b.failure - a.success - a.failure)
})
const totals = computed(() => summary.value?.totals)
const totalRequests = computed(() => (totals.value?.success ?? 0) + (totals.value?.failure ?? 0))
const detailSince = computed(() => tab.value === 'requests' ? page.value?.detail_since : summary.value?.detail_since)
const partialRange = computed(() => {
  if (tab.value === 'overview' && summary.value?.scope === 'lifetime') return false
  try { return !!detailSince.value && (filters().from ?? 0) < detailSince.value } catch { return false }
})
async function resetStats() {
  if (!await confirm({ title: '重置统计', message: '将清空新旧累计用量、估算费用和全部已结束请求记录。此操作无法撤销；有进行中的请求时不能重置。', confirmText: '重置', danger: true, typeToConfirm: '重置' })) return
  try { await api.delete('/stats'); notify('success', '统计已重置'); refresh() }
  catch (error) { notifyError('重置失败', error) }
}
function exportCsv() {
  const header = ['account_id', 'account', 'model', 'success', 'failure', 'prompt_tokens', 'output_tokens', 'total_tokens', 'estimated_usd', 'unpriced_requests', 'known_estimated_usd', 'partially_priced_requests']
  const quote = (value: string | number) => `"${String(value).replace(/"/g, '""')}"`
  const lines = (summary.value?.records ?? []).map((r: UsageGroup) => {
    const u = usageTotals(r.usage)
    return [r.account, accountName(r.account), r.model, r.success, r.failure, u.prompt, u.output, u.total, (r.amount_nano / 1e9).toFixed(9), r.unpriced, (r.known_amount_nano / 1e9).toFixed(9), r.partially_priced].map(quote).join(',')
  })
  downloadFile(`ais2api-stats-${new Date().toISOString().slice(0, 10)}.csv`, '\uFEFF' + [header.join(','), ...lines].join('\r\n'), 'text/csv')
}
</script>

<template>
  <div>
    <PageHeader title="用量统计" subtitle="查看调用用量、参考价格估算与每次请求的执行情况。">
      <button v-if="tab === 'overview'" class="btn" :disabled="!summary?.records.length" @click="exportCsv">
        <FIcon name="arrow_download" />导出 CSV</button>
      <button class="btn" :disabled="loading" @click="refresh">
        <FIcon name="arrow_clockwise" />刷新</button>
      <button v-if="tab === 'overview'" class="btn subtle" @click="resetStats">
        <FIcon name="delete" />重置</button>
    </PageHeader>
    <div class="stack" style="--gap: 16px">
      <div class="segmented tabs" role="group" aria-label="用量视图">
        <button :aria-pressed="tab === 'overview'" @click="switchTab('overview')">统计概览</button>
        <button :aria-pressed="tab === 'requests'" @click="switchTab('requests')">请求记录</button>
      </div>
      <form class="card filters" @submit.prevent="apply">
        <label>时间范围<select v-model="range" class="select">
            <option v-for="(label, key) in ranges" :key="key" :value="key">{{ key === 'all' && tab === 'requests' ? '全部可查记录' : label }}</option>
          </select>
        </label>
        <template v-if="range === 'custom'">
          <label>开始日期<input v-model="start" class="input" type="date" />
          </label>
          <label>结束日期<input v-model="end" class="input" type="date" />
          </label>
        </template>
        <label>模型<input v-model="model" class="input" placeholder="全部模型" />
        </label>
        <label>账号<select v-model="account" class="select">
            <option value="">全部账号</option>
            <option value=":unassigned">未分配账号</option>
            <option v-for="a in accounts" :key="a.id" :value="a.id">{{ accountName(a.id) }}</option>
          </select>
        </label>
        <label>状态<select v-model="status" class="select">
            <option value="">全部状态</option>
            <option v-for="(label, key) in requestStatuses" :key="key" :value="key">{{ label }}</option>
          </select>
        </label>
        <label>请求编号<input v-model="requestId" class="input mono" placeholder="完整请求编号" />
        </label>
        <div class="row">
          <button class="btn accent" type="submit">查询</button>
          <button class="btn" type="button" @click="switchTab(tab)">重置筛选</button>
        </div>
      </form>
      <div class="row wrap t-caption t-secondary">
        <span class="grow">{{ ranges[appliedRange] }} · {{ timezone }}<template v-if="detailSince"> · 明细可查起点 {{ timestamp(detailSince) }}</template>
        </span>
        <button class="btn subtle" @click="switchTab(tab === 'requests' ? 'overview' : 'requests', true)">{{ tab === 'requests' ? '查看此范围统计' : '查看此范围请求' }}</button>
      </div>
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />
      <p v-if="partialRange" class="t-caption t-secondary">此范围仅包含明细可查起点之后的记录；更早的累计可在统计概览的“累计”中查看。</p>
      <template v-if="tab === 'overview'">
        <section class="card-grid" style="--min: 180px" aria-label="合计">
          <StatTile label="请求总数" :value="formatCompact(totalRequests)" icon="data_bar_vertical" caption="已结束的客户端请求" />
          <StatTile label="成功率" :value="totalRequests ? formatPercent((totals?.success ?? 0) / totalRequests) : '—'" icon="checkmark_circle" :caption="`失败或中断 ${formatNumber(totals?.failure ?? 0)} 次`" />
          <StatTile label="Tokens" :value="formatCompact(usageTotals(totals?.usage).total)" icon="arrow_upload" :caption="`输入 ${formatCompact(usageTotals(totals?.usage).prompt)} · 输出 ${formatCompact(usageTotals(totals?.usage).output)}`" />
          <StatTile class="cost-tile" :label="summary?.scope === 'lifetime' ? '累计已知费用 · USD' : '范围已知费用 · USD'" :value="totals ? dollars(totals.known_amount_nano) : '—'" icon="data_bar_vertical" :caption="totals ? pricingCoverage(totals) : '正在加载…'" />
        </section>
        <section class="card">
          <div class="row wrap section-head">
            <h2 class="t-body-strong grow">分组明细</h2>
            <div class="segmented" role="group" aria-label="分组方式">
              <button :aria-pressed="group === 'model'" @click="group = 'model'">按模型</button>
              <button :aria-pressed="group === 'account'" @click="group = 'account'">按账号</button>
              <button :aria-pressed="group === 'detail'" @click="group = 'detail'">账号 × 模型</button>
            </div>
          </div>
          <EmptyState v-if="!loading && !rows.length" icon="data_bar_vertical" title="暂无统计" message="请求结束后，这里会显示用量和估算费用。" />
          <div v-else class="table-wrap">
            <table class="table">
              <thead>
                <tr>
                  <th>{{ group === 'account' ? '账号' : '模型' }}</th>
                  <th class="num">请求数</th>
                  <th>成功率</th>
                  <th class="num">Tokens</th>
                  <th class="num">估算费用</th>
                  <th>
                  </th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="row in rows" :key="row.key">
                  <td class="name">{{ row.label }}<div v-if="row.sub" class="t-caption t-secondary">{{ row.sub }}</div>
                  </td>
                  <td class="num">{{ formatNumber(row.success + row.failure) }}</td>
                  <td>
                    <SuccessMeter :success="row.success" :failure="row.failure" compact />
                  </td>
                  <td class="num">{{ formatCompact(row.total) }}</td>
                  <td class="num">{{ dollars(row.amount) }}<div v-if="row.unpriced" class="t-caption t-secondary">部分 {{ row.partial }} 次 · 未知 {{ row.unpriced - row.partial }} 次</div>
                  </td>
                  <td>
                    <button class="btn subtle" @click="switchTab('requests', true, row)">查看请求</button>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>
        <p v-if="summary" class="t-caption t-secondary">统计起点 {{ timestamp(summary.since) }}。金额为已知费用的估算小计，包含部分计价请求可确认的费用；账号请求数归属最终执行账号，用量和费用按实际消耗账号归集。</p>
        <details v-if="summary?.legacy_records.length" class="card section-head">
          <summary>启用请求记录前的历史累计 · 原统计口径</summary>
          <p class="t-caption t-secondary">历史失败计数可能包含内部重试，保留原值；没有对应逐条记录或估算费用。</p>
          <div class="table-wrap">
            <table class="table">
              <thead>
                <tr>
                  <th>账号 / 模型</th>
                  <th class="num">成功</th>
                  <th class="num">失败</th>
                  <th class="num">Tokens</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="r in summary.legacy_records" :key="`${r.account}:${r.model}`">
                  <td>{{ accountName(r.account) }} / {{ r.model }}</td>
                  <td class="num">{{ formatNumber(r.success) }}</td>
                  <td class="num">{{ formatNumber(r.failure) }}</td>
                  <td class="num">{{ formatCompact(usageTotals(r.usage).total) }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </details>
        <details class="card section-head">
          <summary>参考价格</summary>
          <div class="stack" style="--gap: 8px; margin-top: 12px">
            <FInfoBar v-if="priceError || prices?.error" severity="error" title="价格更新失败" :message="priceError || prices?.error || ''" />
            <div v-for="source in prices?.sources" :key="source.source" class="row wrap">
              <strong>{{ source.source }}</strong>
              <span>{{ source.models }} 个模型</span>
              <span class="t-caption t-secondary grow">{{ source.fetched_at ? formatTimestamp(source.fetched_at) : '尚未获取' }}</span>
              <span v-if="source.error" class="t-caption">更新失败：{{ source.error }}</span>
            </div>
            <p class="t-caption t-secondary">每日自动更新，更新失败继续使用已保存价格。每次请求保留当时的价格；估算结果不代表实际账单。</p>
            <button class="btn" :disabled="prices?.refreshing" @click="refreshPrices">{{ prices?.refreshing ? '正在更新…' : '更新参考价格' }}</button>
          </div>
        </details>
      </template>
      <section v-else class="card">
        <div class="row wrap section-head">
          <h2 class="t-body-strong grow">{{ formatNumber(page?.total ?? 0) }} 条请求</h2>
          <span class="t-caption t-secondary">{{ appliedRange === 'today' && isLatestPage && !detailOpen ? '每 10 秒刷新' : '手动刷新' }}</span>
        </div>
        <p v-if="loading && !page" class="section-head">正在加载…</p>
        <EmptyState v-else-if="!page?.records.length" icon="data_bar_vertical" :title="appliedRange === 'today' ? '今天暂无请求' : '此范围暂无请求'" message="可切换昨天、近 7 天或调整筛选查看。" />
        <div v-else class="table-wrap">
          <table class="table requests">
            <thead>
              <tr>
                <th>时间 / 模型</th>
                <th class="desktop">账号</th>
                <th>状态</th>
                <th class="desktop num">耗时</th>
                <th class="num">Tokens</th>
                <th class="num">估算费用</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="record in page.records" :key="record.id" @click="openDetail(record.id)">
                <td class="name">
                  <button class="request-link" @click.stop="openDetail(record.id)">{{ record.model || '未解析模型' }}</button>
                  <div class="t-caption t-secondary">{{ timestamp(record.created_at) }}</div>
                </td>
                <td class="desktop">{{ accountName(record.account) }}</td>
                <td>{{ requestStatusLabel(record) }}</td>
                <td class="desktop num">{{ record.duration_ms == null ? '—' : `${(record.duration_ms / 1000).toFixed(1)}s` }}</td>
                <td class="num">{{ formatCompact(usageTotals(record.usage).total) }}</td>
                <td class="num">{{ activeRequest(record.status) ? '—' : estimateDollars(record.cost) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <div class="row section-head">
          <button class="btn" :disabled="isFirstPage || loading" @click="previousPage">上一页</button>
          <span class="grow t-caption t-secondary">第 {{ cursors.length + 1 }} 页 · 每页 50 条</span>
          <button class="btn" :disabled="page?.next_cursor == null || loading" @click="nextPage">下一页</button>
        </div>
      </section>
    </div>
    <RequestDetails v-model:open="detailOpen" :record="detail" :error="detailError" :loading="detailLoading" :account-name="accountName" @refresh="loadDetail" />
  </div>
</template>

<style scoped>
.tabs { align-self: flex-start; }
.cost-tile :deep(.bottom .truncate) { white-space: normal; overflow: visible; }
.filters { display: flex; flex-wrap: wrap; gap: 12px; padding: 16px; align-items: end; }
.filters label { display: grid; gap: 6px; flex: 1 1 160px; min-width: 0; }
.filters .select, .filters .input { width: 100%; min-width: 0; }
.section-head { padding: 16px; }
.name { max-width: 280px; overflow-wrap: anywhere; }
.request-link { background: transparent; border: 0; color: var(--accent); text-align: left; padding: 0; font: inherit; cursor: pointer; overflow-wrap: anywhere; }
.requests tbody tr { cursor: pointer; }
summary { cursor: pointer; }
@media (max-width: 640px) {
  .desktop { display: none; }
  .filters { padding: 12px; gap: 10px; }
  .filters label { flex-basis: calc(50% - 10px); }
  .requests { font-size: 12px; }
  .requests th, .requests td { padding: 10px 6px; }
  .requests .name { max-width: 140px; }
  .requests td.num { white-space: normal; }
}
</style>
