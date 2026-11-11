<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import FToggle from '../components/FToggle.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import { ApiError, api, describeError } from '../api/client'
import { base64ToBytes, isRaw, readLogStream, type LogBatch } from '../api/logs'
import type { LogSource } from '../api/types'
import { downloadFile } from '../stores/ui'
import { formatBytes, formatTimestamp } from '../utils/format'

interface Line {
  n: number
  text: string
  /** Structured journal record; absent for raw sources. */
  record?: Record<string, unknown>
  event?: string
  time?: number
  tone?: 'critical' | 'caution' | ''
}

const MAX_LINES = 20000
const RENDER_LIMIT = 1500
const RAW_TAIL_BYTES = 512 * 1024

const sources = ref<LogSource[]>([])
const sourceId = ref('') // '' = structured management journal
const tailOnly = ref(true)
const following = ref(true)
const wrap = ref(true)
const query = ref('')
const state = ref<'idle' | 'connecting' | 'live' | 'retrying' | 'paused'>('idle')
const error = ref('')
const notice = ref('')
const expanded = ref<number | null>(null)
const atBottom = ref(true)
const viewport = ref<HTMLElement>()

// Lines live outside Vue's reactivity; `version` is bumped once per frame.
let lines: Line[] = []
let counter = 0
const version = ref(0)
let frame = 0
const schedule = () => {
  if (frame) return
  frame = requestAnimationFrame(() => {
    frame = 0
    version.value++
  })
}

let controller: AbortController | null = null
let cursor = '0'
let decoder = new TextDecoder()
let partial = ''
let dropFirstLine = false

async function loadSources() {
  try {
    sources.value = (await api.get<{ sources: LogSource[] }>('/logs/sources')).sources ?? []
  } catch (e) {
    error.value = describeError(e)
  }
}

// Journal event names emitted by the service, with short descriptions.
const EVENT_LABELS: Record<string, string> = {
  listening: '服务开始监听',
  stopped: '服务已停止',
  http_request: 'HTTP 请求',
  config_saved: '配置已保存',
  maintenance_started: '维护任务开始',
  maintenance_done: '维护任务完成',
  maintenance_failed: '维护任务失败',
  generation: '生成完成',
  generation_attempt: '生成尝试',
  generation_parameters_ignored: '已忽略不支持的参数',
  account_failure: '账号失败并冷却',
  session_recovery: '会话就地恢复后重试',
  browser_failure: '浏览器适配失败',
  cookie_import: 'Cookie 导入',
  cookie_import_failed: 'Cookie 导入失败',
  setup_stage: '安装阶段',
  setup_download: '安装下载',
  runtime_leftovers_removed: '已清理残留的临时目录',
  backend_deprecated: '仍在使用已弃用的浏览器后端',
}

function journalTone(record: Record<string, unknown>): Line['tone'] {
  const event = String(record.event ?? '')
  const status = typeof record.status === 'number' ? record.status : 0
  // Account failures put one account on cooldown; the service keeps working.
  if (event === 'account_failure') return 'caution'
  if (/fail|error/.test(event) || status >= 500) return 'critical'
  if (status >= 400 || /ignored|cooldown|retry|expired|recovery/.test(event)) return 'caution'
  return ''
}

function summarize(record: Record<string, unknown>): string {
  return Object.entries(record)
    .filter(([key, value]) => !['timestamp', 'event', 'id'].includes(key) && value !== '' && value !== null)
    .map(([key, value]) => `${key}=${typeof value === 'object' ? JSON.stringify(value) : String(value)}`)
    .join('  ')
}

