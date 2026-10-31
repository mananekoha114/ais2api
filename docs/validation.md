# 验证记录

更新：2026-10-31。本项目仍是开发版本，尚未达到完整版本的发布门槛。本文汇总已执行的验证及其能证明的范围；各专项的详细过程见 [docs/reports/](reports/README.md)。

阅读时请注意区分四类证据，它们不能互相替代：

- **离线合成**：合成样例、模拟页面或合成生成源，不连接 Google。
- **真实浏览器**：真实启动 CloakBrowser/Chromium，但使用合成 Cookie 或拦截页面。
- **真实账号**：使用真实 Google 账号 Cookie 访问 AI Studio。
- **真实设备**：在目标平台的真实环境（如 Android 手机）上运行。

## 自动化测试

| 范围 | 最近结果 | 能证明的范围 |
|---|---|---|
| C++ 全量测试 | 2026-10-31，ARM64 Termux，Catch2 330 个用例、5,433 条断言通过（[模型目录与请求计价报告](reports/request-usage-pricing-2026-10-31.md)）；此前 Windows 结果见[搜索修复报告](reports/search-fix-2026-10-24.md) | 合成协议、解析器、存储、CLI 控制通道、HTTP、资源限制、重试、逐请求记录、计价、迁移与保留期等 |
| 页面桥接脚本 | `node --test browser/test_bridge.cjs` 46 项通过 | 模拟页面下的捕获、快照、信用协议、取消、身份与错误分类 |
| WebUI | 2026-10-31，`vue-tsc --noEmit` 与 Vitest 8 个文件、41 项通过 | 前端类型与交互（模拟管理 API），包括模型目录替换、失败保留、统计与请求记录的筛选、分页、日期边界、生成设置与部分费用 |
| CLI 冒烟 | ARM64 Termux 36 项通过（2026-10-31）；Windows 36 项通过（2026-10-23）；Windows / Debian 各 34 项（2026-10-22） | init 不生成密钥、旧配置迁移、start/stop、在线存储、配置密钥轮换与清空、脱敏、热更新 |
| Responses 专项 | Windows 14 个用例、233 条断言 | JSON / 命名 SSE、usage、函数签名与完整历史、重复工具 ID、图片输入、schema、截断与失败状态、流式限额、FIN/RST 取消；未运行真实账号或 SDK 集成 |
| Cookie 导入与专用脚本 | Windows / Debian 离线 HTTP 测试及 Node VM 测试通过 | 原版推送格式、独立密钥、元数据转换、分区过滤、身份检查、事务更新、队列限额、超时、取消与保存前撤销检查；未使用真实 Cookie |
| Linux sanitizer 与模糊测试 | Debian 13 GCC 14.2 与 Clang 19 ASan/UBSan 均为 105 个用例、1,591 条断言通过；Clang ASan/UBSan 下 21 秒完成 878,837 次输入，无 sanitizer 错误 | 本机 WSL 2 原生 Linux；短时线路解析器冒烟，不是长期稳定性或完整覆盖证明 |

C++ HTTP 测试使用真实的回环 TCP、HTTP 分块编码、SQLite 和本地 IPC，生成源是合成的 `GenerationBackend`。它们验证了多轮、Gemini 聚合、OpenAI SSE usage 数据块、正常结束、首个内容后出错不重试以及客户端断开取消。生产程序始终使用 BrowserGateway，没有模拟生成开关。

其他回归覆盖：任意网络分块、UTF-8、XSSI、截断与对象限额；函数值与 schema、逐块签名、重复工具调用 ID 与扩展往返；账号隔离元数据、冷却与持久化；归档越界、重复文件、Windows 设备路径；32 个半请求头连接上限；累计媒体预算；超大及已完成的下载残片处理；`probe all` 在单个账号过期时继续检查其他账号、过期账号不参与调度、无可用账号时 `/ready` 返回 503；Cookie 文件/管道末尾的 CRLF 与 UTF-8 BOM。

## 真实浏览器与真实账号

