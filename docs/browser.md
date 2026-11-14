# 浏览器网关

本文说明 ais2api 如何驱动浏览器访问 AI Studio：进程与上下文管理、指纹、Cookie 写回、页面桥接、请求签名、流式传输，以及失败重试与账号调度规则。

## 架构

C++ 进程直接启动配置的 Chromium / CloakBrowser 可执行文件；在 Termux 上，这个可执行文件是 `setup` 生成的启动器，会进入项目专属的 proot 环境。整个链路不依赖 Python、Node、Playwright 或外部 CDP 服务。

- 每次启动使用随机的私有资料目录，只读取其中的 `DevToolsActivePort`。
- 通过 Boost.Beast 连接本机回环地址上的浏览器 WebSocket，创建一个隐身 BrowserContext，写入 storage state 中的 Cookie，在该上下文中创建页面并以扁平化 CDP 会话附加。
- CDP 连接由专用线程持有，持续读取所有消息并按 id 分发回复；无关事件读到即丢弃。单次调用取消或超时只放弃该调用，不会断开浏览器连接。
- 切换账号时先销毁旧上下文。
- Windows 上所有子进程属于“关闭即终止”的 Job Object；POSIX 上浏览器拥有独立进程组，关闭时只针对该进程组。
- 关闭后删除运行时资料目录；导入的凭据保留在数据库中。
- 页面会话启用 `Inspector`，渲染进程崩溃时进行中的调用立即以 `browser_disconnected` 失败，由重启逻辑接管。
- 磁盘缓存上限 64 MB；`data/logs/browser-*.log` 只保留最近 8 份。
- Cookie 与页面快照从不写入诊断日志。

默认只有一个浏览器进程、一个驻留账号，同一时间处理一个生成请求，最多排队 8 个（`limits.queue_size`）。取消贯穿 HTTP、原生层和浏览器读取。

## 浏览器指纹

每个账号拥有永久固定的浏览器指纹：

- 账号首次保存（命令行导入、管理 API 导入或 Cookie 导入验证通过）时分配一个 10000–99999 之间的 CloakBrowser 种子；重新导入和凭据更新保留原种子，旧数据库中的账号在首次打开时补发。
- 启动参数包括 `--fingerprint=<seed>`、`--fingerprint-platform`（macOS 主机上为 `macos`，其他情况一律为 `windows`）、`--fingerprint-timezone` 与 `--fingerprint-locale`。
- 时区与语言默认跟随本机区域设置，也可以按账号覆盖（IANA 时区名与 BCP 47 语言标签）。检测来源：Windows 为用户区域和系统时区；POSIX 为 `TZ`、`/etc/localtime`、`/etc/timezone` 与 `LC_ALL` / `LC_MESSAGES` / `LANG`；Android/Termux 为系统属性。检测失败时回退为 `UTC` 与 `en-US`。CloakBrowser 把语言应用到 `navigator.language(s)`、`Intl` 与 `Accept-Language`。
- `--force-webrtc-ip-handling-policy=disable_non_proxied_udp` 阻止 WebRTC 在代理之外发送 UDP，避免 ICE 候选暴露真实地址。
- 其他 Chromium 构建会忽略指纹参数。没有种子时 CloakBrowser 每次启动随机选择设备，没有时区/语言时呈现 UTC + en-US。

CloakBrowser 的指纹作用于整个进程，因此切换到指纹不同的账号时会重启浏览器；指纹相同的账号复用同一进程。管理 API 与 WebUI 可以查看指纹、覆盖时区和语言，或重新生成种子（Google 会把它视为新设备）；修改在该账号下次使用时生效。诊断信息中的 `fingerprint` 字段显示当前运行的身份。

### 按账号选择后端

每个账号记住自己的浏览器后端：

| 取值 | 身份 | 使用的浏览器 |
|---|---|---|
| `cloakbrowser` | Chrome | CloakBrowser（**已弃用**，将在以后的版本中移除） |
| `auto` | Firefox | 由 ais2api 选择 Firefox 后端，目前即 Camoufox |
| `camoufox` | Firefox | Camoufox |
| `spidermonkey` | Firefox | 不启动浏览器（**实验性**，只在开启 `AIS2API_SPIDERMONKEY` 编译的版本中提供，见下文） |

