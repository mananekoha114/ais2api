# SpiderMonkey 原型（P3）验证

日期：2026-11-12。[Firefox 后端迁移计划](../firefox-backends.md) 的 P3：把 SpiderMonkey 152.0.4 编成静态库嵌入 ais2api，不启动浏览器，在进程内运行 Google 的 WAA（BotGuard）程序生成 proof，再由 ais2api 直接发出生成请求。本文记录当时的结果，后续行为以代码和功能文档为准。网络指纹的调研见 [Firefox 网络指纹调研](firefox-network-fingerprint-2026-11-12.md)。

## 结论

| 完成条件 | 结果 |
|---|---|
| 生成的 proof 被 AI Studio 接受 | 初步通过。一个测试账号用 SpiderMonkey 后端生成 2 次（冷启动非流式、复用 VM 的流式），都成功，没有 403、Code 7 或冷却 |
| 通过率与 Camoufox 相当 | 没有证明。样本只有上面 2 次。四格对照实验的第一轮就遇到一次 403，按停止条件中止；那一格同时存在几种差异，不能据此判断原因（见“四格对照实验”）。对照实验移到 P4，在实现 Firefox 网络指纹之后进行 |
| 决定网络指纹实现方案 | 调研完成，建议以 curl-impersonate 为基础，由维护者选定 |
| 资源占用 | 明显降低：生成后进程工作集 75 MiB，没有浏览器进程；Camoufox 后端是 5 个进程共 706 MiB，加 ais2api 本身 21 MiB |

新账号的默认后端暂不改变，仍是 Camoufox（`auto`），到 P4 再定。

## 环境

- Windows 11 x64 开发机，MSVC Release 构建，运行库 `/MT`。
- SpiderMonkey 152.0.4 用 Mozilla 的 `--enable-bootstrap` 编译（clang-cl 20.1.8，Rust 1.90.0），打补丁让独立的 js 构建也用 `/MT`，MSVC 编的 ais2api 直接静态链接。编译和打包的配方在 `tools/spidermonkey/`，产物（约 408 MB）不进仓库。
- 网络层是 vcpkg 的 libcurl，Windows 上用 Schannel，开了 HTTP/2。
- 对照用 Camoufox `v158.0.1-beta.41`。
- 独立的配置与数据目录，只导入测试账号，经本机 HTTP 代理访问 Google。所有生成请求只使用 `gemini-3.1-flash-lite`。

## 实现概要

- **构建拆分**：`ais2api_common`、`ais2api_bidi`（只有头文件）和 `ais2api_waa` 三个库，CTest 里的分层检查保证它们不依赖服务端代码。CMake 选项 `AIS2API_SPIDERMONKEY` 默认关闭，开启时用 `AIS2API_SPIDERMONKEY_DIR` 指定静态库。CI 只编关闭选项的版本。
- **WAA 运行时**（`waa/`）：参考项目 AIStudio2API 的 `dom.js`、`smhost.js` 和对象结构表逐字节取用（MIT 声明在 `THIRD_PARTY_NOTICES.md`）；Go 部分译成 C++，包括 challenge 解析、事件循环线程、两个 Realm、11 个原生函数、`settle`、`proof` 和 snapshot 刷新。Google 的解释器和 program 运行时下载，按摘要缓存在数据目录，不进仓库。
- **`SpiderMonkeyGateway`**（`src/spidermonkey.cpp`）：
  - 使用账号已有的 Camoufox 身份，没有身份时返回 503 `browser_not_configured`；
  - 用 Firefox 的请求头顺序加载页面，取得 API key 和页面全局量，调用 `GetLoggingContext`、`GetAiStudioBenefitTier`；
  - 通过 `Waa/Create` 取得 challenge，启动 VM；
  - 对每个请求按请求内容算 `wire_content_hash`，交给 VM 生成 proof，再带 proof 发 `GenerateContent`，之后照页面的做法发一次 `CountTokens`；
  - 跳转登录页、地区或账号不受支持、proof 无效各有对应的错误码。
