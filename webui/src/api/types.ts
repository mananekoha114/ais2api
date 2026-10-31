// Shapes returned by the ais2api management API (schemas/management.openapi.json).
// Most responses allow additional properties, so every field is optional unless
// the backend guarantees it.

export interface ApiErrorBody {
  error?: { code?: number | string; message?: string; status?: string; reason?: string; type?: string }
}

export interface Status {
  running?: boolean
  version?: string
  logging_healthy?: boolean
  pending_logs?: number
  uptime_seconds?: number
  active?: boolean
  active_generation_account_id?: string | null
  queued?: number
  connections?: number
  ready?: boolean
  verification?: string
}

export interface Cooldown {
  model: string
  until: number
  count: number
}

export interface Account {
  id: string
  name?: string
  email?: string
  auth_user?: string
  disabled?: boolean
  expired?: boolean
  failures?: Cooldown[]
  /** Stored per-account browser identity; empty timezone/locale follow the host. */
  fingerprint?: { seed: number; timezone: string; locale: string }
}

/** GET/PATCH /accounts/{id}/fingerprint and POST .../regenerate. */
export interface FingerprintView {
  seed: number
  platform: string
  /** Stored overrides; empty means "follow this computer". */
  timezone: string
  locale: string
  effective_timezone: string
  effective_locale: string
  host_timezone: string
  host_locale: string
}

export interface Rotation {
  preferred_account_id?: string | null
  next_account_id?: string | null
  active_generation_account_id?: string | null
  accounts?: Account[]
}

export interface Operation<R = Record<string, unknown>> {
  operation_id: string
  status: 'queued' | 'done' | 'failed'
  result?: R
  error?: { status?: number; code?: string; message?: string }
}

export interface ProbeResult {
  account_id: string
  status?: string
  email?: string
  ready?: boolean
  login?: boolean
  identityAmbiguous?: boolean
  regionBlocked?: boolean
  error?: { status?: number; code?: string; message?: string }
}

export interface Model {
  name?: string
  displayName?: string
  description?: string
  source?: string
  inputTokenLimit?: number
  outputTokenLimit?: number
  supportedGenerationMethods?: string[]
  [key: string]: unknown
}

export interface ModelCatalog {
  models: Model[]
  source: 'saved' | 'configured' | 'none'
  account_id: string | null
  saved_at: number | null
}

export interface StatRecord {
  account: string
  model: string
  success: number
  failure: number
  usage?: Record<string, unknown>
}
export interface UsageTotal {
  success: number
  failure: number
  priced: number
  unpriced: number
  amount_nano: number
  known_amount_nano: number
  partially_priced: number
  usage: Record<string, number>
}
export interface UsageGroup extends UsageTotal { account: string; model: string }
export interface UsageSummary {
  records: UsageGroup[]
  totals: UsageTotal
  legacy_records: StatRecord[]
  scope: 'lifetime' | 'filtered'
  since: number
  detail_since: number
}
export interface PriceQuote {
  model: string
  source: string
  fetched_at: number
  currency: 'USD'
  supported: boolean
  tiers: { above_tokens: number; input_pico: number | null; output_pico: number | null; cache_pico: number | null }[]
}
export interface RequestCost {
  status: 'priced' | 'unpriced'
  reason: string
  amount_nano: number | null
  currency: 'USD'
  quote?: Partial<PriceQuote>
  above_tokens?: number
  lines?: { kind: string; tokens: number; rate_pico: number | null; amount_nano: number }[]
}
export interface RequestEstimate extends RequestCost {
  known_amount_nano: number | null
  unpriced_attempts: number
}
export type RequestStatus = 'queued' | 'running' | 'finishing' | 'succeeded' | 'failed' | 'cancelled' | 'interrupted'
export interface RequestRecord {
  id: string
  endpoint: 'chat_completions' | 'responses' | 'gemini'
  created_at: number
  ended_at: number | null
  model: string
  requested_model: string
  account: string
  stream: boolean
  status: RequestStatus
  http_status: number | null
  generation_completed: boolean
  delivery_completed: boolean
  error: string
  finish_reason: string
  queue_ms: number | null
  first_token_ms: number | null
  generation_ms: number | null
  duration_ms: number | null
  usage: Record<string, number>
  cost: RequestEstimate
  attempt_count: number
}
export interface RequestDetail extends Omit<RequestRecord, 'attempt_count'> {
  settings: Record<string, number | boolean | string | Record<string, number | boolean | string>>
  attempts: {
    account: string; started_at: number; ended_at: number | null; completed: boolean
    finish_reason: string; error: string; usage: Record<string, number>; cost: RequestCost
  }[]
}
export interface RequestPage {
  records: RequestRecord[]
  total: number
  snapshot: number
  next_cursor: number | null
  since: number
  detail_since: number
}
export interface PriceStatus {
  currency: 'USD'
  refreshing: boolean
  error: string
  sources: { source: string; fetched_at: number; attempted_at: number; error: string; models: number }[]
}

