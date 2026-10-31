# 本机验证与回放记录

本目录保存各次修复与验证的专项报告。报告是对当时状态的历史记录，后续行为可能已有变化，当前状态以 [验证记录](../validation.md) 和各功能文档为准。

## 专项报告

| 日期 | 报告 | 内容 |
|---|---|---|
| 2026-10-31 | [模型目录、请求记录与参考费用](request-usage-pricing-2026-10-31.md) | 模型目录持久化、请求与尝试、部分费用、内容拦截、迁移和留存；Termux 原生、离线 HTTP 与桌面/手机浏览器验证 |
| 2026-10-24 | [搜索 502 修复与搜索模型](search-fix-2026-10-24.md) | 与参考项目的搜索差异、响应容错解析、`*-search` 模型、OpenAI 搜索入口，以及 `gemini-3.1-flash-lite` 的真实账号验收 |
| 2026-10-22 | [Google 请求逻辑对齐](upstream-request-alignment-2026-10-22.md) | 模型默认值、会员等级头、重试、配额冷却与完成判定的落地范围、保留差异及验证证据 |
| 2026-10-22 | [Google Search 与内置工具转换修复](grounding-tools-2026-10-22.md) | RikkaHub 混用工具的拒绝原因、搜索参数与时区转换、回归覆盖和真实账号验收边界 |
| 2026-10-22 | [Gemini 客户端兼容修复](gemini-clients-2026-10-22.md) | RikkaHub / SillyTavern 源码核查、请求合同修复和离线 HTTP/SSE 回归，预算与种子限制 |
| 2026-10-22 | [API 兼容审查](api-compatibility-2026-10-22.md) | Cherry Studio schema 拒绝问题、三种 API 的兼容矩阵、降级语义与未验证项 |
| 2026-10-22 | [桥接版本错配修复](browser-embedded-bridge-2026-10-22.md) | Cherry Studio `browser_incompatible` 的原因、桥接脚本内嵌与版本诊断、真实复测 |
| 2026-10-21 | [前端管理 API 与完整实时日志](management-api-2026-10-21.md) | 管理鉴权、账号/配置/多密钥接口、历史与实时日志、故障行为及本机回归 |
| 2026-10-21 | [模型发现与账号身份等待修复](model-discovery-2026-10-21.md) | 真实账号的身份误判复现、Windows ListModels 复测，以及 WSL 到 Windows 服务的调用 |
| 2026-10-21 | [真实文本生成与 XHR 初始化修复](generation-bootstrap-2026-10-21.md) | 生成模板与快照适配、同一账号的普通生成、SSE、Gemini 和多轮复测 |
| 2026-10-21 | [LLM API 对照与真实请求复核](llm-api-parity-2026-10-21.md) | 固定原项目提交的 31 项离线差分与 5 次廉价模型真实生成：Flash-Lite 媒体/schema/多轮与 Gemma SSE 通过，函数调用 403 |
| 2026-10-20 | [Debian 13 / WSL 2 本机适配](debian13-wsl2-2026-10-20.md) | 额外 Linux 目标的构建、运行与无账号浏览器验证 |
| 2026-10-20 | [便携存储、并行下载与浏览器整合包](portable-browser-2026-10-20.md) | Windows / Debian 回归、下载对照和本地整合包校验 |
| 2026-10-20 | [上传前范围与归属审查](publication-audit-2026-10-20.md) | 首次上传前的忽略规则、凭据扫描、许可与第三方归属审查 |

LLM API 对照的原始数据：[llm-api-differential-2026-10-21.json](llm-api-differential-2026-10-21.json)、[llm-api-gemma-supplement-2026-10-21.json](llm-api-gemma-supplement-2026-10-21.json)、[llm-api-live-audit-2026-10-21.json](llm-api-live-audit-2026-10-21.json)、[llm-api-live-updated-cookie-2026-10-21.json](llm-api-live-updated-cookie-2026-10-21.json)。

## Windows 开发回放记录

2026-10-20，Windows 11 Pro Insider Preview 10.0.29634 x64；AMD Ryzen AI 7 H 350（16 逻辑处理器），系统可见内存 32,630,636 KiB。MSVC 14.51、C++20、Release、静态运行库，依赖由根 vcpkg baseline 锁定。采集时工作区尚未初始化 Git，这些测量不能绑定到之后的任何提交。

| 记录 | 负载 | 结果 |
|---|---|---|
| [未节流回放](windows-unpaced-120s.json) | 4 个客户端持续新建短连接，无等待 | 120 秒，28,111 次尝试，15,857 成功，12,254 失败；不通过 |
| [有间隔回放](windows-paced-120s.json) | 4 个客户端，每次完成后等待 100 ms | 120.104 秒，4,414/4,414 成功，零请求错误、零采样错误 |

未节流回放当时尚未采集错误分类，不能断言失败全部由某个原因造成。成功数接近 Windows 动态端口容量，短连接与 TIME_WAIT 是待验证的归因；该轮初期还并行运行了浏览器未登录冒烟测试，不能作为受控性能基线。之后工具增加了错误分类和固定的客户端间隔，没有修改系统 TCP 参数。两轮负载不同，**不能用它们计算加速倍数**。

有间隔回放的 p50/p95/p99 约为 5.36/7.54/10.11 ms（固定直方图，量化约 5%）。进程 RSS 首/末/采样峰值约 12.69/13.89/14.03 MB（十进制），队列采样峰值为 1。生成工作线程累计 CPU 0.95 秒；包含 HTTP 客户端、服务和采样器的整个回放进程累计 CPU 24.97 秒。这些不是独立服务进程或浏览器的资源数据，也不是极限吞吐能力。

两个负载都使用合成生成源、真实的回环 HTTP/SSE、SQLite 与本地控制通道；没有实际模型生成，也没有与原项目对照。两小时运行、慢客户端/大媒体/取消混合回放、浏览器资源测量和三端验收仍待完成。工具说明见 [性能工具](../performance.md)。
