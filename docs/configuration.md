# 配置参考

本文列出 `config.json` 的全部字段、对应的环境变量与命令行参数，以及哪些修改可以热更新、哪些需要重启。机器可读的定义见 [schemas/config.schema.json](../schemas/config.schema.json)。

## 配置文件的位置与优先级

- 默认配置文件是可执行文件旁的 `config.json`，数据目录是旁边的 `data/`。不会使用 AppData、HOME 或 XDG 目录；程序目录不可写时直接报错，而不是改写到用户目录。
- 生效配置按以下顺序叠加，后者覆盖前者：**默认值 → 配置文件 → `AIS2API_*` 环境变量 → 命令行参数**。
- 环境变量与命令行参数只影响本次运行，不会写回配置文件。
- `--config PATH` 与 `--data-dir PATH` 可以指定完全隔离的一套安装；对同一个服务执行的所有命令都要使用相同的 `--data-dir`。

### 路径规则

- 配置文件内的相对路径以**配置文件所在目录**为基准；命令行和环境变量中的相对路径以**当前工作目录**为基准。
- 程序自己写入的路径尽量保存为相对路径，因此整个程序目录可以整体移动。已有的外部绝对路径保持不变，也不会自动迁移旧位置的数据。
- `ais2api doctor` 会列出实际使用的程序、配置、数据、下载与日志路径。

### 自动升级与格式

加载已有配置时，程序会补齐缺失的字段（包括 `limits` 内的子项），并统一为两空格缩进、每个字段一行的格式；已有的值与相对路径保持不变。配置无效或版本不受支持时拒绝启动，并且不改写文件。格式已经是最新时不会重写。配置版本号目前固定为 `1`。

## 字段一览

“生效方式”一列：**热更新** 表示保存（管理 API / WebUI）或执行 `ais2api config reload` 后立即生效；**重启** 表示需要重启服务。

### 网络与鉴权

