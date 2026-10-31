# ais2api

[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-blue.svg)](LICENSE)

ais2api 是一个 C++20 编写的 AI Studio 网页会话网关：它驱动本地浏览器登录 Google AI Studio，把网页会话转换为 **Gemini 原生 API**、**OpenAI Chat Completions** 与 **OpenAI Responses** 接口，供 Cherry Studio、SillyTavern、RikkaHub 等客户端使用。支持 Windows、Linux 与 Android（Termux）。

> **开发版本**：Windows 上已用真实账号验证基础文本生成、流式、多轮和搜索，但工具调用、生图以及 Linux / Termux 的真实账号场景尚未完成验收。详见 [验证记录](docs/validation.md)。

## 特性

- **三种 API**：Gemini `generateContent` / `streamGenerateContent`、OpenAI `/v1/chat/completions` 与 `/v1/responses`，支持流式、多模态输入、函数调用、JSON Schema 输出。
- **真 role 与思考签名**：保留完整历史中的消息边界和原生部件，思考签名可在多轮对话中往返。
- **搜索模型**：每个模型自动提供 `<模型>-search` 变体，无需客户端配置即可开启 Google 搜索。
- **多账号管理**：账号轮换、按模型冷却与自动恢复、每个账号固定的浏览器指纹、Cookie 自动写回。
- **三种管理方式**：命令行、独立鉴权的管理 API，以及嵌入程序的 Web 控制台（运行时无需 Node.js）。
- **便捷导入**：Tampermonkey 脚本一键推送 Cookie，兼容原项目脚本。
- **便携部署**：配置与数据都在程序目录旁，整个目录可以直接移动；附带一键启动脚本。
- **资源可控**：请求、响应、队列都有上限；流式输出逐块确认、不整体缓存；可选空闲时关闭浏览器。

## 快速开始

1. 从 GitHub Actions 下载对应平台的运行包并解压（详见 [快速开始](docs/getting-started.md)）。
2. 启动：Windows 双击 `start.cmd`；Linux / Termux 执行 `sh start.sh`。首次运行会自动初始化并下载浏览器。
3. 在生成的 `config.json` 中填写 `api_key`，执行 `ais2api config reload`。
4. 打开 `http://127.0.0.1:18451/`，用默认密码 `admin` 登录控制台并按提示修改。
5. 导入 Google 账号：安装 [Cookie 导入脚本](docs/userscript.md)，或在控制台“账号”页粘贴 Cookie。
6. 在控制台“模型”页执行“实时发现”并保存清单，或通过 `configured_models` 配置备用列表。客户端只读取本地清单，不会自动发现模型。
7. 在客户端填写接口地址：

| 客户端 | 设置 |
|---|---|
| Cherry Studio / OpenAI 兼容 | Base URL `http://127.0.0.1:18451/v1` |
| SillyTavern | Google AI Studio，反向代理 `http://127.0.0.1:18451` |
| RikkaHub | Google 提供商，Base URL `http://127.0.0.1:18451/v1beta` |

API Key 均填写 `config.json` 中的 `api_key`。更多说明见 [客户端接入](docs/clients.md)。

```sh
curl http://127.0.0.1:18451/v1/chat/completions \
  -H "Authorization: Bearer YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"model":"gemini-3.1-flash-lite","messages":[{"role":"user","content":"你好"}],"stream":true}'
```

## 文档

| 使用 | 参考 | 开发 |
|---|---|---|
| [快速开始](docs/getting-started.md) | [协议合同](docs/protocol.md) | [构建与打包](docs/platforms.md) |
| [客户端接入](docs/clients.md) | [Responses 接口](docs/responses.md) | [验证记录](docs/validation.md) |
| [配置参考](docs/configuration.md) | [真 role 与思考签名](docs/true-role.md) | [路线图](docs/roadmap.md) |
| [命令行](docs/cli.md) | [管理 API](docs/management-api.md) | [贡献指南](CONTRIBUTING.md) |
| [Web 管理界面](docs/webui.md) | [浏览器网关](docs/browser.md) | [专项报告](docs/reports/README.md) |
| [Cookie 导入](docs/userscript.md) | | |
| [故障排查](docs/troubleshooting.md) | | |

完整目录见 [docs/README.md](docs/README.md)。容器部署见 [Docker 部署](docs/docker.md)。

## 从源码构建

```powershell
# Windows（开发者 PowerShell）
./scripts/build.ps1 -Package -WebUI
```

```sh
# Ubuntu 24.04 / Debian 13
sh scripts/build.sh linux --package --webui
# Termux（需先 pkg install boost-headers）
CXXFLAGS=-fexperimental-library sh scripts/build.sh termux --install-deps
```

依赖由 `vcpkg.json` 的固定 baseline 锁定。构建 WebUI 需要 Node.js 20.19+ 或 22.12+；运行时不需要 Node.js 或 Python。各平台的前提和选项见 [构建与打包](docs/platforms.md)。

## 限制

- 同一时间只处理一个生成请求，其余排队。
- Responses 为无状态模式；没有 Files、embeddings 或独立的图片/音频接口。
- `thinkingBudget` 与 `seed` 被接受但不生效；强制工具调用与 strict schema 默认拒绝（可选择舍弃后继续）。
- 搜索来源与引用元数据暂不解析。
- 依赖 AI Studio 网页的私有协议，上游变化时需要更新适配器。

## 致谢与许可

工作原理参考了 [aistudio-api-termux](https://github.com/ReedSein/aistudio-api-termux/tree/84b50663fcae79058fe33e0e45b5c3452da7c461)；本项目的实现代码与测试样例为独立编写。

Copyright (c) 2026 Mana Nekoha (mananekoha114)。本项目自有源码、脚本、schema、测试与文档采用 [Mozilla Public License 2.0](LICENSE)，许可声明见 [NOTICE](NOTICE)。第三方依赖与浏览器保留各自的许可，其中可选 `Browser-Local` 包内的 CloakBrowser 仅限个人或内部使用，详见 [第三方说明](THIRD_PARTY_NOTICES.md)。

请勿将真实 Cookie、API Key 或数据目录提交到任何仓库。