const ANSI = /\x1b\[[0-9;]*[A-Za-z]/g
function rawTone(text: string): Line['tone'] {
  if (/\b(error|fatal|fail(ed|ure)?)\b/i.test(text)) return 'critical'
  if (/\bwarn(ing)?\b/i.test(text)) return 'caution'
  return ''
}

function append(batch: LogBatch) {
  if (isRaw(batch)) {
    if (!batch.data) return
    partial += decoder.decode(base64ToBytes(batch.data), { stream: true })
    const parts = partial.split('\n')
    partial = parts.pop() ?? ''
    for (let text of parts) {
      if (dropFirstLine) {
        dropFirstLine = false
        continue
      }
      text = text.replace(/\r$/, '').replace(ANSI, '')
      lines.push({ n: counter++, text, tone: rawTone(text) })
    }
  } else {
    for (const entry of batch.entries) {
      const record = entry.record ?? {}
      lines.push({
        n: counter++,
        record,
        event: String(record.event ?? 'record'),
        time: typeof record.timestamp === 'number' ? record.timestamp : undefined,
        text: summarize(record),
        tone: journalTone(record),
      })
    }
  }
  if (lines.length > MAX_LINES) lines = lines.slice(lines.length - MAX_LINES)
  schedule()
}

function resetBuffer() {
  lines = []
  counter = 0
  partial = ''
  decoder = new TextDecoder()
  expanded.value = null
  version.value++
}

function startCursor(): string {
  if (!sourceId.value || !tailOnly.value) return '0'
  const bytes = sources.value.find((s) => s.id === sourceId.value)?.bytes ?? 0
  const start = Math.max(0, bytes - RAW_TAIL_BYTES)
  dropFirstLine = start > 0
  return String(start)
}

const sleep = (ms: number, signal: AbortSignal) =>
  new Promise<void>((resolve) => {
    const t = setTimeout(resolve, ms)
    signal.addEventListener('abort', () => (clearTimeout(t), resolve()))
  })

async function follow() {
  controller?.abort()
  const local = new AbortController()
  controller = local
  error.value = ''
  let delay = 1000
  while (!local.signal.aborted) {
    state.value = 'connecting'
    try {
      cursor = await readLogStream({
        source: sourceId.value || undefined,
        cursor,
        signal: local.signal,
        onOpen: () => {
          state.value = 'live'
          error.value = ''
          delay = 1000
        },
        onBatch: (batch, next) => {
          append(batch)
          cursor = next
        },
      })
    } catch (e) {
      if (local.signal.aborted) break
      if (e instanceof ApiError && e.reason === 'log_truncated') {
        notice.value = '日志文件已被截断或轮换，已从头重新读取。'
        resetBuffer()
        cursor = '0'
        dropFirstLine = false
        continue
      }
      if (e instanceof ApiError && (e.status === 401 || e.status === 404)) {
        error.value = describeError(e)
        state.value = 'idle'
        return
      }
      error.value = describeError(e)
    }
    if (local.signal.aborted) break
    state.value = 'retrying'
    await sleep(delay, local.signal)
    delay = Math.min(delay * 2, 15000)
  }
}

function stop(paused = true) {
  controller?.abort()
  controller = null
  state.value = paused ? 'paused' : 'idle'
}

function restart() {
  stop(false)
  resetBuffer()
  notice.value = ''
  cursor = startCursor()
  if (following.value) follow()
  else readOnce()
}

/** History-only mode: page through to the current end, then stop. */
async function readOnce() {
  controller?.abort()
  const local = new AbortController()
  controller = local
  state.value = 'connecting'
  try {
    for (;;) {
      const batch = await api.get<LogBatch>('/logs', { query: { source: sourceId.value || undefined, cursor, limit: 500 }, signal: local.signal })
      append(batch)
      cursor = batch.next_cursor
      if (!batch.has_more || local.signal.aborted) break
    }
    state.value = 'paused'
  } catch (e) {
    if (!local.signal.aborted) {
      error.value = describeError(e)
      state.value = 'idle'
    }
  }
}

watch(following, (value) => {
  if (value) follow()
  else stop()
})
watch([sourceId, tailOnly], restart)

onMounted(async () => {
  await loadSources()
  restart()
})
onBeforeUnmount(() => {
  stop(false)
  cancelAnimationFrame(frame)
})

// ---------- Rendering ----------
const visible = computed(() => {
  void version.value
  const q = query.value.trim().toLowerCase()
  const matched = q
    ? lines.filter((l) => l.text.toLowerCase().includes(q) || l.event?.toLowerCase().includes(q) || (l.event && EVENT_LABELS[l.event]?.includes(q)))
    : lines
  return { total: matched.length, rows: matched.slice(-RENDER_LIMIT) }
})

function onScroll() {
  const el = viewport.value
  if (!el) return
  atBottom.value = el.scrollHeight - el.scrollTop - el.clientHeight < 32
}
function scrollToEnd() {
  const el = viewport.value
  if (el) el.scrollTop = el.scrollHeight
  atBottom.value = true
}
watch(version, async () => {
  if (!atBottom.value) return
  await nextTick()
  scrollToEnd()
})

const stateLabel = computed(
  () =>
    ({
      idle: { text: '未连接', tone: '' },
      connecting: { text: '连接中', tone: '' },
      live: { text: '实时', tone: 'success' },
      retrying: { text: '重连中', tone: 'caution' },
      paused: { text: '已暂停', tone: '' },
    })[state.value],
)

function download() {
  const isJournal = !sourceId.value
  const body = lines.map((l) => (isJournal ? JSON.stringify(l.record) : l.text)).join('\n') + '\n'
  downloadFile(`ais2api-${sourceId.value || 'journal'}-${new Date().toISOString().replace(/[:.]/g, '-')}.${isJournal ? 'jsonl' : 'log'}`, body, 'text/plain')
}

function clearView() {
  lines = []
  expanded.value = null
  version.value++
}
</script>

<template>
  <div class="logs-page">
    <PageHeader title="日志" subtitle="完整历史与实时推送，支持断线续读。日志不会被自动截断。">
      <button class="btn" :disabled="!version" @click="download"><FIcon name="arrow_download" />下载</button>
      <button class="btn subtle" @click="clearView"><FIcon name="eraser" />清屏</button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="error && state !== 'live'" severity="error" :message="error" />
      <FInfoBar v-if="notice" severity="warning" :message="notice" closable @close="notice = ''" />

      <div class="toolbar card">
        <div class="field source">
          <label for="log-source" class="sr-only">日志源</label>
          <select id="log-source" v-model="sourceId" class="select">
            <option value="">结构化日志（管理 journal）</option>
            <option v-for="s in sources" :key="s.id" :value="s.id">{{ s.id }} · {{ formatBytes(s.bytes) }}</option>
          </select>
        </div>
        <button class="btn subtle icon-only" title="刷新日志源列表" aria-label="刷新日志源列表" @click="loadSources"><FIcon name="arrow_clockwise" /></button>
        <div class="input-group filter">
          <span class="lead"><FIcon name="filter" /></span>
          <input v-model="query" class="input has-lead" type="search" placeholder="过滤" aria-label="过滤日志" />
        </div>
        <div class="row toggles">
          <FToggle v-model="following" on-text="跟踪" off-text="跟踪" label="实时跟踪" />
          <FToggle v-model="wrap" on-text="换行" off-text="换行" label="自动换行" />
          <FToggle v-if="sourceId" v-model="tailOnly" on-text="仅末尾" off-text="全部" label="仅读取末尾 512 KiB" />
        </div>
        <span class="state badge" :class="stateLabel.tone">
          <ProgressRing v-if="state === 'connecting' || state === 'retrying'" :size="12" />
          <span v-else class="dot" :class="stateLabel.tone" />
          {{ stateLabel.text }}
        </span>
      </div>

      <div class="viewer card" :class="{ wrap }">
        <div ref="viewport" class="viewport" role="log" aria-live="off" tabindex="0" @scroll.passive="onScroll">
          <p v-if="visible.total > visible.rows.length" class="truncated t-caption t-secondary">
            仅显示最近 {{ visible.rows.length }} 条（共 {{ visible.total }} 条匹配）。下载可获取浏览器内的全部缓冲。
          </p>
          <template v-if="!sourceId">
            <div
              v-for="line in visible.rows"
              :key="line.n"
              class="line journal"
              :class="[line.tone, { open: expanded === line.n }]"
              @click="expanded = expanded === line.n ? null : line.n"
            >
              <span class="time">{{ formatTimestamp(line.time) }}</span>
              <span class="event">{{ line.event }}</span>
              <span v-if="line.event && EVENT_LABELS[line.event]" class="event-label">{{ EVENT_LABELS[line.event] }}</span>
              <span class="text">{{ line.text }}</span>
              <pre v-if="expanded === line.n" class="detail" @click.stop>{{ JSON.stringify(line.record, null, 2) }}</pre>
            </div>
          </template>
          <template v-else>
            <div v-for="line in visible.rows" :key="line.n" class="line raw" :class="line.tone">{{ line.text || ' ' }}</div>
          </template>
          <p v-if="!visible.rows.length && state !== 'connecting'" class="empty t-secondary">
            {{ query ? '没有匹配的日志' : '暂无日志' }}
          </p>
        </div>
        <button v-if="!atBottom" class="btn accent jump" @click="scrollToEnd"><FIcon name="arrow_down" />最新</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px 12px;
  padding: 8px 12px;
}
.source {
  flex: 1 1 240px;
  max-width: 360px;
}
.filter {
  flex: 1 1 180px;
  max-width: 280px;
}
.toggles {
  gap: 16px;
  flex-wrap: wrap;
}
.state {
  margin-left: auto;
}
.viewer {
  position: relative;
  overflow: hidden;
}
.viewport {
  height: calc(100dvh - 330px);
  min-height: 320px;
  overflow: auto;
  padding: 8px 0;
  font-family: var(--font-mono);
  font-size: 12.5px;
  line-height: 20px;
  background: var(--card-secondary);
}
.line {
  padding: 0 14px;
  white-space: pre;
  border-left: 3px solid transparent;
}
.wrap .line {
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
.line:hover {
  background: var(--subtle-secondary);
}
.line.critical {
  border-left-color: var(--critical);
}
.line.critical .event,
.raw.critical {
  color: var(--critical);
}
.line.caution {
  border-left-color: var(--caution);
}
.line.caution .event {
  color: var(--caution);
}
.journal {
  cursor: pointer;
}
.journal .time {
  color: var(--text-tertiary);
  margin-right: 12px;
}
.journal .event {
  font-weight: 600;
  margin-right: 8px;
}
.journal .event-label {
  margin-right: 12px;
  font-family: var(--font-text);
  font-size: 12px;
  color: var(--text-secondary);
}
.journal .text {
  color: var(--text-secondary);
}
.journal.open {
  background: var(--subtle-secondary);
}
.detail {
  margin: 6px 0 8px;
  padding: 8px 12px;
  white-space: pre-wrap;
  border-radius: var(--radius-control);
  background: var(--control-alt-secondary);
  color: var(--text-primary);
  cursor: text;
}
.truncated,
.empty {
  padding: 4px 14px 8px;
  font-family: var(--font-text);
}
.empty {
  padding: 32px;
  text-align: center;
}
.jump {
  position: absolute;
  right: 16px;
  bottom: 16px;
  box-shadow: var(--shadow-flyout);
}
@media (max-width: 640px) {
  .viewport {
    height: calc(100dvh - 380px);
    font-size: 12px;
  }
  .source,
  .filter {
    max-width: none;
  }
  .journal .time {
    display: block;
    margin: 0;
    font-size: 11px;
  }
}
</style>