- **CloakBrowser 已弃用**：
  - 新建的配置默认使用 `camoufox`；已有的配置和账号保持原样，不会被自动换成另一种身份。
  - 默认后端或任何账号仍是 `cloakbrowser` 时，`doctor`、`GET /status`、`setup` 的结果和诊断报告都带有 `deprecations`（代号 `cloakbrowser_backend`，`default_backend` 和 `accounts` 说明是哪一处在用），服务启动时写一条 `backend_deprecated` 日志，WebUI 的账号页和诊断页显示提示。
  - 迁移：把 `browser_backend` 改为 `camoufox`，再运行 `ais2api accounts backend --all auto --yes` 把账号换成 Firefox 身份。Google 会把换了身份的账号看作一台新设备。
- 新账号保存时取配置项 `browser_backend` 对应的值：`cloakbrowser` 保存为 `cloakbrowser`，`camoufox` 保存为 `auto`。升级前已有的账号在第一次启动时保存为当时实际使用的后端。之后修改 `browser_backend` 只影响新账号和 Cookie 导入的验证，不会把已有账号换成另一种身份。
- 在 CloakBrowser 与 Firefox 之间切换必须显式操作并确认（命令行 `accounts backend`、管理 API 或 WebUI 的指纹对话框），因为 Google 会把新身份看作一台新设备。Firefox 后端之间可以直接切换，身份不变。两种身份都由同一个种子确定，切回时得到原来的身份。
- 同一时间只有一个浏览器驻留：轮到另一个后端的账号时，先关闭当前浏览器，再启动另一个。
- `setup` 和 `doctor` 按“需要的后端”工作：默认后端加上已保存账号用到的后端。诊断信息的 `backend` 是当前运行的后端，`default_backend` 是默认后端。
- Linux 上的 Camoufox 在 Xvfb 虚拟显示中以有界面模式运行（`headless=true` 时），字体使用 Camoufox 自带的 Linux 字体组，见 [Camoufox 后端](platforms.md#camoufox-后端)。
- Termux 上的 Camoufox 与 Xvfb 都运行在 proot 容器 `ais2api-browser` 中，见 [浏览器容器](platforms.md#浏览器容器)。

### SpiderMonkey 后端（实验性原型）

SpiderMonkey 后端不启动浏览器：ais2api 用 HTTP 加载 AI Studio 页面并发出页面自己的请求，Google 的 WAA（BotGuard）程序在进程内嵌的 SpiderMonkey 152 中运行，生成每次请求的 proof。它是 [Firefox 后端迁移计划](firefox-backends.md) P3 阶段的原型，只建议用于测试账号。

- **构建**：默认的构建和发布版不包含它。用 [`tools/spidermonkey`](../tools/spidermonkey/README.md) 的配方编出 SpiderMonkey 静态库，再以 `-DAIS2API_SPIDERMONKEY=ON -DAIS2API_SPIDERMONKEY_DIR=<产物目录>` 构建；可执行文件约大 32 MB。这样的构建默认同时开启 `AIS2API_CURL_IMPERSONATE`（见下文“网络层”），程序旁边多一个 `libcurl-impersonate.dll`，目前只支持 Windows x64。不包含它的版本中，保存为 `spidermonkey` 的账号由 Camoufox 服务。
- **身份**：沿用账号在 Camoufox 后端下生成的 Firefox 身份（navigator、屏幕、窗口、语言、时区），User-Agent 改为声称 Firefox 152。还没有 Firefox 身份的账号要先用 Camoufox 服务一次，否则返回 503 `browser_not_configured`。同一账号在 Camoufox（158）和 SpiderMonkey（152）之间切换，在 Google 看来是浏览器版本倒退。
- **网络层**：用 [curl-impersonate](https://github.com/lexiforest/curl-impersonate)（lexiforest 分支）的 libcurl，CMake 下载锁定版本并校验 SHA-256（`scripts/curl-impersonate.cmake`）。
  - **TLS 与 HTTP/2**：按实测的 Firefox（Camoufox 158）逐项设置，包括加密套件、扩展顺序、密钥交换组、签名算法、ECH GREASE、HTTP/2 SETTINGS、连接级 WINDOW_UPDATE、伪首部顺序，以及每个请求 HEADERS 帧上的权重（页面文档 42，其余 22）；不使用 curl-impersonate 自带的 Firefox 预设。
  - **请求头**：按 Firefox 的名称和顺序发出，每个 Cookie 单独一个头。
  - **离线测试**：`ais2api_tests "[firefox-http][fingerprint]"` 在本地抓取 ClientHello 和 HTTP/2 帧，与实测值逐项比较，其中包括 JA4。
  - **已知差异**：
    - Firefox 在每个 HEADERS 之后发一个流级 WINDOW_UPDATE，预编译的 curl-impersonate 不发；
    - BoringSSL 与 Firefox 的 NSS 在细节上可能不同；
    - 不经代理时 Firefox 会改用 HTTP/3，这个后端始终用 HTTP/2。
  - **单独开启**：`AIS2API_CURL_IMPERSONATE` 也可以不带 SpiderMonkey 开启，便于测试网络层。开启后整个程序（包括 `setup` 的下载）都用这份 libcurl，证书用系统的证书库验证。
- **流程**：第一次请求时加载页面（跳转到登录页时返回 401 `session_expired`），调用 `GetLoggingContext` 和 `GetAiStudioBenefitTier`，再调用 `Waa/Create`、取得解释器（按摘要缓存在 `<data_dir>/runtime/waa-interpreters/`）并启动程序。之后每次生成先在宿主页面里填入提示词并点击 Run 取得 proof，再发送 `GenerateContent`，约 2.2 秒后像页面一样发出一次 `CountTokens`。程序运行 12 小时后在下一次请求时按页面的方式续期。`GetLoggingContext` 返回 `[7,"Region not supported."]` 时报 403 `region_blocked`。
- **其他接口**：`models` 直接调用 `ListModels`；`probe` 只加载页面，页面数据中有账号邮箱时核对它；Cookie 导入始终由默认后端验证。`reset_session` 丢弃页面和程序、保留连接；空闲释放时关闭全部。
- **诊断**：`backend` 为 `spidermonkey`，另有 `pageLoaded`、`vm`（运行时长、解释器摘要前 12 位）、`vmsStarted`、`host`（嵌入的宿主脚本与对象结构表的 SHA-256、SpiderMonkey 版本）、`claimedFirefox`、`network`（用 curl-impersonate 时为 `curl-impersonate`，否则为 `libcurl`）和 `networkStack`（libcurl、TLS 库和 nghttp2 的版本）。

## Cookie 写回

Google 会在页面使用过程中轮换会话 Cookie（例如 `__Secure-1PSIDTS`）。新账号上下文通过身份核对后写回一次，之后每次生成成功最多每 5 分钟写回一次：

- 浏览器中的 Google Cookie 替换存储中的 Google Cookie 集合；其他域名的 Cookie，以及浏览器不导出的分区 Cookie 保持不变。
- 缺少 SAPISID 系列或 SID 系列 Cookie 的导出结果不会保存。
- 写回以“比较后交换”的方式进行，基准是构建该上下文时使用的凭据，因此同时发生的导入或删除总是优先；身份、过期状态、冷却与配额计数不受影响。
- 写回后的状态成为上下文的新基准，不会触发重建。
- 诊断中的 `cookieRefresh` 显示最近一次结果（`saved`、`unchanged`、`failed` 或 `not_attempted`），从不记录 Cookie 值。

注入 Cookie 时会剔除 `OSID`、`__Secure-OSID`、`SIDCC`、`__Secure-1PSIDCC`、`__Secure-3PSIDCC`（如果全部被剔除则原样注入，与参考项目一致）。剔除只发生在注入时，不影响存储和写回比较。

## 页面桥接

桥接脚本是本项目独立编写的 JavaScript（`browser/bridge.js`），构建时由 CMake 以字节形式嵌入可执行文件：

- 生成始终使用内嵌副本，旁边的旧脚本或开发源码目录不能覆盖它；运行包中保留的 `bin/browser/bridge.js` 仅作参考。
- 诊断显示 `bridgeSource=embedded`、内嵌脚本的 SHA-256 与当前适配阶段；浏览器失败时只记录这些安全字段，不记录原始 JavaScript 异常或页面数据。

### 引导

桥接脚本发现页面中实时的 MakerSuite 快照适配器，观察其真实的服务参数，并从页面捕获一个真实的 GenerateContent 请求作为模板：

1. 关闭可能出现的弹窗，但只点击 Close、Dismiss、Got it、No thanks、Not now、Maybe later、Skip、Cancel 一类按钮或发送 Escape；**从不**勾选复选框或点击同意/接受/继续，唯一的例外是下面的欢迎对话框。
   - **欢迎对话框**：AI Studio 的“欢迎使用 AI Studio”对话框在新会话中会反复出现，账号同意过也一样，所以桥接脚本会点击它的按钮继续。点击即表示同意对话框中链接的《Gemini API 附加服务条款》和《Google 隐私权政策》。识别只看结构，不看语言：对话框链接了 Google 的条款页面，只有一个可用的按钮，没有复选框或输入框。
   - **其他条款弹窗**：有多个按钮、有复选框，或按钮不可用的，保持原样，由用户自行处理（`terms_required`）。点击 10 秒后仍未关闭的欢迎对话框也按此处理。
2. 插入一段很短的引导提示并点击 Run；找不到 Run 按钮时在输入框发送 Ctrl+Enter（macOS 指纹使用 Meta），1 秒内仍未捕获到模板时再通过 CDP 发送一次可信按键。
3. 捕获到的引导生成请求和保存提示词请求都会在本地返回空响应，因此引导不会真正发出生成，也不会保存聊天记录。
4. 尽量启用 Temporary chat，完成或失败后停止探针并恢复输入框。

fetch 与 XHR 两种捕获方式都支持。页面内部结构变化时报告 `browser_incompatible`。项目不提供虚构的请求模板或快照回退。

### 每次请求

- 请求前重新调用页面的快照服务，基于内容哈希获取新的防滥用票据（BotGuard 快照），不复用旧票据。快照失败、返回空值或单次超过 9 秒时重试，最多 3 次。这个票据与线路编解码器处理的部件级 `thoughtSignature` 是两种不同的数据。
- 内容哈希基于最终编码的 contents 计算（包括展开的工具结果附件）：每个文本或内联数据贡献对应值，代码和工具部件贡献空串，以单个空格连接。
- 请求体按 512 KiB 分块（不切开 UTF-8 字符）上传到页面，在页面中转为 Blob 后作为 fetch 的 body 发出，避免在 JS 堆中保留多份副本；请求体 ≥ 1 MiB 时，生成结束后触发一次垃圾回收。

## 请求签名与会员等级

生成与模型发现使用所选页面的 CDP 签名 Cookie 缓存（包括 HttpOnly Cookie），以新时间戳重新计算 SAPISIDHASH、SAPISID1PHASH 与 SAPISID3PHASH：

- SAPISID 优先于 `__Secure-1PAPISID`；缺少 1P/3P 值时回退为主值。
- 缓存在签名 Cookie 缺失或过期、以及每 60 秒刷新一次，账号上下文销毁或浏览器关闭时清空。有效的缓存可以在临时刷新失败时继续使用；取消会正常传播。
- 无法本地签名时，只回退使用该页面捕获的 Authorization；两者都没有则本地失败。
- 每次发送都填写已验证账号的槽位（`X-Goog-AuthUser`），并拒绝捕获头中冲突的槽位。
- 签名 Cookie 与请求头只保存在内存中，不会出现在诊断或日志里。

生成与模型发现还会通过实时 RPC 端点和当前账号授权查询 GetAiStudioBenefitTier：超时 8 秒，响应上限 8 KiB，每个页面缓存 10 分钟，失败后 30 秒再试。捕获的等级请求头会被移除；只有确认的 plus/pro/ultra 才添加 TIER0/TIER1/TIER2，free 与未知等级不加。每次生成记录发送时使用的等级，后续缓存过期不会改变错误分类。等级未知时的 403 直接结束请求，不会让健康账号冷却或轮换。

## 流式传输

- 页面使用 fetch 的 BYOB 读取器，每次使用新的 32 KiB 缓冲区，同一时间只允许一个 base64 数据块待处理。
- 桥接方法 `next(sequence, waitMs)` 同时确认上一块并在页面中等待下一块或结束（最多 250 ms），不做轮询休眠。
- C++ 解码数据块并增量解析深度为 3 的线路记录。下游消费完成后事件接收端才返回，此时 C++ 才确认该序号并允许浏览器继续读取。整个过程从不构建或保存完整的原始响应。
- 不支持 BYOB 的实现会明确失败。
- 取消、生成时限和空闲时限会中止页面 fetch；CDP 操作也有超时和取消检查。流式请求使用空闲时限，只要数据持续到达就可以超过 `generation_seconds`；非流式请求还受总时限约束。
- 状态码为 200 但内容为 `google.rpc.Status` 错误信封的响应会在解码数据块前分类：`RESOURCE_EXHAUSTED`、`PERMISSION_DENIED`、`UNAUTHENTICATED` 分别映射为 429、403、401。
- 内联媒体和非流式响应有独立的对象上限，但这并不意味着 C++ 能精确限制整个 Chromium 进程的内存占用。

## 失败重试与冷却

尚未向客户端输出任何内容的失败尝试，按统一策略（`include/ais2api/retry_policy.hpp`）处理，总次数受 `limits.max_attempts` 限制：

| 情况 | 处理 |
|---|---|
| 可识别的 GenerateContent 路由 404 | 在原账号已验证的上下文中重建请求体和快照后重试。 |
| 首次尝试返回空的 204 | 清除模板后在原账号重试一次（不占用下面的“重建/重启”机会）；再次 204 则返回错误。 |
| 普通 404 和其他客户端错误、地区限制、生成/空闲超时 | 直接返回，不影响账号状态。 |
| 上游 5xx、传输不完整 | 短暂等待（0.5 秒、1 秒……）后在原账号重试，不冷却。 |
| 403（BotGuard 快照过期或不匹配也表现为 PERMISSION_DENIED） | 先就地重建该账号的页面会话（新上下文、Cookie、页面、BotGuard 与请求模板，浏览器进程保留）再重试原账号。再次 403 才让该模型冷却 5 分钟；等级未知时的 403 直接返回，不冷却。 |
| RPC UNAUTHENTICATED、身份未确认、签名失败 | 同样先重建一次；再次失败则整个账号进入 5 分钟 `auth` 冷却。 |
| AI Studio 条款对话框（`terms_required`） | 欢迎对话框自动继续，不算失败。其他条款对话框先重建一次；仍出现则整个账号冷却 1 小时，认证放行不适用。 |
| Cookie 背后是另一个 Google 账号（`identity_mismatch`） | 直接进入 5 分钟 `auth` 冷却。 |
| 在全新会话上仍跳转登录页，或账号没有 Cookie | 标记账号已过期。 |
| 浏览器不兼容、断开、CDP 超时、启动失败 | 重启浏览器一次后重试原账号，不冷却。 |
| 代理错误（`proxy_failed`） | 直接失败，不冷却账号。 |
| 无法连接 Google（`network_failed`） | 按 5xx 处理，在原账号重试。 |

补充规则：

- 每个请求最多进行一次会话重建或浏览器重启。切换到其他账号总是开启新会话；只有下一个账号的指纹与当前进程不同时才重启浏览器。
- 响应必须带有非 unspecified 的结束原因，并且传输帧完整。已经输出给客户端的内容在失败后不会重新生成。
- **配额冷却**按模型计算：分钟级限额至少等待 60 秒且不增加每日计数；其他 429 依次等待 60/60/300/300/600 秒，计数在太平洋时间午夜重置，成功后保留递增计数。`Retry-After` 可以把等待延长到最多 1 天。
- 替换凭据不会清除未到期的配额冷却。显式的全局冷却和管理端的重置操作保持原有含义。
- 非 200 响应只保留状态码、解析出的 `Retry-After` 和最多 8 KiB 的错误文本；格式错误的流分类最多保留 16 KiB 前缀。这些错误文本不会写入日志或返回给客户端。

## 账号调度

为请求选择账号时按以下顺序：

1. **当前账号**：最近一次生成成功的账号，也就是浏览器中驻留的会话。
2. **首选账号**：通过 `accounts select`、WebUI“设为首选”或轮换设置的账号。
3. 该模型最近成功使用的账号。
4. 导入顺序。

因此交替请求不同模型不会频繁换号；只有当前账号对某模型处于冷却时才切换，切换后的账号成为新的当前账号，**不会自动切回首选账号**（与参考项目一致）。设为首选和轮换操作会同时设置当前账号，立即生效。

当所有账号都只因 `auth` 冷却而不可用时，生成请求会放行其中一个账号尝试一次，以便在 Cookie 恢复后自动恢复服务；同一冷却周期内每个账号最多放行一次，失败只延长冷却。配额、权限、已过期和已禁用的账号不会被放行。生成成功会清除该账号的 `auth` 冷却。就绪检查和模型发现不会消耗放行机会。

## 探测与模型发现

- **探测**检查页面就绪、登录与地区跳转，以及提供时的期望邮箱。邮箱缺失或不明确时不能满足期望邮箱条件。HTTP 和浏览器就绪不代表模型生成已经成功。
- **模型发现**在浏览器中使用实时页面请求捕获的凭据调用真实的 ListModels RPC。只有返回的模型记录才以 `live` 来源公布；成功结果按账号缓存 10 分钟（`cache`）。没有可用账号或发现失败时，可以返回 `configured_models`（`configured`）。不会虚构模型目录或 API Key。

## 空闲回收与代理

- `browser_idle_seconds` 大于 0 时，工作线程在队列空闲到期后先写回 Cookie 再关闭浏览器，诊断状态为 `idle_closed`，日志事件为 `browser_idle_released`；之后在有新任务前不会再次尝试。
- `browser_proxy_relay` 开启时，浏览器只连接本机转发代理，由 ais2api 向上游 HTTP 或 SOCKS5 代理认证。隧道建立阶段超时 20 秒，最多 128 条隧道。诊断中的 `proxy.mode`（`none`、`direct` 或 `relay`）与 `proxy.relayError` 显示当前状态。配置方法见 [配置参考](configuration.md#使用带账号密码的代理)。
- 浏览器启动参数沿用桌面版的配置（单 GPU 进程、SwiftShader 等），以便 CloakBrowser 在容器内稳定运行。仅在无界面模式下额外加入低内存渲染参数（V8 堆上限 128 MiB、低端设备模式等）；显示窗口时保持正常渲染，便于排查登录。Linux 上始终以 `--no-zygote` 启动，并同时加入 `--no-sandbox`（沙箱依赖 zygote），非 root 用户也不例外；Windows 与 macOS 没有 zygote，两者都不加入。`--remote-allow-origins=*` 与关闭站点隔离分别由 `browser_remote_allow_origins`、`browser_site_isolation` 控制，默认不启用。
- 页面导航失败时会检查 `errorText`：代理类错误报 `502 proxy_failed`，无法连接 Google 报 `502 network_failed`，不会再等满超时后冷却账号。

## 验证边界

本地单元测试无法证明上游页面钩子、Cookie、地区可用性或 Android 后台存活。真实 Google 与 Termux 环境的结果见 [验证记录](validation.md)。