| 字段 | 类型与默认值 | 生效方式 | 说明 |
|---|---|---|---|
| `bind` | 字符串，`"127.0.0.1"` | 重启 | 监听地址。需要局域网访问时改为 `0.0.0.0` 或具体网卡地址。 |
| `port` | 1–65535，`18451` | 重启 | 监听端口。 |
| `api_key` | 字符串，`""` | 热更新 | 生成与模型接口的密钥。为空且没有通过管理 API 创建的密钥时，受保护接口全部拒绝。最长 4096 个可见 ASCII 字符，不含空白。 |
| `admin_key` | 字符串，新配置为 `"admin"` | 热更新 | 管理 API 与 WebUI 的密码，与 `api_key` 相互独立。为空表示关闭 HTTP 管理。默认值 `admin` 有使用限制，见 [管理 API](management-api.md#鉴权)。 |
| `admin_origins` | 字符串数组，`[]` | 热更新 | 允许跨域访问管理 API 的完整来源，例如 `http://localhost:5173`。为空时只允许同源。不能包含路径、凭据或通配符。 |
| `cookie_import_key` | 字符串，`""` | 热更新 | userscript Cookie 导入使用的独立密钥；为空表示关闭导入。它不能用于生成接口。 |
| `cookie_import_max_accounts` | 1–32，`8` | 热更新 | 原版脚本未指定账号索引时，最多依次探测多少个账号。单个索引未登录或返回 401 时继续探测，连续三次失败后停止。 |
| `cookie_import_timeout_seconds` | 1–240，`180` | 热更新 | 单次 Cookie 导入验证的超时时间（秒）。 |

### 存储、浏览器与代理

| 字段 | 类型与默认值 | 生效方式 | 说明 |
|---|---|---|---|
| `data_dir` | 字符串，程序旁 `data/` | 重启 | 数据库、日志、下载与浏览器临时资料所在目录。 |
| `browser_executable` | 字符串，`""` | 重启 | 浏览器路径。留空时自动使用程序旁 `browser-runtime/` 中的锁定浏览器，或 `setup` 安装的浏览器。 |
| `headless` | 布尔，`true` | 重启 | 是否以无界面模式运行浏览器。 |
| `proxy` | 字符串，`""` | 重启 | 上游代理，同时用于浏览器和服务端下载。 |
| `browser_proxy_relay` | 布尔，`false` | 重启 | 开启本机转发代理，让浏览器能使用带用户名密码的 HTTP 或 SOCKS5 代理，详见下文。 |
| `browser_remote_allow_origins` | 布尔，`false` | 重启 | 以 `--remote-allow-origins=*` 启动浏览器，允许任意来源的网页连接本机 DevTools WebSocket。ais2api 自身不需要；仅供外部 CDP 工具使用，开启后本机浏览器中打开的网页也能尝试连接该端口。 |
| `browser_site_isolation` | 布尔，`true` | 重启 | Chromium 站点隔离。设为 `false` 可在小内存主机上节省渲染进程内存，代价是不同站点不再分进程隔离。 |
| `browser_idle_seconds` | `0` 或 10–86400，`0` | 热更新 | 连续空闲多少秒后关闭浏览器以释放内存；`0` 表示不关闭。 |
| `request_retention_days` | 1–3650，`90` | 热更新 | 已结束请求明细的保存天数；后台分批清理，累计用量和费用继续保留。 |

### 生成行为

| 字段 | 类型与默认值 | 生效方式 | 说明 |
|---|---|---|---|
| `ignore_unsupported_parameters` | 布尔，`false` | 热更新 | 舍弃已知不支持的可选参数后继续生成，而不是报错。舍弃清单见 [协议合同](protocol.md#optional-unsupported-parameter-dropping)。 |
| `search_model_drop_functions` | 布尔，`true` | 热更新 | `*-search` 模型同时收到自定义函数时：`true` 舍弃函数、保留搜索；`false` 保留函数、不加搜索。见 [搜索模型](protocol.md#search-models)。 |
| `model_aliases` | 对象，`{}` | 热更新 | 模型别名，键为客户端使用的名称，值为实际模型 id，例如 `{"flash": "gemini-3.1-flash-lite"}`。 |
| `model_defaults` | 对象，`{}` | 热更新 | 按模型 id 设置默认请求字段（Gemini 请求格式），客户端显式传入的字段优先。例如 `{"gemini-3.1-flash-lite": {"generationConfig": {"temperature": 0.7}}}`。 |
| `configured_models` | 数组，`[]` | 热更新 | 手动声明的备用模型（字符串 id 或模型对象）。当前账号没有保存的发现结果时作为 `source: "configured"` 返回，不代表真实可用。实时发现失败会报错并保留旧清单，不会把备用模型保存为发现结果。 |

### 资源限额 `limits`

| 字段 | 默认值 | 生效方式 | 说明 |
|---|---|---|---|
| `request_bytes` | 33554432（32 MiB） | 热更新 | 单个 HTTP 请求体及媒体展开后的累计上限。 |
| `object_bytes` | 67108864（64 MiB） | 重启 | 非流式响应、内联媒体等单个对象的聚合上限。 |
| `relay_bytes` | 1048576（1 MiB） | 重启 | 浏览器到服务的转发窗口，不能大于 `object_bytes`。 |
| `queue_size` | 8（1–4096） | 热更新 | 等待中的生成请求数上限，超出返回 `429 queue_full`。 |
| `queue_seconds` | 30 | 热更新 | 请求最多排队多少秒，超时返回 `504 queue_timeout`。 |
| `generation_seconds` | 240 | 重启 | 非流式生成的总时限。 |
| `stream_idle_seconds` | 240 | 重启 | 流式生成两次收到数据之间的最长间隔；只要数据持续到达，流式请求可以超过 `generation_seconds`。 |
| `max_attempts` | 3（1–16） | 热更新 | 单个请求在没有输出任何内容前的最大尝试次数。 |

## 环境变量与命令行参数

| 字段 | 环境变量 | 命令行参数 |
|---|---|---|
| 配置文件 | — | `--config PATH` |
| `bind` | `AIS2API_BIND` | `--bind ADDRESS` |
| `port` | `AIS2API_PORT` | `--port PORT` |
| `admin_key` | `AIS2API_ADMIN_KEY` | — |
| `admin_origins` | `AIS2API_ADMIN_ORIGINS`（JSON 数组） | — |
| `data_dir` | `AIS2API_DATA_DIR` | `--data-dir PATH` |
| `browser_executable` | `AIS2API_BROWSER_EXECUTABLE` | `--browser PATH` |
| `headless` | `AIS2API_HEADLESS` | `--headless=true\|false` |
| `proxy` | `AIS2API_PROXY` | `--proxy URL` |
| `browser_proxy_relay` | `AIS2API_BROWSER_PROXY_RELAY` | `--browser-proxy-relay=true\|false` |
| `browser_remote_allow_origins` | `AIS2API_BROWSER_REMOTE_ALLOW_ORIGINS` | — |
| `browser_site_isolation` | `AIS2API_BROWSER_SITE_ISOLATION` | — |
| `browser_idle_seconds` | `AIS2API_BROWSER_IDLE_SECONDS` | `--browser-idle-seconds N` |
| `request_retention_days` | `AIS2API_REQUEST_RETENTION_DAYS` | — |
| `ignore_unsupported_parameters` | `AIS2API_IGNORE_UNSUPPORTED_PARAMETERS` | — |
| `search_model_drop_functions` | `AIS2API_SEARCH_MODEL_DROP_FUNCTIONS` | — |
| `model_aliases` | `AIS2API_MODEL_ALIASES`（JSON） | — |
| `model_defaults` | `AIS2API_MODEL_DEFAULTS`（JSON） | — |
| `configured_models` | `AIS2API_CONFIGURED_MODELS`（JSON） | — |
| `limits` | `AIS2API_LIMITS`（JSON 对象） | — |
| `limits.*` 各项 | `AIS2API_REQUEST_BYTES`、`AIS2API_OBJECT_BYTES`、`AIS2API_RELAY_BYTES`、`AIS2API_QUEUE_SIZE`、`AIS2API_QUEUE_SECONDS`、`AIS2API_GENERATION_SECONDS`、`AIS2API_STREAM_IDLE_SECONDS`、`AIS2API_MAX_ATTEMPTS` | `--request-bytes`、`--object-bytes`、`--relay-bytes`、`--queue-size`、`--queue-seconds`、`--generation-seconds`、`--stream-idle-seconds`、`--max-attempts` |

说明：

- 布尔与数字类环境变量按 JSON 解析，例如 `AIS2API_HEADLESS=false`、`AIS2API_PORT=18452`。
- `api_key` 与 `cookie_import_key` 没有对应的环境变量或命令行参数，只能写在配置文件中（或通过管理 API / WebUI 修改）。
- 控制台输出格式另由 `AIS2API_CONSOLE_FORMAT`（`auto` / `text` / `json`）和 `NO_COLOR` 控制，见 [命令行](cli.md#输出格式)。

## 热更新与重启

- **需要重启**的字段：`bind`、`port`、`data_dir`、`browser_executable`、`proxy`、`browser_proxy_relay`、`browser_remote_allow_origins`、`browser_site_isolation`、`headless`、`limits.generation_seconds`、`limits.stream_idle_seconds`、`limits.object_bytes`、`limits.relay_bytes`。
- 其余字段都可热更新。热更新前会校验完整配置；校验失败时保留原配置。
- `ais2api config reload` 重新读取配置文件和环境变量。存在需要重启的差异时返回 `restart_required`，不会只应用其中一部分。
- 通过管理 API 或 WebUI 保存时，如果修改中包含需要重启的字段，整份配置会作为“待重启配置”保存到磁盘，当前进程继续使用原配置；之后的修改以待重启配置为基础叠加。详见 [管理 API](management-api.md#配置保存与重启)。

## 常用场景

### 设置 API Key

```json
{
  "api_key": "换成你自己的长随机字符串"
}
```

保存后执行 `ais2api config reload`（或重启）。新的 Key 立即生效，旧 Key 立即失效；清空表示撤销。`init` 不会生成 Key，重复执行 `init` 也不会覆盖已填写的 Key。

### 让局域网或手机访问

把 `bind` 改为 `0.0.0.0` 并重启，客户端使用本机的局域网地址。请务必设置强 `api_key`，并修改默认管理密码。

### 使用带账号密码的代理

Chromium 不能直接使用带用户名密码的代理，也不支持 SOCKS 认证。开启本机转发代理：

```json
{
  "proxy": "socks5://user:password@proxy.example.com:1080",
  "browser_proxy_relay": true
}
```

重启后，浏览器只连接 ais2api 在 `127.0.0.1` 随机端口开启的转发代理，由 ais2api 向上游代理认证；凭据不会出现在浏览器命令行中。规则如下：

- 开启后 `proxy` 必须是单个 `http://[user:password@]host:port` 或 `socks5://[user:password@]host:port`（`socks5h://` 等同，目标域名始终由上游代理解析），否则配置被拒绝。
- 转发代理只允许 `CONNECT` 到 443 端口。
- 未开启时配置了带凭据的代理，浏览器启动前即以 `503 proxy_auth_unsupported` 失败。
- 服务端下载仍由 libcurl 直接使用 `proxy`。

### 低内存设备

```json
{
  "browser_idle_seconds": 900
}
```

连续 900 秒没有任何任务（生成、探测、模型发现）时，先写回 Cookie 再关闭浏览器。之后的第一个请求需要重新启动浏览器并完成页面引导，会明显变慢。

### 模型别名与默认参数

```json
{
  "model_aliases": { "fast": "gemini-3.1-flash-lite" },
  "model_defaults": {
    "gemini-3.1-flash-lite": {
      "generationConfig": { "temperature": 0.5, "maxOutputTokens": 4096 }
    }
  }
}
```

通过管理 API 修改时，`model_aliases`、`model_defaults`、`configured_models` 与 `admin_origins` 会整体替换；要删除某个别名，提交不含它的完整对象即可。`limits` 则按子项合并。

## 安全提示

- `config.json` 以明文保存 `api_key`、`admin_key`、`cookie_import_key` 与代理凭据。`config show`、管理 API 与诊断输出会脱敏，但文件本身应按凭据保护。
- Cookie 保存在权限受限的 SQLite 数据库 `data/state.sqlite3` 中，没有应用层加密。备份 `data/` 时同样应视为凭据。
