# Camoufox 原型（P1）验证

日期：2026-11-06。[Firefox 后端迁移计划](../firefox-backends.md) 的 P1：在 `poc/firefox-backends` 分支上用 C++ 实现基于 WebDriver BiDi 的 Camoufox 后端，在 Windows 上用两个测试账号验证。本文记录当时的结果，后续行为以代码和功能文档为准。

## 结论

| 完成条件 | 结果 |
|---|---|
| 在真实账号上可用 | 通过。引导、三种接口的流式与非流式、多轮、函数调用、搜索、超过 1 MiB 的内联图片、Cookie 写回、账号切换、浏览器崩溃恢复、代理中转均正常 |
| AI Studio 接受 Firefox 身份 | 初步通过。约 1.5 小时内两个账号共 48 次生成，没有 403、跳转登录页或冷却；观察时间还短，P2 期间继续观察 |
| `bridge.js` 在 Firefox 上可用 | 通过，没有修改 bridge。BYOB 读取器可用，`input.performActions` 发出的按键在页面上为可信事件 |
| Windows 的显示模式 | 用无界面模式，理由见下文“显示模式” |

## 环境

- Windows 11 x64 开发机，MSVC Release 构建。
- Camoufox `v158.0.1-beta.41`（Windows x64 官方发布包，SHA-256 与 GitHub 记录一致）；身份由 PyPI `camoufox` 0.5.9 生成，共 16 份，未加载 uBlock Origin。
- 独立的配置与数据目录，只导入测试账号；经本机 HTTP 代理访问 Google。
- 所有生成请求只使用 `gemini-3.1-flash-lite`。

## 实现概要

- `browser/bidi.hpp`：BiDi 客户端。读取线程负责 WebSocket，回复按 id 分发；`browsingContext.contextDestroyed` 让等待该页面的调用立即失败。
- `src/camoufox.cpp`：`CamoufoxGateway`，流程与 `BrowserGateway` 对应：每个账号一个 user context，Cookie 经 `storage.setCookie` 注入，bridge 与 localStorage 恢复作为 preload 脚本，身份等待、引导、Cookie 写回和生成转发沿用现有逻辑。
- 每个账号按指纹种子固定使用身份池中的一份，叠加账号保存的时区和语言；身份变化时重启浏览器。
- 浏览器使用私有资料目录和 user.js，身份通过 `CAMOU_CONFIG_*` 传入，输出写入 `logs/camoufox-*.log`，BiDi 端点从输出中读取。
- `browser_backend: "camoufox"` 开启该后端；可执行文件必须是 Camoufox，身份池必须存在。

## 离线与运行时检查

- 全部单元测试通过；Draft PR 的 CI（Windows、Debian、Ubuntu 含 sanitizers、Termux、容器镜像）全部通过。
- 隐藏的运行时测试 `[.camoufox-runtime]` 只打开本地页面，在无界面和有界面模式下都通过：bridge preload 已注入，`navigator.webdriver === false`，BYOB 读取器可用，Ctrl+Enter 为 `isTrusted` 的 keydown；进程与资料目录随后释放，日志保留。

## 真实账号验证

| 项目 | 结果 |
|---|---|
| Cookie 导入 | 两个账号（authuser 0、1）经 Camoufox 验证后导入，用时 39 秒；之后的模型发现（ListModels）成功。此前的第一次导入以 `identity_unverified` 失败，原因见“发现的问题” |
| 引导 | 首次请求捕获到模板和快照；服务重启后的首个请求（含启动浏览器）约 15 秒 |
| OpenAI / Gemini / Responses | 非流式、流式都正常，多轮对话正确 |
| 函数调用 | 往返正常 |
| 搜索 | 在 `gemini-3.1-flash-lite` 上用 Gemini 原生 `googleSearch` 得到训练数据之后的信息；搜索来源元数据按协议本来就不解码 |
| 内联图片 | 1.5 MB（base64 后）图片经分块上传，答案正确，用时 19.5 秒。经同一代理单独上传 1.5 MB 约需 16 秒，浏览器侧开销约 2 秒 |
| Cookie 写回 | `cookieRefresh` 为 `saved`；重启服务后用写回的 Cookie 直接生成成功 |
| A、B 交替 | 切换账号时浏览器换用对应身份（第 14 号与第 3 号）重启，每次切换 14–16 秒 |
| 结束浏览器进程 | 下一个请求第一次尝试就自动重启浏览器并成功，用时 14.6 秒 |
| 代理中转 | `browser_proxy_relay` 开启后，user.js 指向本机中转端口，生成正常 |
| 页面内检查 | AI Studio 页面上：`navigator.webdriver` 为 false；UA 与 `platform`（Win32）一致；时区和语言与账号指纹一致；`visibilityState` 为 visible；BYOB 可用 |
| 连续使用 | 无界面 20 个、有界面 10 个请求（每 30 秒一个，三种接口、流式与非流式交替）全部成功，中位耗时 1.6 秒；全程 48 次生成无错误 |

