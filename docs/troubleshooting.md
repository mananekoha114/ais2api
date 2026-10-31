# 故障排查

出错时，HTTP 响应和命令行输出都包含一个 `reason` 字段（例如 `queue_full`），下表按 `reason` 和现象整理常见问题。更多上下文可在以下位置查看：

- 前台运行（`serve`）：终端输出。
- 后台运行（`start`）：`data/service.log`。
- WebUI 的“日志”和“诊断”页，或管理 API 的 `/logs`、`/diagnostics`。
- `ais2api doctor`：检查本地前提与路径。

日志不会记录正文、Cookie、API Key 或签名，可以放心查看；分享前仍建议检查一遍。

## 启动与安装

### 端口无法监听

`start` 超时或 `serve` 立即退出，日志中有监听错误。

- Windows 上 `os_error=10013` 通常表示端口落在系统保留范围内。用以下命令查看保留范围：

  ```text
  netsh interface ipv4 show excludedportrange protocol=tcp
  ```

  保留范围可能动态变化。在 `config.json` 中把 `port` 改为可用端口，或使用 `--port PORT`，然后重启。
- 端口被其他程序占用时同样处理。服务不会自动更换端口。
- 监听失败发生在浏览器登录之前，与导入的 Cookie 无关。

### `already_running`

服务已经在运行，而当前命令需要独占数据目录（如 `setup`、`browser update`）。先执行 `ais2api stop`。

### `not_running`、`operation_pending`

- `not_running`：`config reload`、`accounts probe` 等命令需要服务正在运行，请先启动服务；同时确认使用了与服务相同的 `--config` / `--data-dir`。
- `operation_pending`：模型发现或账号探测超过 120 秒仍未完成。用输出中的 ID 执行 `ais2api operations status ID` 继续查询。

### 程序目录不可写

程序不会改写到 AppData 或 HOME。请把程序放在当前用户可写的目录，或用 `--config` / `--data-dir` 指定其他位置。

### Linux `setup` 失败

- `setup --yes` 需要可用的 sudo（交互终端或已认证会话）。
- 只支持 Ubuntu 24.04 与 Debian 13；其他发行版或版本会被拒绝（`unsupported_distribution`），而不是猜测包名。
- WSL 2 中请把源码、程序和数据放在 Linux 文件系统内（如 `~/ais2api`），不要放在 `/mnt/c`。

### Windows 中文显示乱码

使用 `start.cmd` 启动，它会把控制台临时切换为 UTF-8。手动运行时可先执行 `chcp 65001`，或设置 `AIS2API_CONSOLE_FORMAT=json` 输出纯 JSON。

## 鉴权

| reason | 原因与处理 |
|---|---|
| `authentication`（401） | API Key 错误，或 `api_key` 为空且没有托管密钥。检查客户端填写的 Key 和 `config.json`；修改配置后需 `config reload`。 |
| `admin_authentication`（401） | 管理密码错误。 |
| `admin_disabled`（403） | `admin_key` 为空，HTTP 管理已关闭。在 `config.json` 设置后重启或 `config reload`。 |
| `admin_password_change_required`（403） | 仍在使用默认密码 `admin`。登录 WebUI 按提示修改，或调用 `POST /api/admin/v1/auth/password`。 |
| `admin_default_password_remote`（403） | 默认密码只能从本机或局域网使用。先在本机登录修改，或直接在 `config.json` 设置强 `admin_key`。 |
| `admin_origin`（403） | 跨域访问管理 API 的来源未列入 `admin_origins`。 |
| `cookie_import_disabled` / `cookie_import_authentication` | 未设置 `cookie_import_key`，或脚本中填写的导入密钥不一致。 |

## 账号与 Cookie

| reason | 原因与处理 |
|---|---|
| `no_account` / `no_available_account`（503） | 没有导入账号，或所有账号都处于冷却、禁用或过期状态。在 WebUI“账号”页查看冷却原因，必要时清除冷却或重新导入。 |
| `session_expired`（401） | Cookie 已失效，页面跳转到登录页。在浏览器重新登录 AI Studio 后重新导入。 |
| `missing_cookies`（401） | 账号没有可用 Cookie。重新导入。 |
| `identity_mismatch`（401） | Cookie 对应的 Google 账号与记录不一致，账号会进入 5 分钟冷却。检查是否导错账号或账号索引（`authuser`）。 |
| `cookie_not_authenticated`（400） | 导入的 Cookie 未登录。确认在已登录的 AI Studio 页面导出。 |
| `identity_unverified`（409） | 页面上无法确认唯一的 Google 身份（例如同时出现多个账号），或与填写的期望邮箱不符。检查账号索引和期望邮箱。 |
| `region_blocked`（403） | AI Studio 在浏览器所在地区不可用。使用可用地区的代理。 |