- **接入**：按账号选择后端时多了 `spidermonkey`，诊断信息、按后端统计、WebUI 和管理接口都能显示它。没有开启选项的构建拒绝选它（400 `backend_unavailable`），已经存下的设置按 Camoufox 处理。
- **实验程序**（`tools/waa_experiment.cpp`）：两个网关的 `generate` 拆成“取 proof”和“发送”两半，实验程序交叉组合它们，服务本身的行为不变。CMake 选项 `AIS2API_BUILD_WAA_EXPERIMENT` 开启，不安装、不打包。

## 离线验证

| 构建 | 结果 |
|---|---|
| 关闭选项 | 489 个用例全部通过，分层检查通过 |
| 开启选项 | 505 个用例全部通过（多出的 16 个是离线宿主测试，用本地脚本驱动 VM），分层检查通过 |
| 其他 | `browser/test_bridge.cjs` 48 个、WebUI vitest 59 个通过，`vue-tsc` 无错误 |

PR 里的三个代码提交（库拆分、WAA 运行时、网关）分别在两种构建下编译和测试过，都通过。

可执行文件：开启选项后从 11.5 MB 增加到 44.0 MB（+32.4 MB），不需要额外的 DLL。

## 真实账号验证

一个测试账号，先用 Camoufox 生成一次作基线，再把账号切到 SpiderMonkey 后端：

| 测试 | 结果 |
|---|---|
| 基线：Camoufox，非流式 | 200，2.2 秒 |
| SpiderMonkey 第一次，非流式 | 502 `browser_incompatible`。上游已返回 2xx 和数据，是本地中继状态的类型错误，见“发现的问题” |
| 修复后，非流式（冷启动） | 200，8.1 秒，其中 VM 引导 1840 ms；另外包括加载页面、页面 RPC、proof 和生成 |
| 修复后，流式（复用 VM） | 200，0.98 秒，首字节 0.94 秒，正常结束 |

- **内存**：SpiderMonkey 后端没有浏览器进程，ais2api 空闲 14 MiB，生成后工作集 75 MiB、私有内存 61 MiB。同一账号在 Camoufox 后端下是 5 个进程，工作集 706 MiB、私有内存 545 MiB，加 ais2api 21 MiB。
- **诊断**：显示后端 `spidermonkey`、网关就绪、网络 `libcurl`、声称的 Firefox 版本 152、SpiderMonkey 152.0.4。按后端统计里第一次失败记为一次重启。
- 导出的诊断报告里没有邮箱、账号 ID、本机路径、代理地址、Cookie 名和 GPU 字符串。
- 全程没有 403、Code 7 或冷却。测完账号改回 `auto`。

## 四格对照实验

### 设计

为了区分拒绝是来自 proof 的来源，还是来自发送方式，实验程序交叉组合两者：

| 格 | proof | 发送 | 含义 |
|---|---|---|---|
| 1 | Camoufox 页面 | 页面 | 现在的 Camoufox 后端 |
| 2 | Camoufox 页面 | libcurl | 只换发送方式 |
| 3 | SpiderMonkey | 页面 | 只换 proof 的来源 |
| 4 | SpiderMonkey | libcurl | SpiderMonkey 后端 |

- 一个账号、一个很短的提示词，四格逐轮轮流，使它们处在同一段时间里。先在同一会话里测（保留页面和 VM），再在每次请求前都新建会话。计划每格每种 10 次。
- 每次尝试写一行 JSON：结果、错误码、取 proof 和发送的耗时、是否新启动了 VM，不含提示词、proof 和回复。
- 停止条件：第一次 401、403、429、地区或账号拒绝就全部停止；某一格连续两次其他失败就停这一格。

### 结果

| 时间（UTC） | 格 | 结果 |
|---|---|---|
| 13:47 | 1 | 401 `session_expired`：Camoufox 停在登录页。没有发出生成请求，实验停止 |
| — | — | 账号重新导入 Cookie |
| 14:34 | 1 | 成功，32.8 秒（含启动 Camoufox；取 proof 17.2 秒，发送 15.6 秒） |
| 14:36 | 2 | 403，RPC 错误码 7（`permission_denied`），实验停止 |

之后没有再发生成请求，格 3、格 4 和新会话部分都没有跑。

