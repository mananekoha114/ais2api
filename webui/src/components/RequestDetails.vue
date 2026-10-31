<script setup lang="ts">
import { computed } from 'vue'
import FDialog from './FDialog.vue'
import FInfoBar from './FInfoBar.vue'
import type { RequestDetail } from '../api/types'
import { formatNumber, formatTimestamp } from '../utils/format'
import { activeRequest, attemptsSummary, costKinds, costReasons, dollars, estimateCoverage, estimateDollars, finishLabel, generationSettings, rateDollars, requestStatusLabel } from '../utils/usage'

const props = defineProps<{ record: RequestDetail | null; error: string; loading: boolean; accountName: (id: string) => string }>()
const open = defineModel<boolean>('open', { default: false })
const emit = defineEmits<{ refresh: [] }>()
const timestamp = (ms: number | null) => formatTimestamp(ms == null ? null : ms / 1000)
const duration = (ms: number | null) => ms == null ? '—' : `${(ms / 1000).toFixed(3)} 秒`
const settings = computed(() => generationSettings(props.record?.settings ?? {}))
const endpoint = { chat_completions: 'Chat Completions', responses: 'Responses', gemini: 'Gemini' }
</script>

<template>
  <FDialog v-model:open="open" title="请求详情" :width="860">
    <FInfoBar v-if="error" severity="error" title="加载失败" :message="error" />
    <p v-if="loading && !record">正在加载…</p>
    <template v-if="record">
      <div class="row wrap">
        <strong>{{ requestStatusLabel(record) }}</strong>
        <span class="grow mono">{{ record.model || '尚未解析模型' }}</span>
        <strong>{{ estimateDollars(record.cost) }}</strong>
      </div>
      <dl class="facts">
        <dt>请求编号</dt>
        <dd class="mono">{{ record.id }}</dd>
        <dt>接口</dt>
        <dd>{{ endpoint[record.endpoint] }} · {{ record.stream ? '流式' : '非流式' }}</dd>
        <dt>请求模型</dt>
        <dd>{{ record.requested_model || '—' }}</dd>
        <dt>执行账号</dt>
        <dd>{{ accountName(record.account) }}</dd>
        <dt>开始时间</dt>
        <dd>{{ timestamp(record.created_at) }}</dd>
        <dt>结束时间</dt>
        <dd>{{ timestamp(record.ended_at) }}</dd>
        <dt>耗时</dt>
        <dd>排队 {{ duration(record.queue_ms) }} · 首段输出 {{ duration(record.first_token_ms) }} · 生成 {{ duration(record.generation_ms) }} · 总计 {{ duration(record.duration_ms) }}</dd>
        <dt>完成情况</dt>
        <dd>生成 {{ record.generation_completed ? '完成' : '未完成' }} · 传输 {{ record.delivery_completed ? '完成' : '未完成' }} · HTTP {{ record.http_status ?? '—' }}</dd>
        <template v-if="record.finish_reason"><dt>结束原因</dt><dd>{{ finishLabel(record.finish_reason) }} · <span class="mono">{{ record.finish_reason }}</span></dd></template>
        <template v-if="record.error">
          <dt>错误原因</dt>
          <dd>{{ record.error }}</dd>
        </template>
      </dl>
      <details v-if="settings.length">
        <summary>生成设置</summary>
        <dl class="settings-grid">
          <div v-for="setting in settings" :key="setting.key" class="generation-setting">
            <dt>{{ setting.label }}</dt>
            <dd>{{ setting.value }}</dd>
          </div>
        </dl>
      </details>
      <p class="t-caption t-secondary">{{ estimateCoverage(record.cost) }}</p>
      <p v-if="record.attempts.length > 1" class="t-caption t-secondary">{{ attemptsSummary(record.attempts) }}</p>
      <section v-for="(attempt, index) in record.attempts" :key="index" class="attempt">
        <div class="row wrap">
          <h3 class="t-body-strong grow">尝试 {{ index + 1 }} · {{ accountName(attempt.account) }}</h3>
          <span>{{ dollars(attempt.cost.amount_nano) }}</span>
        </div>
        <p class="t-caption t-secondary">{{ timestamp(attempt.started_at) }} → {{ timestamp(attempt.ended_at) }} · {{ finishLabel(attempt.finish_reason) || attempt.error || '进行中' }}<span v-if="attempt.finish_reason"> · {{ attempt.finish_reason }}</span></p>
        <p class="t-caption">输入 {{ formatNumber(attempt.usage.promptTokenCount) }} · 其中缓存 {{ formatNumber(attempt.usage.cachedContentTokenCount ?? 0) }} · 输出 {{ formatNumber(attempt.usage.candidatesTokenCount) }} · 其中思考 {{ formatNumber(attempt.usage.thoughtsTokenCount ?? 0) }}</p>
        <p v-if="attempt.cost.quote?.source" class="t-caption t-secondary">参考价：{{ attempt.cost.quote.source }} · {{ attempt.cost.quote.model }} · {{ formatTimestamp(attempt.cost.quote.fetched_at) }}<span v-if="attempt.cost.above_tokens"> · 输入超过 {{ formatNumber(attempt.cost.above_tokens) }} Tokens 的阶梯</span>
        </p>
        <div v-if="attempt.cost.lines?.length" class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th>项目</th>
                <th class="num">Tokens</th>
                <th class="num">单价</th>
                <th class="num">估算费用</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="line in attempt.cost.lines" :key="line.kind">
                <td>{{ costKinds[line.kind] ?? line.kind }}</td>
                <td class="num">{{ formatNumber(line.tokens) }}</td>
                <td class="num">{{ rateDollars(line.rate_pico) }}</td>
                <td class="num">{{ dollars(line.amount_nano) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <p v-if="attempt.cost.status === 'unpriced'" class="t-caption">{{ costReasons[attempt.cost.reason] ?? attempt.cost.reason }}</p>
      </section>
      <p v-if="!record.attempts.length" class="t-secondary">请求尚未进入上游执行。</p>
    </template>
    <template #footer>
      <button v-if="record && activeRequest(record.status)" class="btn" :disabled="loading" @click="emit('refresh')">刷新</button>
      <button class="btn accent" @click="open = false">关闭</button>
    </template>
  </FDialog>
</template>

<style scoped>
.facts { display: grid; grid-template-columns: 90px minmax(0, 1fr); gap: 8px 12px; margin: 8px 0; }
dt { color: var(--text-secondary); overflow-wrap: anywhere; }
.settings-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 180px), 1fr)); gap: 12px 20px; margin: 12px 0 0; }
.generation-setting { min-width: 0; }
.generation-setting dt { font-size: 12px; }
.generation-setting dd { margin-top: 4px; }
dd { margin: 0; overflow-wrap: anywhere; }
.attempt { border-top: 1px solid var(--card-stroke); padding-top: 12px; display: grid; gap: 8px; }
.mono { overflow-wrap: anywhere; }
summary { cursor: pointer; }
@media (max-width: 640px) {
  .facts { grid-template-columns: 76px minmax(0, 1fr); }
  .settings-grid { grid-template-columns: minmax(0, 1fr); }
}
</style>
