# 快速开始

本文介绍从获取程序到发出第一个请求的完整流程。如果需要自行编译，请先阅读 [构建与打包](platforms.md)。

## 1. 获取程序

GitHub Actions 每次构建都会生成运行包（保留 28 天）：

| 平台 | 运行包 | 一键启动脚本 |
|---|---|---|
| Windows 10/11 x64 | `.zip` | `start.cmd` |
| Ubuntu 24.04 / Debian 13 x86_64 | `.tar.gz` | `start.sh` |
| Android ARM64（Termux） | `.tar.gz` | `start.sh` |

把运行包解压到**当前用户可写的新目录**（Termux 需放在其私有存储内，Android 共享存储不能执行程序）。目录结构大致如下：

```text
ais2api/
├── start.cmd / start.sh      一键启动脚本
├── bin/
│   ├── ais2api(.exe)         主程序
│   ├── browser/bridge.js     页面桥接脚本（参考副本，程序使用内嵌版本）
│   └── userscript/           Cookie 导入脚本
└── share/                    文档、schema、许可证
```

首次运行后，`bin/` 旁会生成 `config.json` 和 `data/`，程序所有状态都在这两处。

> 可选的 `Browser-Local` 包内附锁定版本的 CloakBrowser，省去下载浏览器的步骤。它受 CloakBrowser 独立许可约束，仅供个人或内部使用，详见 [第三方说明](../THIRD_PARTY_NOTICES.md)。

## 2. 一键启动

- **Windows**：双击 `start.cmd`。
- **Linux / Termux**：在终端执行 `sh start.sh` 或 `./start.sh`。

脚本会依次完成：

1. 服务已经在运行时，只显示查看状态和停止服务的命令，然后退出。
2. 执行 `init` 创建配置与数据库。如果 `api_key` 为空或仍在使用默认管理密码，会给出提示。
3. 没有可用浏览器时执行 `setup`。Windows 只下载浏览器，直接进行；Linux 与 Termux 可能安装系统依赖，会先询问确认（非交互终端下拒绝执行）。
4. 以 `serve` 在当前窗口运行服务，按 Ctrl+C 停止。

附加参数：`--background` 改为后台启动；其他参数（如 `--port 18452`）原样传给每个命令。`start.cmd` 会临时把控制台切换为 UTF-8，退出时恢复原代码页，因此中文 Windows 的 GBK 控制台也能正常显示中文。

## 3. 设置 API Key

`init` 不会生成 API Key。打开 `config.json`，填入你自己的密钥：

```json
{
  "api_key": "换成你自己的长随机字符串"
}
```

然后执行 `ais2api config reload`（服务运行中）或重新启动。也可以在 WebUI 的“API 密钥”页设置。

## 4. 修改默认管理密码

新创建的配置默认管理密码为 `admin`。在浏览器打开 `http://127.0.0.1:18451/`，用 `admin` 登录后，控制台会强制要求设置新密码；修改之前，除改密码以外的管理操作都会被拒绝。默认密码只接受来自本机或局域网的连接。

> 仅在启用了 WebUI 的构建中才有网页控制台。GitHub CI 的 Windows、Ubuntu、Debian 与 Termux 运行包均已启用。

## 5. 导入 Google 账号

任选一种方式：

**方式 A：浏览器脚本（推荐）**

1. 在 `config.json` 设置 `cookie_import_key`（与 `api_key` 不同的独立密钥），执行 `ais2api config reload`。
2. 在 Tampermonkey 中打开 `http://127.0.0.1:18451/userscript/ais2api-cookie.user.js` 安装脚本。
3. 打开已登录的 AI Studio 页面，在脚本面板中填写导入密钥，点击“读取并导入当前账号”。

详见 [Cookie 导入](userscript.md)。

**方式 B：WebUI**

在控制台“账号”页选择“导入账号”，粘贴 Cookie 或凭据 JSON。

**方式 C：命令行**

```text
ais2api accounts import --file cookies.json --name personal
```

支持 Cookie 请求头文本、Netscape Cookie 文本、Cookie JSON、Playwright storage state 和已有凭据包。不要把 Cookie 写在命令参数里，避免进入 shell 历史。

导入只验证浏览器中的登录身份，不代表生成能力已经可用。导入后可以执行一次探测：

```text
ais2api accounts probe all
```

## 6. 发出第一个请求

首次导入账号后，先在控制台“模型”页执行“实时发现”，也可在服务运行时执行 `ais2api models list`。发现成功后按账号保存完整清单，服务重启后仍可读取；没有保存结果时可以通过 `configured_models` 提供备用列表。

```sh
# 查看已保存或显式配置的模型列表（只读本地，不触发发现）
curl http://127.0.0.1:18451/v1/models -H "Authorization: Bearer YOUR_KEY"

# OpenAI Chat Completions 流式请求
curl http://127.0.0.1:18451/v1/chat/completions \
  -H "Authorization: Bearer YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"model":"gemini-3.1-flash-lite","messages":[{"role":"user","content":"你好"}],"stream":true}'
```

模型 id 以 `/v1/models` 的实际返回为准。接入 Cherry Studio、SillyTavern、RikkaHub 等客户端的方法见 [客户端接入](clients.md)。

## 手动操作

不使用一键脚本时，完整流程如下：

```text
ais2api init                      创建 config.json 与数据库
ais2api setup --yes               下载浏览器并安装系统依赖
ais2api accounts import --file credentials.json --name personal
ais2api start                     后台启动（serve 为前台运行）
ais2api accounts list
ais2api accounts probe all
ais2api models list
ais2api status
ais2api stop
```

`setup --yes` 表示授权安装本次列出的系统依赖：Ubuntu/Debian 可能需要 sudo，Termux 会创建项目专属的 proot Ubuntu 24.04 容器。浏览器归档按固定摘要和大小校验，安装中断后可以重新执行。它不会升级其他系统软件或更换软件源。已有浏览器也可以通过 `--browser PATH` 或配置项 `browser_executable` 指定。

## 数据目录

| 路径 | 内容 |
|---|---|
| `config.json` | 配置文件（含明文密钥，请妥善保管） |
| `data/state.sqlite3` | 账号、Cookie、统计等数据库 |
| `data/service.log` | `start` 后台运行时的服务日志（JSON 行） |
| `data/management.log` | 管理日志，超过 8 MiB 时轮转 |
| `data/logs/browser-*.log` | 浏览器日志，只保留最近 8 份 |
| `data/runtime/` | 浏览器安装目录、下载缓存和临时资料 |

整个程序目录可以一起移动。不要把 `config.json` 或 `data/` 提交到仓库或分享给他人。

## 下一步

- [客户端接入](clients.md)：配置各类客户端、使用搜索模型。
- [配置参考](configuration.md)：代理、局域网访问、低内存模式等。
- [故障排查](troubleshooting.md)：常见错误与处理方法。