账号进入冷却的常见原因：

- **配额**（429）：按模型冷却。分钟级限额至少等待 60 秒；其他限额依次等待 60/60/300/300/600 秒，上游返回 `Retry-After` 时可延长（最长 1 天），每日计数在太平洋时间午夜重置。
- **权限**（403）：会先在原账号上重建页面会话重试一次，仍失败才让该模型冷却 5 分钟。
- **鉴权**（401 或身份未确认）：先重建会话重试，仍失败则整个账号冷却 5 分钟；所有账号都处于鉴权冷却时，会放行一个账号尝试一次以便自动恢复。

导入新凭据不会清除已有的配额冷却。详细的重试规则见 [浏览器网关](browser.md#失败重试与冷却)。

## 生成请求

| reason | 原因与处理 |
|---|---|
| `queue_full`（429） | 排队请求超过 `limits.queue_size`（默认 8）。同一时间只处理一个生成请求，请降低并发或调大队列。 |
| `queue_timeout`（504） | 排队超过 `limits.queue_seconds`（默认 30 秒）。 |
| `generation_timeout` / `stream_idle_timeout`（504） | 生成总时长或流式数据间隔超过限额（默认均为 240 秒），可调整 `limits.generation_seconds`、`limits.stream_idle_seconds`（需重启）。 |
| `invalid_argument`（400） | 请求包含不支持或冲突的参数。错误信息会指出字段名；可开启 [忽略不支持的参数](clients.md#兼容开关忽略不支持的参数)。 |
| `model_not_found`（404） | 模型 id 不存在。以 `/v1/models` 返回为准。 |
| `request_limit` / `media_limit` / `object_limit`（413） | 请求体、媒体或响应超出 `limits` 中的限额。 |
| `browser_incompatible`（502） | AI Studio 页面结构变化，桥接脚本无法适配。服务会自动重启浏览器重试一次；持续出现说明需要更新程序。 |
| `browser_disconnected` / `browser_timeout` | 浏览器崩溃或无响应。服务会自动重启浏览器重试一次。 |
| `proxy_auth_unsupported`（503） | 配置了带用户名密码的代理，但未开启 `browser_proxy_relay`。见 [配置参考](configuration.md#使用带账号密码的代理)。 |
| `proxy_failed`（502） | 代理连接失败。诊断中的 `proxy.relayError` 会给出转发代理的具体原因（不含凭据）。不会让账号进入冷却。 |
| `network_failed`（502） | 无法连接 Google。检查网络或代理；会按临时错误在原账号重试，不会冷却账号。 |
| `empty_upstream` / `incomplete_upstream` / `upstream_no_content`（502） | 上游返回空内容或数据不完整。服务会在原账号短暂等待后重试；已经输出给客户端的内容不会重新生成。 |
| `cancelled`（499） | 客户端断开连接，生成已取消。 |

## 客户端相关

- **Cherry Studio 提示 schema 不支持**：确认使用最新版本；strict schema 默认被拒绝，可关闭客户端的 strict 选项或开启“忽略不支持的参数”。
- **SillyTavern 404**：反向代理地址填 `http://HOST:18451` 或 `http://HOST:18451/v1beta`，不要填 `/v1`。
- **手机连不上**：确认 `bind` 不是 `127.0.0.1`、防火墙放行端口，且客户端使用的是电脑的局域网地址。
- **thinkingBudget 无效**：该参数被接受但不生效（Gemini 4 上的 `0` 映射为 `LOW`），请改用 `thinkingLevel`。
- **Gemini 4 长回答被截断**：AI Studio 上游每次最多输出 32,768 token（含思考），没有续写机制，网关会把输出上限压到 32768，到达上限时上游返回的 `Internal error encountered.` 会转成 `MAX_TOKENS` 结束（日志 `output_limit_reached`）。可降低思考等级给可见输出留出更多空间，或把任务拆成多次请求。
- **原生 Gemini 接口看不到思考**：与官方一致，需要在 `generationConfig.thinkingConfig` 中设置 `includeThoughts: true`，或在 `model_defaults` 中为该模型配置。
- **搜索没有返回来源**：目前不解析搜索来源元数据，这是已知限制。

## 仍无法解决

在 WebUI“诊断”页导出脱敏诊断快照（或调用 `POST /api/admin/v1/diagnostics/export`），连同 `data/service.log` 中相关时间段的记录一起提交 Issue。提交前请再次确认其中没有 Cookie、密钥或个人信息。