| 范围 | 环境与结果 | 能证明的范围 |
|---|---|---|
| 搜索与 `*-search` 模型 | 2026-10-24，Windows 真实账号，`gemini-3.1-flash-lite`，8 个搜索请求全部 200，回答包含训练数据之后的信息 | Gemini 原生搜索（普通与 SSE）、`-search` 模型、Chat `web_search_options`、Responses `web_search`；见 [报告](reports/search-fix-2026-10-24.md) |
| 基础文本生成 | `gemini-3.1-flash-lite` 的 OpenAI 普通生成、SSE、Gemini 普通生成与 WSL 多轮均为 200 | Windows 真实账号、XHR 模板、逐请求快照、正文、usage 与 SSE；不代表全部模型或高级能力；见 [报告](reports/generation-bootstrap-2026-10-21.md) |
| 模型发现 | Windows 冷启动与缓存请求、Gemini 列表、模型详情与 WSL localhost 调用均为 200 | 真实 Cookie、页面唯一身份、上游 ListModels；见 [报告](reports/model-discovery-2026-10-21.md) |
| LLM API 对照 | 固定原版提交 3e2eff8，31 项离线同输入转换对照；更新 Cookie 后 5 次真实生成 | Flash-Lite 的 system / 多轮 / schema / 图片、原生 SSE 音频与 stop、Gemma OpenAI SSE 通过；函数调用返回 403，结果回传未执行，不能宣称完全等价；见 [报告](reports/llm-api-parity-2026-10-21.md) |
| 桥接版本错配修复 | 修复后 Flash-Lite 真实生成复测 200 | 内嵌桥接脚本与版本诊断；见 [报告](reports/browser-embedded-bridge-2026-10-22.md) |
| 原版 Cookie 头兼容 | Windows / Debian 真实浏览器离线回归通过 | 五项 Google 鉴权 Cookie 保持 JS 可读、HSID 保持 HttpOnly、无输入框时识别唯一账号；合成 Cookie 与拦截页面 |
| 原版 userscript 导入 | 用户确认原版脚本导入成功，随后真实生成通过 | 原版“抓取并推送”路径；专用脚本未在所有扩展管理器下实测 |
| 浏览器准备 | Windows 下载锁定附件、SHA-256 校验、解压，重跑复用安装目录 | CloakBrowser 146.0.7680.177.4 实际启动、CDP 1.3、创建与释放隔离上下文 |
| 未登录路径 | 合成 Cookie 下真实页面返回登录跳转，401 / `session_expired` 分类正确 | 浏览器启动、导航、桥接脚本与过期状态记录 |
| 无账号浏览器专项 | 内置 Chromium 146，本地伪造页面、代理指向不通端口 | 流式等待式转发、崩溃感知（`Inspector.targetCrashed`）、30 MiB 请求体分块上传后渲染进程内存从 175 MB 降至 48 MB；未在真实生成中验证 |

## 平台与打包

| 范围 | 结果 | 能证明的范围 |
|---|---|---|
| Windows 构建与安装包 | MSVC 14.51、C++20、Release、静态运行库；CMake install、CPack ZIP、摘要与资产检查通过 | 本机编译链接；独立后端、脚本、清单、schema、文档与依赖许可证打包 |
| ARM64 Termux 原生与 WebUI | 2026-10-31，原生构建/CTest、CLI 36 项、便携目录 59 项、Chromium 桌面/手机视口通过；见[模型目录与请求计价报告](reports/request-usage-pricing-2026-10-31.md) | Android API 35 实机上的合成与隔离验证，含模型页和展开生成设置的文字测量；未运行真实 Google 生成或完整设备验收清单 |
| Windows WebUI 整合 | Windows 前端构建、内嵌与纯后端构建、安装与 ZIP 校验通过；合成实例登录、模型/密钥/配置页与日志 SSE 检查 | Windows 本地管理界面集成的既有验证 |
| Debian 13 / WSL 2 | 普通用户、保持沙箱，CDP 1.3 与隔离上下文；C++ setup、TGZ 检查、已安装程序冒烟、Windows 到 WSL localhost 健康请求通过 | 系统依赖、锁定浏览器、CLI/IPC/HTTP；无真实账号；见 [报告](reports/debian13-wsl2-2026-10-20.md) |
| 便携目录与本地浏览器包 | Windows / Debian 普通模式 35 项、内置浏览器模式 38 项；逐文件清单、SHA-256、许可证与无用户数据检查通过 | 更换工作目录、移动到含空格目录、用户目录无新增数据、配置密钥保留；见 [报告](reports/portable-browser-2026-10-20.md) |
| 本地回放短测 | 4 个客户端、100 ms 间隔、120 秒，4,414 次全部成功 | 合成生成源下的 HTTP/SSE、调度与 CPU/RSS；不是浏览器或两小时验收；见 [性能工具](performance.md) |

