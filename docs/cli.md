# 命令行

可执行文件为 `ais2api`（Windows 为 `ais2api.exe`）。除命令行外，服务还可以通过独立鉴权的 [管理 API](management-api.md) 和启用 WebUI 构建中的 [网页控制台](webui.md) 管理。

## 命令速查

```text
ais2api init | serve | start | stop | status | setup | doctor
ais2api browser update | browser rollback
ais2api accounts list
ais2api accounts import (--file PATH | --stdin) [--name NAME]
ais2api accounts select ID | remove ID | reset ID | probe (ID | all)
ais2api keys list
ais2api stats
ais2api models list
ais2api config show | config validate | config reload
ais2api operations status ID
ais2api --help
```

## 输出格式

- 成功时向 stdout 输出一个 JSON 对象，退出码 0；失败时向 stderr 输出结构化 JSON 错误，退出码 1。`--help` 输出普通文本。
- 在交互式控制台中，输出改为便于阅读的形式：命令结果为缩进 JSON；错误为一行文字（`错误：message（reason，HTTP status）`）；`serve` 与 `setup` 的日志为带本地时间、级别（信息/提示/警告/错误）、事件和关键字段的中文单行，例如：

  ```text
  10-24 22:35:43 信息 生成完成  chatcmpl-1cdf…  gemini-3.1-flash-lite  耗时 2.4s
  ```

  成功的 HTTP 请求不在控制台显示，失败的才显示。
- 输出被重定向到文件或管道时（包括 `start` 写入的 `service.log`），命令结果仍为单行 JSON，日志仍为每行一条 JSON 记录，便于脚本处理。
- `AIS2API_CONSOLE_FORMAT=json` 或 `text` 可强制指定格式（默认 `auto`）；设置 `NO_COLOR` 关闭颜色。管理 API 的日志接口不受影响。
- 所有输出都会隐藏密钥。

## 初始化与服务生命周期

```text
ais2api init      创建配置和数据库
ais2api setup     准备浏览器和系统依赖（--yes 授权安装系统包）
ais2api doctor    检查本地环境
ais2api serve     在当前终端前台运行
ais2api start     后台启动
ais2api status    查看运行状态
ais2api stop      停止服务
```

### init

写入初始配置并初始化存储。它**不会生成或输出 API Key**；结果中的 `api_key_configured`、`next_step` 提示当前状态和下一步。新写入的配置使用默认管理密码 `admin`，此时结果中的 `admin_password_default` 为 `true`，`management_next_step` 说明如何登录控制台并修改密码。重复执行 `init` 不会覆盖已有的密钥。

### serve 与 start

- `serve` 在当前终端运行，日志写到 stderr。
- `start` 启动后台进程，并最多等待约 5 秒确认本地控制通道就绪；日志写入数据目录的 `service.log`。
- `status` 返回 `running: true` 表示进程和本地控制通道可达。它描述的是本地状态，不代表 Google 生成已经成功。
- `stop` 请求有序关闭；可轮询 `status` 直到 `running: false`。

