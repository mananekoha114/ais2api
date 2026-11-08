<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import FIcon from '../components/FIcon.vue'
import FInfoBar from '../components/FInfoBar.vue'
import PageHeader from '../components/PageHeader.vue'
import ProgressRing from '../components/ProgressRing.vue'
import { api, describeError } from '../api/client'
import type { BackendEvents, BrowserDiagnostics, Capabilities, ConfigView, LogSource, Status } from '../api/types'
import { refreshStatus } from '../stores/service'
import { confirm, copyText, downloadFile, notify, notifyError } from '../stores/ui'
import { browserPhase, browserStatus, cookieRefreshLabel, modelDiscoveryLabel, proxyModeLabel } from '../utils/browser'
import { formatBytes, formatDuration, formatTimestamp } from '../utils/format'

interface Diagnostics {
  version?: string
  build_mode?: 'release' | 'dev'
  status?: Status
  config?: ConfigView
  stats?: { records?: unknown[] }
  logs?: { sources?: LogSource[] }
  browser?: BrowserDiagnostics
  events?: BackendEvents
  google_verified?: boolean
}

const backendNames: Record<string, string> = { cloakbrowser: 'CloakBrowser', camoufox: 'Camoufox', spidermonkey: 'SpiderMonkey', custom: '其他' }
const eventNames: Record<string, { label: string; hint: string }> = {
  permission_denied: { label: '403（Code 7）', hint: '上游以 permission_denied 拒绝了生成，包括随后被就地重建救回的' },
  login_redirect: { label: '跳转登录页', hint: '页面被重定向到 Google 登录，Cookie 可能已失效' },
  auth_cooldown: { label: '认证冷却', hint: '账号因认证失败进入冷却' },
  permission_cooldown: { label: '403 冷却', hint: '账号因重复的 403 进入冷却' },
  login_expired: { label: '登录失效', hint: '账号被标记为需要重新登录' },
  terms_required: { label: '需要同意条款', hint: 'AI Studio 要求账号同意条款；ais2api 不会代为同意' },
  session_rebuild: { label: '就地重建会话', hint: '失败后在同一浏览器内重建页面会话' },
  browser_restart: { label: '重启浏览器', hint: '失败后重启了整个浏览器进程' },
}

const data = ref<Diagnostics | null>(null)
const capabilities = ref<Capabilities | null>(null)
const loading = ref(true)
const loadError = ref('')
const exporting = ref(false)
const showRaw = ref(false)
const stopping = ref(false)

async function load() {
  loading.value = true
  try {
    const [d, c] = await Promise.all([api.get<Diagnostics>('/diagnostics'), api.get<Capabilities>('/capabilities').catch(() => null)])
    data.value = d
    capabilities.value = c
    loadError.value = ''
  } catch (e) {
    loadError.value = describeError(e)
  } finally {
    loading.value = false
  }
}
onMounted(load)

const json = computed(() => JSON.stringify(data.value, null, 2))
const sources = computed(() => data.value?.logs?.sources ?? [])
const browser = computed(() => data.value?.browser)
const browserState = computed(() => browserStatus(browser.value?.status))
const yesNo = (value: boolean | undefined, yes: string, no: string) => (value === undefined ? '—' : value ? yes : no)
const logBytes = computed(() => sources.value.reduce((sum, s) => sum + s.bytes, 0))
const eventBackends = computed(() => Object.keys(data.value?.events?.backends ?? {}).sort())
const eventRows = computed(() => {
  const backends = data.value?.events?.backends ?? {}
  const seen = new Set(Object.values(backends).flatMap(events => Object.keys(events)))
  const known = Object.keys(eventNames).filter(event => seen.has(event))
  const other = [...seen].filter(event => !(event in eventNames)).sort()
  return [...known, ...other].map(event => ({
    event,
    label: eventNames[event]?.label ?? event,
    hint: eventNames[event]?.hint ?? '',
    cells: eventBackends.value.map(backend => backends[backend]?.[event]),
  }))
})