## 尚未满足的发布门槛

| 发布条件 | 当前状态 |
|---|---|
| Windows 真实账号单轮、多轮、流式 | Flash-Lite 的普通生成、OpenAI SSE、多轮与搜索通过；完整能力矩阵未完成 |
| 工具调用、思考签名、多模态、生图 | 图片与短音频输入通过；函数调用返回 403，结果回传与逐块签名的真实往返未验证；生图未测 |
| 页面引导与身份确认 | Windows 真实账号通过；其他平台未用真实账号验收 |
| Ubuntu 24.04 GCC/Clang | 已配置 CI；本机仅验证 Debian 13 |
| Android 10+ ARM64 Termux | Android API 35 实机原生构建、离线测试和隔离 WebUI 已通过；干净安装及完整真实账号能力矩阵仍未完成 |
| 三端干净环境自动准备、升级失败与回退 | 未完成真实验收 |
| 至少两小时固定回放、浏览器资源与泄漏检查 | 未完成；短测不能代替 |
| 同设备、同负载与原项目对照 | 未执行，无加速倍数结论 |

## 尚未实际验证的实现

以下功能已实现并有单元或合成测试，但未在真实账号上确认：

- Cookie 写回（诊断页的 `cookieRefresh` 可观察实际效果）。
- 204 之后在同一页面重新引导；失败时会以 `browser_incompatible` 落到重启浏览器的逻辑。
- 弹窗关闭与 Ctrl+Enter 备用触发方式（仅在合成 DOM 上测试）。
- WebRTC 防泄露参数（只确认不改变指纹，未证明确实阻止泄露）。
- 日志轮转与 `service.log` 截断的 POSIX 分支（依赖 CI 在 Linux/Termux 上编译运行）。

## 有意的限定

- `/ready` 返回 `scope=local_prerequisites` 与 `google_verified=false`，它不是生成成功的证明。
- 配置声明的模型标记为 `configured`，不把固定模型表当作实测能力。
- `thinkingBudget` 与 `seed` 没有经过验证的私有协议映射，接受但忽略；`thinkingBudget=0` 不关闭思考。强制函数调用（ANY / required / 指定函数）、`VALIDATED` 与 strict JSON Schema 默认拒绝。详见 [协议合同](protocol.md)。
- 当前只有一个驻留上下文和一个生成任务，没有多上下文并发调度。统计保存逐请求用量、参考价格快照与账号/模型累计；请求记录和日志不写正文、Cookie、API Key 或签名。
- Termux 使用官方 proot-distro，其系统依赖包含 Python。C++ 后端与浏览器脚本运行时不依赖 Python/Node。系统包版本由平台包管理器决定，浏览器版本与桌面源码依赖则被锁定。

## 复核命令

```text
ctest --preset windows
node --check browser/bridge.js
node --test browser/test_bridge.cjs
node userscript/test_cookie_helper.cjs
python tests/cli_smoke.py --exe build/windows/ais2api.exe
python scripts/check-package.py build/windows/packages/ais2api-0.1.0-Windows-AMD64.zip
cd webui && npm run typecheck && npm test
```

Linux / Termux 的构建入口与系统前提见 [构建与打包](platforms.md)，固定负载命令与报告限制见 [性能工具](performance.md)。
