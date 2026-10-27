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

1. 关闭可能出现的弹窗，但只点击 Close、Dismiss、Got it、No thanks、Not now、Maybe later、Skip、Cancel 一类按钮或发送 Escape；**从不**勾选复选框或点击同意/接受/继续。需要同意条款的弹窗保持原样，由用户自行处理。
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

## Build App 上游（实验性）

`upstream` 设为 `build_app` 后，生成不再回放聊天页的私有 RPC，而是在 AI Studio Build App 的预览框架内直接调用公共 Gemini API（`generativelanguage.googleapis.com`），由 AI Studio 为该框架的请求补全账号凭据。工作原理参考了 [AIStudioToAPI](https://github.com/iBUHub/AIStudioToAPI)；本项目的实现独立编写，不需要在 App 里粘贴任何代码。

**准备页面**

1. 浏览器进程、隐身上下文、指纹、Cookie 注入与身份核对和默认上游相同：先在聊天页确认邮箱。
2. 移除聊天页桥接脚本的新文档注入（它会拦截 AI Studio 自己的 RPC），然后以账号的 `authuser` 打开 `build_app_url`。
3. 每 0.5 秒通过 `Target.getTargets` 查找同一上下文中主机名以 `.usercontent.goog` 结尾的 HTTPS iframe 目标，最多 60 秒；找到后以扁平化会话附加，并在框架主世界执行内嵌的 `browser/app_bridge.js`。框架是跨站 iframe，只有开启站点隔离才会成为独立目标，因此该上游要求 `browser_site_isolation`。
4. 等待期间检查顶层页面：跳转登录报 `401 session_expired`，地区限制报 `403 region_blocked`，出现 “Continue to the app” 确认时报 `409 build_app_consent_required`。确认按钮**从不**点击，这与引导阶段只关闭弹窗的规则一致。
5. 超时仍没有可用框架报 `browser_incompatible`，由重启逻辑接管。之后每次请求先检查框架中的桥接是否仍在，框架重载或替换时重新附加。

**每次请求**

- 请求体是规范化后的 Gemini 请求本身，不经过线路编码、BotGuard 快照、SAPISIDHASH 签名或会员等级查询。设置了 `thinkingLevel` 时补上 `includeThoughts: true`（与默认上游一样返回思考），并舍弃同时存在的 `thinkingBudget`（公共 API 不允许两者并存）。
- 模型 id 必须是单个路径段（字母数字开头，只含字母数字、`.`、`_`、`-`），否则返回 `400 invalid_argument`，不会发出请求。
- 请求体按 512 KiB 分块上传，由框架以 `POST .../v1beta/models/<id>:streamGenerateContent?alt=sse` 发出，不附带任何密钥或授权头。非流式请求同样走流式接口，由服务端聚合。
- 响应沿用逐块确认的转发：支持字节流时使用 32 KiB 的 BYOB 读取；AI Studio 自行构造的响应若只提供默认读取器，则把每块切成不超过 32 KiB 的片段逐片转发。
- C++ 增量解析 SSE，单个事件受 `limits.object_bytes` 限制。每个 `data:` 负载都是一个 Gemini 响应对象，直接交给下游，与默认上游解码后的事件格式相同；负载中的 `error` 信封和非 200 响应（保留最多 8 KiB）按与默认上游相同的规则分类，`Retry-After` 同样生效，因此重试、冷却与调度不变。
- 模型列表改为在框架中调用 `GET /v1beta/models`（每页 1000 条，最多 10 页，每页 10 秒超时、1 MiB 上限），缓存规则不变。
- 诊断中的 `upstream` 显示当前上游，`appFrame` 表示是否已附加预览框架，`appBridgeSha256` 是内嵌 App 桥接脚本的摘要。

**限制**

- 账号探测与 Cookie 导入仍在聊天页进行；驻留页面正显示 Build App 时会重建该账号的上下文，下一次生成再重新打开 App。
- 所有账号共用一个 `build_app_url`。对不拥有该 App 的账号，AI Studio 可能要求确认，这些请求会失败但不会冷却账号。
- 公共 API 的配额、可用模型和错误格式与私有 RPC 不同，尚未经过真实账号验证。

## 验证边界

本地单元测试无法证明上游页面钩子、Cookie、地区可用性或 Android 后台存活。真实 Google 与 Termux 环境的结果见 [验证记录](validation.md)。