async function exportSnapshot() {
  exporting.value = true
  try {
    const snapshot = await api.post<unknown>('/diagnostics/export')
    downloadFile(`ais2api-diagnostics-${new Date().toISOString().replace(/[:.]/g, '-')}.json`, JSON.stringify(snapshot, null, 2))
    notify('success', '诊断报告已导出', '只含安全字段：不含 Cookie、邮箱、账号 ID、请求内容和本机路径。')
  } catch (e) {
    notifyError('导出失败', e)
  } finally {
    exporting.value = false
  }
}

async function stopService() {
  const ok = await confirm({
    title: '停止服务',
    message: '将停止 ais2api 服务进程，所有进行中的请求会被取消。停止后需要在服务器上手动运行 ais2api start 重新启动，本控制台无法远程启动。',
    confirmText: '停止服务',
    danger: true,
    typeToConfirm: 'STOP',
  })
  if (!ok) return
  stopping.value = true
  try {
    await api.post('/service/stop')
    notify('warning', '服务正在停止', '状态指示会在数秒内变为“无法连接”。')
    setTimeout(refreshStatus, 2000)
  } catch (e) {
    notifyError('停止失败', e)
  } finally {
    stopping.value = false
  }
}

const capabilityList = computed(() =>
  Object.entries(capabilities.value ?? {})
    .filter(([key]) => key !== 'api_version')
    .map(([key, value]) => ({ key, value: typeof value === 'object' ? JSON.stringify(value) : String(value) })),
)
</script>

