# 路线图与实施状态

状态截至 2026-11-23。阶段按原计划的进入条件验收，不能只根据代码是否存在判断完成。各项的证据见 [验证记录](validation.md)。

浏览器后端已从 CloakBrowser 迁移为 Camoufox 与 SpiderMonkey 两种 Firefox 后端（见 [Firefox 后端迁移计划](firefox-backends.md)），并随 [v0.2.0](https://github.com/mananekoha114/ais2api/releases/tag/v0.2.0) 预发布。真实账号目前只在 Windows 上验证过。

## 阶段状态

| 阶段 | 已实现或验证 | 仍需完成 |
|---|---|---|
| M0 工程与合同 | C++20/CMake、锁定依赖、配置与扩展 schema、独立协议测试；Windows、Debian 13 / WSL 2 的 GCC/Clang 与 ARM64 Termux 设备上的离线测试 | Ubuntu 上的实际编译和离线测试 |
| M1 最小真实链路 | Camoufox（WebDriver BiDi 与页面桥接）和进程内 SpiderMonkey 两种后端；Windows 真实账号的单轮与多轮生成，三种后端组合的对照实验全部通过 | 其他平台的真实账号生成 |
| M2 流式与限额 | 增量解析、SSE、信用确认、事件唤醒、断开取消、队列与对象限制、请求体分块上传、可选的非流式保活 | 真实上游与慢客户端、大媒体、断连故障的组合验证 |
| M3 完整生成能力 | 四种 API（含 Anthropic Messages）、签名、函数值、工具、搜索、生图与内联多模态的映射和往返测试；本地 token 计数；真实账号搜索通过；Claude Code 对合成后端的离线端到端通过 | 对应权限账号的完整能力矩阵（函数调用、签名往返、生图），Anthropic 接口与 Claude Code 的真实账号验证 |
| M4 账号运行管理 | CLI/IPC、管理 API、WebUI、凭据更新与 Cookie 写回、冷却与自动恢复、账号调度、永久指纹、按账号选择后端与 `auto` 自动切换、密钥、统计、SQLite | 真实账号切换、浏览器崩溃、长期凭据与重启恢复验证 |
| M5 三端环境 | Windows 原生准备、Debian 13 / WSL 2 自动准备；Termux 原生 NDK 构建默认使用 SpiderMonkey，无需 `proot` 容器；三端构建与准备入口、升级目录切换与回退；一键启动脚本 | Ubuntu 与真实 Android Termux 的干净安装、升级失败与真实调用 |
| M6 稳定与发布 | Windows / Linux / Termux CI 运行包、校验与验包工具、固定负载回放；v0.2.0 预发布 | 两小时资格测试、Windows 以外的资源测量、原项目同机对照、三端完整发布门槛 |

目前交付的是可构建、可在 Windows 上用真实账号进行基础生成的开发版本，不能称为完整重写验收通过。

## 待决定的方向

以下改动影响较大或可能影响正常请求，需要先确定方向再实施。

以下设计方向按 Firefox 后端持续评估。

### 并发生成

现状：一个浏览器、一个驻留账号、同一时间一个生成请求；排队超过 `queue_seconds` 返回 504，账号探测和模型刷新也在同一队列中。Claude Code 的子代理并行工作时会排队，目前只能调大 `queue_seconds` 和 `queue_size`。参考项目可以在同一页面同时运行多个流式请求，切换账号时在后台标签页预先准备。

约束：指纹作用于整个浏览器进程，一个进程只能有一个指纹。多账号并存需要多个浏览器进程，或让指纹相同的账号共用进程。可选范围至少有两种：同账号多请求并发，以及多账号并存。

已定：Firefox 后端的第一版主线不改并发模型，多账号并发在 SpiderMonkey 后端稳定后再评估。SpiderMonkey 后端不需要浏览器进程，常驻内存约为 Camoufox 的十分之一（见 [P4 报告](reports/spidermonkey-experiment-2026-11-15.md)），为并发提供了更低成本的基础。

### 每个账号固定的浏览器资料目录

现在每次启动浏览器都新建随机资料目录，用完删除；参考项目每个账号使用固定目录。由于指纹已经固定、Cookie 也会写回，这一项的收益不确定，但会带来磁盘占用和目录锁问题。

### 浏览器资源控制

- **资源屏蔽**：若需进一步缩小 Camoufox 页面带宽占用，仍需评估对 BotGuard 的影响。
- **JS 内存上限**（`--max-old-space-size`）：分块上传已消除请求体的多份副本，但 AI Studio 前端本身的堆大小需要在真实账号上测量后才能确定上限，或做成配置项。

## 已知的小优化

- `decode64` 对每个字符线性查找字母表。
- `rpc_text_error` 在前 16 KiB 填满后，每块仍重新解析同样的内容。
- 账号数据库 `state.sqlite3` 在 WAL 模式下仍使用默认的 `synchronous=FULL`；`usage.sqlite3` 使用 `NORMAL`。
- 请求记录已改为后台排队写入，但鉴权、模型清单和管理接口读取等其他存储调用仍在 HTTP 线程上同步等待。
- 每个请求多次复制整个配置。

## 已知的功能缺口

- 搜索来源与引用元数据未解码，Responses 不输出 `web_search_call`。
- Anthropic Messages 不支持服务端工具（WebSearch、代码执行等）、强制工具调用和 `clear_thinking_20251015` 以外的 `context_management`；`count_tokens` 是本地估算。
- Camoufox 后端尚未识别 `GetLoggingContext` 返回的 Code 7（地区或账号不受支持），SpiderMonkey 后端已识别。
- 代理池尚未实现（见 [低成本改进](firefox-backends.md#低成本改进独立推进)）。模型名思考强度后缀只按模型系列判断支持的档位，没有读取 `ListModels` 的档位目录。
- `thinkingBudget` 与 `seed` 没有可验证的上游映射。
- RikkaHub `includeServerSideToolInvocations` 要求的额外调用记录未实现。
- 没有 Files、embeddings、独立的图片/音频接口；Responses 没有服务端存储与续接。
- 分区 Cookie 不支持。