## 显示模式

在 AI Studio 页面空闲与连续请求时统计 Camoufox 全部进程（每种模式 5 个进程）：

| | 无界面 | 有界面 |
|---|---|---|
| 引导完成后 | 工作集 723 MiB，私有 586 MiB | 工作集 799 MiB，私有 886 MiB |
| 空闲 5 分钟 | 工作集 737 MiB，私有 590 MiB，CPU 约单核 1.8% | 工作集 776 MiB，私有 854 MiB，CPU 约单核 1.6% |
| 连续请求时工作集峰值 | 759 MiB | 799 MiB |
| 生成 | 20/20 | 10/10 |
| 窗口 | 无 | 有窗口；测量时桌面处于锁屏，没能观察是否抢焦点 |

无界面模式下，主进程约 427 MiB，AI Studio 页面进程约 197 MiB。

**决定：Windows 上用无界面模式。** 两种模式功能相同，但无界面模式私有内存少约 270–300 MiB，也不会在桌面上弹出窗口。有界面模式还会让 `devicePixelRatio` 跟随本机的显示缩放（本机为 1.5），而身份池是按无界面模式生成的。有界面模式只留作排查登录问题时使用。

CloakBrowser 的对照数据本阶段不采集：测过 Camoufox 的账号在原型期间不切回 CloakBrowser。

## 发现的问题与修正

- **页面跳转被误判为浏览器断开**：Firefox 在跳转打断脚本时报 `unknown error`，消息中包含 destroyed 或 discarded。BiDi 客户端原先把这类错误记为 `browser_disconnected`，身份等待会因正常重定向而失败。现在只有找不到页面或 user context 时才算断开；CDP 对同类情况报的是 `browser_incompatible`，身份等待会重试。
- **浏览器界面语言固定为 en-US**：身份池的 prefs 把 `intl.locale.requested` 固定为生成器默认值。Camoufox 自带语言包，现在改为跟随账号语言，与本地化版 Firefox 一致。
- **下载过慢且会中断**：GitHub 发布存储的单个连接约 0.2 MB/s，并两次在中途无报错地结束。开发脚本改为 8 段并行范围下载，每段可续传，最后仍校验大小和 SHA-256。
- **日志来源缺少 Camoufox 日志**：管理接口原先只列出 `browser-*.log`，现在也列出 `camoufox-*.log`。
- **AI Studio 条款对话框导致身份验证失败**：AI Studio 有时会弹出“欢迎使用 AI Studio”对话框，要求点“继续”同意《Gemini API 附加服务条款》。对话框打开时页面背景被设为 `aria-hidden`，bridge 找不到账号头像，身份等待 60 秒后报 `identity_unverified`。bridge 为了不替账号同意条款，按设计不会点“继续”。观察到的一次由服务自动重建会话后成功（对话框没有再出现）；第一次导入失败很可能也是这个原因，但当时没有现场记录。这与浏览器种类无关，CloakBrowser 遇到同样的对话框也会失败。
- **测试账号登录失效**：验证结束约 2.5 小时后再次启动服务，两个账号在身份验证阶段都停在登录页（401 `session_expired`），服务按策略记为登录失效并冷却账号。已确认原因是导入的 Cookie 失效，需要重新导入。
- **原型调试记录**：设置 `AIS2API_CAMOUFOX_DEBUG` 后，身份验证失败或成功时会把页面状态和截图写入本机数据目录的 `logs/`，上面的对话框就是这样定位的。

## 遗留事项

- **条款对话框**：留到 P2 讨论 bridge 是否在模态框打开时仍读取账号身份（会同时影响 Chromium 路径）。
- **残留资料目录**：服务被强制结束后，`runtime/camoufox-*` 资料目录会残留，与 CloakBrowser 的 `browser-*` 行为相同，可在启动时清理。
- **浏览器日志噪音**：Camoufox 输出中有大量内部脚本报错（juggler 的 `SimpleChannel` 重复声明、BiDi 内部的 WeakMap 错误），不影响功能，但会占用日志。失败的那次导入日志末尾还有一条 AI Studio 主脚本 `Script terminated by timeout`，是否只是关闭页面时的副作用未确认。
- **观察时间**：AI Studio 对 Firefox 身份的接受情况在 P2 期间继续观察。
- **未覆盖的平台**：Linux、Termux 不在 P1 范围。字体约占解压后 2.5 GB 中的 2.1 GB，裁剪在 P2 处理。