export interface Key {
  id: string
  name?: string
  created?: number
  source?: 'config' | 'managed' | string
}

export interface KeyList {
  source?: string
  configured?: boolean
  config_key_configured?: boolean
  keys: Key[]
}

export interface KeyCreated {
  id: string
  name: string
  secret: string
  created: number
}

export interface Limits {
  request_bytes?: number
  object_bytes?: number
  relay_bytes?: number
  queue_size?: number
  queue_seconds?: number
  generation_seconds?: number
  stream_idle_seconds?: number
  max_attempts?: number
}

export interface Config {
  version?: number
  bind?: string
  port?: number
  api_key?: string
  cookie_import_key?: string
  cookie_import_max_accounts?: number
  cookie_import_timeout_seconds?: number
  data_dir?: string
  browser_executable?: string
  proxy?: string
  browser_proxy_relay?: boolean
  browser_remote_allow_origins?: boolean
  browser_site_isolation?: boolean
  headless?: boolean
  ignore_unsupported_parameters?: boolean
  search_model_drop_functions?: boolean
  browser_idle_seconds?: number
  request_retention_days?: number
  model_aliases?: Record<string, string>
  configured_models?: Array<string | Record<string, unknown>>
  model_defaults?: Record<string, unknown>
  limits?: Limits
  admin_key?: string
  admin_origins?: string[]
}

export interface ConfigView {
  config: Config
  saved_config?: Config
  config_file: string
  restart_required: boolean
}

export interface ConfigSaved {
  saved: boolean
  applied: boolean
  restart_required: boolean
}

export interface LogSource {
  id: string
  bytes: number
}

export interface JournalEntry {
  cursor: string
  record: Record<string, unknown> & { timestamp?: number; event?: string; id?: string }
}

export interface LogPage {
  entries: JournalEntry[]
  next_cursor: string
  has_more: boolean
}

export interface RawLogPage {
  source: string
  cursor?: string
  next_cursor: string
  has_more: boolean
  encoding: 'base64'
  data: string
}

export interface Capabilities {
  api_version?: string
  [key: string]: unknown
}

/** GET /diagnostics → browser: safe adapter state, never page data or cookies. */
export interface BrowserDiagnostics {
  status?: string
  phase?: string
  bridgeSource?: string
  bridgeSha256?: string
  running?: boolean
  isolatedContext?: boolean
  templateCaptured?: boolean
  modelDiscovery?: 'not_loaded' | 'live_rpc' | string
  chunkBytes?: number
  /** Last cookie write-back result: saved | unchanged | failed | not_attempted. */
  cookieRefresh?: string
  /** Identity the running browser process was launched with. */
  fingerprint?: { seed: number; platform: string; timezone: string; locale: string } | null
  /** How the browser reaches the configured proxy, and the relay's latest upstream failure. */
  proxy?: { mode: 'none' | 'direct' | 'relay' | string; relayError?: string }
}