`start` 超时时请查看 `data/service.log`。监听失败的日志会包含操作、地址、端口和系统错误码；常见原因见 [故障排查](troubleshooting.md#端口无法监听)。服务不会静默更换端口。

### setup、doctor 与浏览器版本

- `setup` 准备当前平台的浏览器和依赖，需要联网；`--yes` 授权本次列出的系统包变更。
- `doctor` 检查本地前提，并列出实际使用的程序、配置、下载、数据与日志路径。浏览器文件存在不代表 Google 调用成功。
- `browser update` 与 `browser rollback` 管理浏览器版本。
- 执行 `setup`、`browser update`、`browser rollback` 前需先停止服务，它们与服务使用同一把数据目录独占锁。
- 程序旁存在 `browser-runtime/PLATFORM/chrome[.exe]`（Browser-Local 包）时会自动使用，无需下载；`setup` 仍会校验其完整文件清单并准备系统依赖。

### 一键启动脚本

运行包根目录的 `start.sh`（Linux、Termux）与 `start.cmd`（Windows）依次执行 `init`、`doctor`，在没有浏览器时执行 `setup`，最后运行 `serve`（加 `--background` 时为 `start`），其他参数原样转发。详见 [快速开始](getting-started.md#2-一键启动)。

## 全局选项

选项可以写在命令之前或之后，值可以用空格或 `=` 连接：

```text
--config PATH          --data-dir PATH       --bind ADDRESS
--port PORT            --browser PATH        --proxy URL
--browser-proxy-relay=true|false
--headless=true|false                        --browser-idle-seconds N
--request-bytes N      --object-bytes N      --relay-bytes N
--queue-size N         --queue-seconds N     --generation-seconds N
--stream-idle-seconds N                      --max-attempts N
```

生效配置依次叠加默认值、配置文件、`AIS2API_*` 环境变量和命令行参数。`start` 会把生效设置传给子进程，但不会把代理凭据写进子进程命令行。对同一个服务执行的所有命令都要使用相同的 `--data-dir`。各选项的含义见 [配置参考](configuration.md)。

## 账号

```text
ais2api accounts import --file ./accounts.json --name "Imported account"
ais2api accounts import --stdin --name "Imported account"
ais2api accounts list
ais2api accounts select ACCOUNT_ID
ais2api accounts remove ACCOUNT_ID
ais2api accounts reset ACCOUNT_ID
ais2api accounts probe ACCOUNT_ID
ais2api accounts probe all
```

- **import**：输入可以是 Cookie 请求头文本、Netscape Cookie 文本、Cookie JSON、storage state 或已有凭据包。只有显式指定 `--stdin` 才会读取标准输入。不要把 Cookie 写在命令参数里，避免进入 shell 历史。导入与列表结果不包含 Cookie 值，但账号名称和邮箱对本机用户可见。
- **select**：设为首选账号。
- **reset**：清除账号记录的失败/禁用状态，不能修复已过期的 Google Cookie。
- **probe**：使用账号凭据实际访问上游，需要服务正在运行，可能会启动浏览器。不要把它当作离线健康检查。

账号 ID 来自 `accounts list`。

## 密钥

```text
ais2api keys list
```

返回 `source: "config"`、`configured` 和元数据，不包含密钥原文或其哈希。命令行不提供创建/撤销：`keys create` 与 `keys revoke` 返回 `reason: "keys_managed_in_config"`，请直接编辑 `config.json` 的 `api_key` 后重新加载。旧版本数据库中的密钥记录会保留，但不再用于 HTTP 鉴权。通过管理 API 创建的托管密钥存放在独立的表中，见 [管理 API](management-api.md)。

## 配置、模型与异步操作

```text
ais2api config show
ais2api config validate
ais2api config reload
ais2api stats
ais2api models list
ais2api operations status OPERATION_ID
```

- **config show**：隐藏 `api_key`、代理用户信息等凭据，其余设置原样显示。
- **config validate**：在本地校验配置。
- **config reload**：需要服务正在运行，应用可热更新的设置（包括 `api_key`，修改后旧 Key 立即失效）；存在需要重启的差异时返回 `restart_required`。
- **stats**：显示按客户端请求计数的累计结果、usage 和 USD 估算费用。`records` 为账号/模型分组，`totals` 为合计，`legacy_records` 单列功能启用前的原口径累计。`amount_nano` 为完整计价请求的小计，`known_amount_nano` 为含部分计价请求的已知小计，单位均为十亿分之一美元；`priced` / `unpriced` 区分完整与未完整计价请求，`partially_priced` 为后者中有已知金额的请求数。累计不随明细到期而清空，逐条请求与价格状态见 WebUI 或管理 API。
- **models list**：服务运行时请求实时发现并保存完整结果，可能启动浏览器；失败报错并保留之前保存的结果。客户端模型列表只读取本地，不发起发现。服务未运行时本命令只能返回 `configured_models`。

模型发现和账号探测以排队的异步操作执行，使状态查询和停止命令保持响应。命令行每 200 毫秒查询一次结果，最多等待 120 秒；仍未完成时以 `operation_pending` 退出，可以用返回的 ID 执行 `operations status` 继续查询。操作结果只临时保留。

## 本地控制通道

运行中的服务独占数据库，并通过仅限当前用户访问的本地通道（Windows 命名管道或 Unix 域套接字）接受命令。命令行先尝试连接运行中的服务；只有确认服务未运行（`not_running`）时，支持离线执行的命令才会在取得实例锁后直接打开数据库。超时或授权失败不会绕过服务直接访问数据库。

## 开发者冒烟测试

```text
python tests/cli_smoke.py --exe build/windows/ais2api.exe
```

只使用 Python 标准库，在工作区 `.tools/smoke-*` 下创建隔离的配置和数据目录，检查 CLI、本地控制通道和 HTTP。它不输出密钥、不准备浏览器、不访问 Google；结束后停止服务并删除临时目录。Python 只是开发工具，不是运行依赖。