### 分析

格 2 的 403 不能归因于发送方式。除了网络层（Schannel 的 TLS 握手，HTTP/2 设置也不是 Firefox 的）以外，这一格还有两处差异：

- **版本不一致**：proof 在 Camoufox 158 的页面里生成，而 libcurl 发送时的 User-Agent 声称 Firefox 152（SpiderMonkey 后端按 SpiderMonkey 的版本声称）。
- **页面上下文不一致**：proof 属于 Camoufox 页面的那次访问（visit id、`GetLoggingContext` 的结果、WAA 实例），请求却带着 libcurl 会话自己的另一套。

同一天早些时候，proof 和发送都来自 SpiderMonkey 的组合（等同于格 4）成功了 2 次，与“libcurl 发送就会被拒”不符；更可能是混合两个来源本身被识别，但样本太少，不作结论。这也说明混合格的设计本身有缺陷：要单独比较发送方式，Camoufox 的 proof 与 libcurl 请求的版本和页面上下文必须一致，代价是要让 libcurl 复用 Camoufox 页面的会话状态。

这次 403 是 P3 期间唯一的 Code 7 数据点，又出在人为构造的混合组合上，不足以用来校准 `retry_policy.hpp`。

## 发现的问题与修正

- **中继序号类型错误**：网关的中继序号是无符号数，Boost.JSON 存为 `uint64`，而生成转发代码检查的是 `is_int64()`，第一块数据就被判为序号不符；`retryAfter` 也会因此被当作 0。新增纯函数 `relay_state` / `relay_chunk` 统一转成有符号数，并补了 2 个回归测试。第一次失败时上游已返回 2xx，账号没有受影响。
- **登录失效**：对照实验第一次运行时，账号在 Camoufox 里停在登录页（401 `session_expired`）。同一份 Cookie 当天早些时候在 Camoufox 和 SpiderMonkey 下都正常，重新导入后恢复。原因不明。账号在两个后端之间切换时，Google 看到的浏览器版本从 158 退到 152、TLS 握手也不同，这可能有关，但 P1 时只用 Camoufox 也出现过导入约 2.5 小时后 Cookie 失效，所以不能确定。

## 已知差异

- **网络层**：普通 libcurl，TLS 和 HTTP/2 特征不是 Firefox 的。详见网络指纹报告。
- **版本**：SpiderMonkey 后端声称 Firefox 152，Camoufox 是 158，同一账号切换后端时在 Google 看来是版本倒退。P4 升到最新版并与 Camoufox 同步升级。
- **引导集合**：只做最小集合（页面、`GetLoggingContext`、`GetAiStudioBenefitTier`、`Waa/Create`、解释器）。真实页面加载时还有 `GetUserPreferences`、`GenerateAccessToken`、`ListPromos`、`ListIncidentsHistory`、`GetGdpProfileAndBenefits` 等 RPC，以及字体、脚本、统计、头像和账号 Cookie 刷新等子资源请求。
- **正文模板**：`GenerateContent` 的正文由我们自己构造，没有和 Camoufox 截到的模板逐项对比。
- **文本解码**：页面只支持 UTF-8、UTF-16LE/BE 和 windows-1252。

## 遗留事项（P4）

- **对照实验**：实现 Firefox 网络指纹、升到与 Camoufox 相同的版本之后再做。先在较长时间里比较格 1 和格 4 这两个自洽的组合；只有需要单独区分 proof 与发送方式时才跑混合格，并且先解决上面说的版本和页面上下文一致问题。
- **第二层引导**：如果出现拒绝，再补齐页面的其他 RPC 和子资源。
- **模板对比**：把 Camoufox 截到的正文模板骨架与我们构造的逐项对比。
- **带账号的隐藏测试**：`Waa/Create` 要账号 Cookie，`[.waa-live]` 没有写。
- **构建配方**：`tools/spidermonkey/build-windows.cmd` 还没有在全新的工作目录里从头跑过；Linux、Android 的静态库和四平台 CI 在 P4 做。
- **Code 7 的处理**：仍然缺少数据，`retry_policy.hpp` 的校准继续等待。