<template>
  <div>
    <PageHeader title="诊断" subtitle="运行状态、按后端统计与日志清单。诊断与就绪检查都不代表 Google 上游生成已验证。">
      <button class="btn" :disabled="loading" @click="load"><FIcon name="arrow_clockwise" />刷新</button>
      <button class="btn accent" :disabled="exporting" @click="exportSnapshot">
        <ProgressRing v-if="exporting" class="on-accent" /><FIcon v-else name="arrow_download" />导出报告
      </button>
    </PageHeader>

    <div class="stack" style="--gap: 12px">
      <FInfoBar v-if="loadError" severity="error" title="加载失败" :message="loadError" />

      <template v-if="data">
        <FInfoBar
          v-if="data.google_verified === false"
          severity="info"
          message="google_verified = false：本地检查通过不等于上游可用，真实可用性需通过账号探测和实际生成确认。"
        />

        <section class="settings-group">
          <h3>运行时</h3>
          <div class="card info-grid">
            <div><span class="t-caption t-secondary">版本</span><strong>{{ data.version ?? '—' }}</strong></div>
            <div>
              <span class="t-caption t-secondary">构建</span>
              <strong>{{ data.build_mode === 'dev' ? '开发版' : data.build_mode === 'release' ? '正式版' : '—' }}</strong>
            </div>
            <div><span class="t-caption t-secondary">管理 API</span><strong>{{ capabilities?.api_version ?? 'v1' }}</strong></div>
            <div><span class="t-caption t-secondary">运行时间</span><strong>{{ formatDuration(data.status?.uptime_seconds) }}</strong></div>
            <div>
              <span class="t-caption t-secondary">本地就绪</span>
              <strong class="row" style="--gap: 6px">
                <span class="dot" :class="data.status?.ready ? 'success' : 'caution'" />{{ data.status?.ready ? '是' : '否' }}
              </strong>
            </div>
            <div>
              <span class="t-caption t-secondary">日志写入</span>
              <strong class="row" style="--gap: 6px">
                <span class="dot" :class="data.status?.logging_healthy ? 'success' : 'critical'" />
                {{ data.status?.logging_healthy ? '正常' : `积压 ${data.status?.pending_logs ?? 0}` }}
              </strong>
            </div>
            <div><span class="t-caption t-secondary">统计记录</span><strong>{{ data.stats?.records?.length ?? 0 }} 条</strong></div>
            <div class="wide">
              <span class="t-caption t-secondary">配置文件</span>
              <button class="path mono" title="复制路径" @click="copyText(data.config?.config_file ?? '')">
                <span class="break">{{ data.config?.config_file ?? '—' }}</span><FIcon name="copy" :size="14" />
              </button>
            </div>
          </div>
        </section>

        <section v-if="browser" class="settings-group">
          <h3>浏览器适配器</h3>
          <FInfoBar
            v-if="browserState.tone === 'critical'"
            severity="error"
            title="页面适配失败"
            message="AI Studio 页面结构可能已变化，生成请求会以 browser_incompatible 失败。请查看日志中的 browser_failure 记录。"
          />
          <div class="card info-grid">
            <div>
              <span class="t-caption t-secondary">状态</span>
              <strong class="row" style="--gap: 6px">
                <span class="dot" :class="browserState.tone !== 'neutral' ? browserState.tone : ''" />{{ browserState.text }}
              </strong>
            </div>
            <div><span class="t-caption t-secondary">当前阶段</span><strong>{{ browserPhase(browser.phase) }}</strong></div>
            <div><span class="t-caption t-secondary">浏览器进程</span><strong>{{ yesNo(browser.running, '运行中', '未运行') }}</strong></div>
            <div><span class="t-caption t-secondary">隔离上下文</span><strong>{{ yesNo(browser.isolatedContext, '已创建', '未创建') }}</strong></div>
            <div><span class="t-caption t-secondary">请求模板</span><strong>{{ yesNo(browser.templateCaptured, '已捕获', '未捕获') }}</strong></div>
            <div><span class="t-caption t-secondary">模型发现</span><strong>{{ modelDiscoveryLabel(browser.modelDiscovery) }}</strong></div>
            <div v-if="browser.chunkBytes"><span class="t-caption t-secondary">中继分块</span><strong>{{ formatBytes(browser.chunkBytes) }}</strong></div>
            <div v-if="browser.fingerprint" class="wide">
              <span class="t-caption t-secondary">当前浏览器指纹</span>
              <strong class="mono">
                #{{ browser.fingerprint.seed || '—' }} · {{ browser.fingerprint.platform }} · {{ browser.fingerprint.timezone }} ·
                {{ browser.fingerprint.locale }}
              </strong>
            </div>
            <div v-if="browser.proxy">
              <span class="t-caption t-secondary">浏览器代理</span>
              <strong>{{ proxyModeLabel(browser.proxy.mode) }}</strong>
            </div>
            <div v-if="browser.proxy?.relayError" class="wide">
              <span class="t-caption t-secondary">转发代理最近的错误</span>
              <strong class="t-critical">{{ browser.proxy.relayError }}</strong>
            </div>
            <div v-if="browser.cookieRefresh">
              <span class="t-caption t-secondary" title="会话中被 Google 轮换的 Cookie 会写回账号，延长凭据有效期">Cookie 写回</span>
              <strong>{{ cookieRefreshLabel(browser.cookieRefresh) }}</strong>
            </div>
            <div class="wide">
              <span class="t-caption t-secondary">桥接脚本（{{ browser.bridgeSource === 'embedded' ? '内嵌' : browser.bridgeSource ?? '—' }}）SHA-256</span>
              <button v-if="browser.bridgeSha256" class="path mono" title="复制摘要" @click="copyText(browser.bridgeSha256, '摘要已复制')">
                <span class="break">{{ browser.bridgeSha256 }}</span><FIcon name="copy" :size="14" />
              </button>
              <strong v-else>—</strong>
            </div>
          </div>
          <p class="t-caption t-tertiary">
            桥接脚本内嵌在可执行文件中，旁边的 browser/bridge.js 仅作参考，不会覆盖内嵌版本。状态在浏览器忙碌时可能只显示“处理中”。
          </p>
        </section>

        <section class="settings-group">
          <h3>按后端统计</h3>
          <div class="card">
            <table v-if="eventRows.length" class="table">
              <thead>
                <tr>
                  <th>事件</th>
                  <th v-for="b in eventBackends" :key="b" class="num">{{ backendNames[b] ?? b }}</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="row in eventRows" :key="row.event">
                  <td :title="row.hint">{{ row.label }}</td>
                  <td v-for="(cell, i) in row.cells" :key="eventBackends[i]" class="num">
                    <template v-if="cell">
                      <strong :title="`最近一次：${formatTimestamp(cell.last_at)}`">{{ cell.total }}</strong>
                      <div class="t-caption t-secondary">近 7 天 {{ cell.last_7_days }} · 今天 {{ cell.today }}</div>
                    </template>
                    <span v-else class="t-tertiary">0</span>
                  </td>
                </tr>
              </tbody>
            </table>
            <div v-else class="pad t-secondary">还没有记录到任何事件。</div>
          </div>
          <p class="t-caption t-tertiary">
            <template v-if="data.events?.since">自 {{ formatTimestamp(data.events.since) }} 起累计，</template>“近 7 天”与“今天”按 UTC 日期计算。重置统计时一并清零。
          </p>
        </section>

        <section class="settings-group">
          <h3>日志文件 · 共 {{ formatBytes(logBytes) }}</h3>
          <div class="card">
            <table class="table">
              <thead>
                <tr>
                  <th>日志源</th>
                  <th class="num">大小</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="s in sources" :key="s.id">
                  <td>
                    <RouterLink :to="{ path: '/logs' }" class="mono">{{ s.id }}</RouterLink>
                  </td>
                  <td class="num">{{ formatBytes(s.bytes) }}</td>
                </tr>
                <tr v-if="!sources.length">
                  <td colspan="2" class="t-secondary">没有日志文件</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>

        <section v-if="capabilityList.length" class="settings-group">
          <h3>能力</h3>
          <div class="card pad row wrap caps">
            <span v-for="c in capabilityList" :key="c.key" class="badge outline" :title="c.value">
              {{ c.key }}<template v-if="c.value !== 'true'">: {{ c.value.length > 32 ? '…' : c.value }}</template>
            </span>
          </div>
        </section>

        <section class="settings-group">
          <h3>原始快照</h3>
          <div class="card">
            <button class="expander" :aria-expanded="showRaw" @click="showRaw = !showRaw">
              <FIcon name="code" :size="20" />
              <span class="grow">查看 JSON</span>
              <FIcon :name="showRaw ? 'chevron_down' : 'chevron_right'" />
            </button>
            <div v-if="showRaw" class="raw">
              <div class="row" style="justify-content: flex-end; padding: 0 0 8px">
                <button class="btn subtle" @click="copyText(json)"><FIcon name="copy" />复制</button>
              </div>
              <pre class="code-block">{{ json }}</pre>
            </div>
          </div>
        </section>
      </template>
      <div v-else-if="loading" class="stack" style="--gap: 8px">
        <div class="skeleton" style="height: 160px; border-radius: 8px" />
        <div class="skeleton" style="height: 120px; border-radius: 8px" />
      </div>

      <section class="settings-group">
        <h3>危险操作</h3>
        <div class="setting danger-zone">
          <FIcon name="power" :size="20" />
          <div class="setting-text">
            <div>停止服务</div>
            <div class="desc">远程停止 ais2api 进程。停止后无法从本控制台重新启动。</div>
          </div>
          <div class="setting-control">
            <button class="btn danger" :disabled="stopping" @click="stopService"><ProgressRing v-if="stopping" />停止服务</button>
          </div>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.info-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 16px 24px;
  padding: 16px;
}
.info-grid > div {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}
.info-grid .wide {
  grid-column: 1 / -1;
}
.path {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 2px 6px;
  margin-left: -6px;
  border: 0;
  border-radius: var(--radius-control);
  background: transparent;
  color: var(--text-primary);
  text-align: left;
  cursor: pointer;
}
.path:hover {
  background: var(--subtle-secondary);
}
.caps {
  gap: 8px;
}
.expander {
  display: flex;
  align-items: center;
  gap: 16px;
  width: 100%;
  min-height: 56px;
  padding: 12px 16px;
  border: 0;
  background: transparent;
  color: var(--text-primary);
  text-align: left;
  cursor: pointer;
  border-radius: var(--radius-overlay);
}
.expander:hover {
  background: var(--subtle-tertiary);
}
.raw {
  padding: 0 16px 16px;
}
.raw .code-block {
  max-height: 480px;
}
.danger-zone {
  border-color: color-mix(in srgb, var(--critical) 35%, transparent);
}
.danger-zone > .icon {
  color: var(--critical);
}
.on-accent {
  color: var(--text-on-accent);
}
@media (max-width: 640px) {
  .danger-zone {
    flex-wrap: wrap;
  }
  .danger-zone .setting-text {
    flex-basis: calc(100% - 40px);
  }
  .danger-zone .setting-control {
    flex-basis: 100%;
  }
  .danger-zone .btn {
    width: 100%;
  }
}
</style>
