# Firefox 后端迁移计划

状态截至 2026-11-06。本文记录从 CloakBrowser 迁移到 Firefox 系后端的已定决策、分阶段计划和待实测事项。P1 Camoufox 原型已在 Windows 上完成验证，结果见 [原型报告](reports/camoufox-prototype-2026-11-06.md)。

## 背景

- 当前锁定 CloakBrowser `146.0.7680.177.4`。上游从 148 起的版本都是 Pro 构建：GitHub Release 只提供校验文件，浏览器要用许可证密钥从 cloakbrowser.dev 下载，启动、心跳和关闭时都会连接授权服务器。免费密钥只有 1 个并发会话，服务器总是给免费密钥发最新版。
- 新授权方式与本项目冲突：无法锁定 SHA-256；Windows 关闭 Job Object、POSIX 发送 SIGTERM 后很快 SIGKILL，浏览器来不及释放会话，被强制结束的会话约 4 分钟后才自动释放，期间重启浏览器会被拒绝；二进制要求 `~/.cloakbrowser` 可写，与便携目录的约定冲突。
- `146.0.7680.177.5` 是最后一个无需密钥的版本，只有 Windows x64 和 Linux x64。本项目不再升级 CloakBrowser，[v0.1.0](https://github.com/mananekoha114/ais2api/releases/tag/v0.1.0) 是最后一个使用 CloakBrowser 的版本。
- [AIStudio2API](https://github.com/Mag1cFall/AIStudio2API)（MIT）已经在 AI Studio 上实际使用两条 Firefox 路线：在进程内的 SpiderMonkey 中运行 WAA（BotGuard）并直接发送 HTTP 请求，以及通过 WebDriver BiDi 驱动 Camoufox。本计划的 SpiderMonkey 后端以它为参考。

## 目标

| 优先级 | 目标 |
|---|---|
| 第一 | 摆脱 CloakBrowser |
| 必要 | 降低资源占用：内存、浏览器下载体积、Termux 上的 proot |
| 底线 | 不能比现在更容易被风控 |
| 附带 | 多账号并发等其他收益 |

## 总体架构

- **两种 Firefox 后端长期并存**，都实现 `include/ais2api/backend.hpp` 中的 `GenerationBackend`，接入现有的生成主流程（重试、续写、冷却、Cookie 写回）：
  - **Camoufox**：真实浏览器，通过 WebDriver BiDi 驱动（`--remote-debugging-port=0` 启动，从 stderr 读出 `ws://` 地址）。它是风控变化时的缓冲，也是排查 SpiderMonkey 问题的对照组。
  - **SpiderMonkey**：在进程内运行 WAA 生成 proof，用模仿 Firefox 网络指纹的原生 HTTP 发送请求，不启动浏览器。
- **共用身份**：每个账号只有一份 Firefox 身份，两种后端共用，并且声称同一个 Firefox 大版本。
- **后端选择**：
  - 每个账号的后端设置为"自动 / Camoufox / SpiderMonkey"，命令行、管理 API 和 WebUI 的指纹对话框都能修改。
  - "自动"使用默认后端；就地重建会话后仍是 403（Code 7）这类和 WAA 有关的失败时，这次请求换到另一种 Firefox 后端重试，之后该账号 6 小时内留在新后端。
  - 手动指定后端时固定使用，不自动切换。
  - 不会在 CloakBrowser（Chrome 身份）和 Firefox 后端之间自动切换。
  - 新账号的默认后端在 SpiderMonkey 原型有结果后决定。
- **模块边界**：新写的 SpiderMonkey WAA 运行时和 BiDi 客户端做成不依赖 `server`、`config`、`store` 的独立静态库。JS 宿主与对象结构表在构建时由 CMake 嵌入（与 `bridge.js` 相同），诊断信息显示其 SHA-256；运行时目录由调用方传入。这样以后的 Windows 桌面 App 和 Android App 可以直接使用。整个核心是否拆成库，在 App 立项时再定。
- **并发**：第一版主线沿用"只常驻一个账号、请求排队"的调度方式。多账号并发在 SpiderMonkey 后端稳定后单独评估。

## 身份

- **内容**：navigator、屏幕、字体、WebGL、语音、媒体设备、时区与语言。
- **OS**：非 macOS 主机呈现宿主真实的 OS（Linux 主机呈现 Linux）。Camoufox 在 Linux 上伪装 Windows 时 canvas/WebGL 读回随机数据，本身就是特征；是否伪装 Windows 留到 Linux 阶段实测决定。
- **GPU**：按 Camoufox 官方启动器的做法，不带身份启动一次 Camoufox，在本地页面读出 WebGL 厂商与渲染器。读取时的显示模式和首选项与身份启动相同，每个进程只读一次。
  - fpgen 记录过这块 GPU：身份就声称它。
  - 是硬件 GPU 但没有记录：抽一块 GPU，并开启 canvas 读回保护。
  - 软件渲染：抽一块 GPU，不开保护。
  - 宿主 GPU 与生成身份时不同（换了显卡，或数据目录搬到别的机器）：诊断信息给出提示，身份保持不变，直到重新生成。
- **生成方式**：开发版和正式分发版都在本机为每个账号生成，运行时不依赖 Python。
  - 把 [fpgen](https://github.com/scrapfly/fingerprint-generator)（Apache-2.0，贝叶斯网络采样）和 Camoufox 官方 Python 启动器 0.5.9（MIT）中组装身份、校验一致性的 Windows 与 Linux 逻辑移植成 C++（`src/fpgen.cpp`、`src/firefox_identity.cpp`）。字体、语音、媒体设备等目录数据原样取自启动器包，放在 `browser/camoufox/`，嵌入程序。
  - fpgen 模型由 `setup` 下载到 `<data_dir>/runtime/fpgen/<标签>/`，在 `runtime-manifest.json` 中以 SHA-256 锁定，随版本升级更新。没有模型时，网关返回 503 `browser_not_configured`，提示运行 `ais2api setup`。
  - 生成过程是确定的：盐值取本机密钥、账号 ID 与指纹种子的 SHA-256 前 64 位。Cookie 导入验证与账号第一次真正启动因此呈现同一台设备；本机密钥让不同安装抽到的身份互不重复。
  - 身份和指纹种子一起保存在 state 库中。每次启动时按账号语言填入语言、时区和语音（语音来自该语言的语言包），并按浏览器的 `properties.json` 去掉它不读取的配置项。
  - `scripts/camoufox-dev.py check-identities` 用启动器自己的校验和一致性规则检查生成的身份，并与启动器在同一台机器上生成的身份比较分布。
- **生命周期**：
  - Firefox 升级时身份抽到的值保持不变，只更新浏览器标识中的版本号等字段。
  - 只有用户明确操作时才重新生成身份：重新生成指纹（命令行、管理 API、WebUI）会一并清除身份，下次启动按新种子生成。
  - 已有账号从 CloakBrowser 指纹迁移到 Firefox 身份必须显式操作，不在后台自动迁移；迁移在 Google 看来等同于换了一台新设备。
- **已知限制**：参考项目的 SpiderMonkey 宿主中 WebGL 返回 null，canvas 只有 2d 的空壳实现，图形表现无法与 Camoufox 完全一致。共用身份实际覆盖 navigator、屏幕、语言和时区。

## 版本策略

- **跟随最新版**：两种后端始终声称同一个 Firefox 大版本。Firefox 每出一个新大版本，等 Camoufox 在该大版本上的第一个正式版（不是预发布版），Camoufox、SpiderMonkey 与对象结构表三者一起升级，目标 2 周内完成。SpiderMonkey 部分没准备好时，Camoufox 不升大版本；同一大版本内的小版本更新 Camoufox 可以单独跟进。SpiderMonkey 后端出现之前，Camoufox 自由跟随最新版。
- **Camoufox 版本随程序版本更新**：
  - 由 `runtime-manifest.json` 锁定，维护时决定是否升级；
  - 新版程序锁定了新的 Camoufox 时，升级程序后启动脚本自动下载；
  - 锁定版本没变时不下载。
- **回滚**：升级时保留上一个版本，`ais2api browser rollback` 一条命令即可退回。退回后一直保持，直到运行 `browser update`。
- **升级 Camoufox 时的身份检查**：
  1. 更新锁定版本：
     - `runtime-manifest.json` 的 `camoufox` 条目：版本、URL、大小、SHA-256，开发脚本也从这里读取；
     - `scripts/camoufox-dev.py` 中的启动器版本。
     然后运行 `catalog` 更新 `browser/camoufox/`。它按启动器包的 `RECORD` 核对哈希，并检查启动器锁定的 Camoufox 和 fpgen 模型与 `runtime-manifest.json` 一致。
  2. 对比新旧启动器中已移植的逻辑和 `properties.json`。如果与版本相关的输出变了（例如 WebGL 扩展列表、新增或删除的配置项），在 `firefox_identity::launch()` 中为已保存的身份加迁移，不重新生成。
  3. 运行隐藏测试 `[.fpgen-model]`，并用 `check-identities` 与新启动器对比；再运行 `[.camoufox-runtime]`。
  4. 用 `fetch` 下载不裁剪的完整版，运行 `[.camoufox-fonts]`，确认 `setup` 裁剪后的渲染与完整版相同。
- **对象结构表**：自己编写采集工具，在真实浏览器页面中导出与参考项目 `firefox152.json.gz` 相同格式的数据（全局对象自有键及顺序、接口与属性描述、原生函数标记、Window 原型链与取值）。以同版本的 Mozilla 官方 Firefox 为准，按 Windows 和 Linux 分别采集；Camoufox 只用于对比，找出它与官方版不一致之处。每次升级还要人工检查 `dom.js` 中与版本相关的行为。
- **SpiderMonkey 静态库**：放在单独的公开仓库（例如 `mananekoha114/ais2api-spidermonkey`），保存我们的嵌入补丁（第一版来自参考项目针对 152.0.4 的补丁，之后由我们维护），用 GitHub Actions 编译 windows-x64（clang-cl，兼容 MSVC）、linux-x64、linux-arm64、android-arm64（NDK）。主仓库像 `runtime-manifest.json` 一样用 SHA-256 锁定。

## Camoufox 后端

- **平台顺序**：Windows → Linux x64（含 Docker）→ Termux → macOS（可暂缓）。
- **显示模式**：Linux 上用 Xvfb 虚拟显示、以有界面模式运行，`setup` 负责安装 Xvfb。Termux 上用 Linux arm64 版，像现在的 CloakBrowser 一样跑在 proot Ubuntu 中，proot 内同样使用 Xvfb。Windows 上用无界面模式：P1 实测两种模式功能相同，无界面模式私有内存少约 270–300 MiB，也不弹出窗口；有界面模式只用于排查登录。
- **安装**：
  - **下载**：
    - 正式分发版由 `setup` 从 Camoufox 官方 GitHub Release 下载 `runtime-manifest.json` 锁定的版本，校验 SHA-256；
    - 安装到 `<data_dir>/runtime/camoufox/<版本>-<平台>/`；
    - 运行包和 Docker 镜像都不包含 Camoufox，Docker 在第一次启动时下载到数据卷；
    - `browser_executable` 指向 Camoufox 时改用它，方便开发和排查。
  - **字体**：
    - Camoufox 158 的压缩包约 1.3 GB，解压后 2.5 GB，其中约 2.1 GB 是三套 OS 的字体，浏览器本体约 400 MB；
    - 身份呈现宿主 OS 时，Camoufox 只用系统字体；自带字体中只有 Firefox 本身也带的 emoji 字体 `Twemoji Mozilla` 仍会启用，页面能检测到它，emoji 也用它绘制；
    - 所以 Windows 上解压时跳过 `fonts/` 中 `TwemojiMozilla.ttf` 以外的文件；
    - 隐藏测试 `[.camoufox-fonts]` 确认裁剪版与完整版渲染相同：检测到的字体、每个字体族和通用字体族的文字宽度逐项一致；
    - Linux 按 `fonts/groups.json` 保留宿主 OS 的字体组，在 Linux 阶段实现。
  - **扩展**：不加载自带的 uBlock Origin，与现有的 `--disable-extensions` 一致，解压时就跳过 `addons/UBO`。
  - 开发版可以把 Camoufox 一起打包。

## SpiderMonkey 后端

- **复用参考项目代码**：直接使用 `internal/waa/dom.js`、`smhost.js`、对象结构表和 `mozjs/mozjs.cpp`，把 Go 部分翻译成 C++（`internal/waa` 约 1500 行，`internal/aistudio/runtime_go.go` 约 1000 行）。在 `THIRD_PARTY_NOTICES.md` 中保留 MIT 声明。复用时按本项目的维护习惯调整，并兼顾以后的桌面 App 与 Android App。
- **编译**：参考项目的 Windows 静态库由 llvm-mingw 编译，C++ ABI 与 MSVC 不兼容，必须用 clang-cl 自己编译。
- **网络指纹**：直接发出的受保护请求在 TLS 握手和 HTTP/2 帧特征上必须像 Firefox，否则请求头声称 Firefox、握手却是 OpenSSL。这是进主线的必要条件。具体实现（curl-impersonate 的 Firefox 模式、基于 BoringSSL/NSS 构造握手、是否支持 HTTP/3）在原型阶段调研后再定。原型阶段可以先用普通 libcurl，测试网络指纹是否为硬性条件。
- **Termux 与 Android App**：Termux 使用原生 NDK 版，不需要 proot。Android App 没有 Camoufox，只能使用 SpiderMonkey；是否用 GeckoView 兜底，在 Android App 立项时评估。

## 构建模式

CMake 选项 `AIS2API_DEV_BUNDLE` 区分开发版与正式分发版，默认关闭：

| | 开发版（开启） | 正式分发版（关闭） |
|---|---|---|
| 构建方式 | `scripts/build-dev.ps1` 在本机构建 | `scripts/build.ps1`、`scripts/build.sh` 与 CI |
| 身份 | 本机生成（与正式版相同） | 本机生成 |
| fpgen 模型 | 嵌入 | `setup` 下载 |
| Camoufox | 一起打包，放在 `bin/camoufox-runtime/<版本>-<平台>/` | `setup` 下载 |
| 新建配置的后端 | `camoufox` | `cloakbrowser` |

- 开发版目前只有 Windows x64；Linux 版脚本在 Linux 阶段加入。构建方法见 [构建与打包](platforms.md#构建开发版)。
- 打包的 Camoufox 是刚构建的程序用 `setup` 安装后原样复制的，裁剪方式和归属标记都与 `setup` 的安装相同。只有它是锁定版本、且按本程序的规则裁剪时才使用。使用顺序：指向 Camoufox 的 `browser_executable`、打包版、`setup` 的安装。使用打包版时 `browser rollback` 不可用，要退回就换回上一个开发版包。
- 已有的配置保持原来的后端：账号换成 Firefox 身份始终要显式操作。
- 防止误发：`build.ps1` 和 `build.sh` 拒绝开启了该选项的构建目录；`check-package.py` 要求正式分发包里是正式版构建、且不含 Camoufox，开发版包用 `--dev` 检查。
- 诊断信息、`/version`、`doctor` 和 WebUI 都显示构建模式（`build_mode`：`release` 或 `dev`）。
- CloakBrowser 的 `Browser-Local` 包不能与开发版同时构建，它随 CloakBrowser 退役一起删除；打包 Camoufox 只用开发版。

## 合并标准与问题反馈

- **合并标准**：迁移期间各阶段的 PR 以离线验证为准合并进 main，包括单元测试、隐藏的运行时测试，以及与上游实现的对比（如 `check-identities`）。真实账号上的完整验证（引导、流式和非流式生成、工具调用、Cookie 写回与切换账号）在迁移全部实现后统一进行，之后的问题根据反馈修复。
- **排查手段**：
  - 诊断信息和 WebUI 按后端分别统计 403（Code 7）、认证冷却和被跳转到登录页的次数。
  - 一键导出诊断报告，只含安全字段，不含 Cookie、请求正文与对话内容，用户报告问题时附上。

## CloakBrowser 退役

1. 在 Camoufox 后端覆盖所有平台（包括 Termux）之前，CloakBrowser 保持 `146.0.7680.177.4` 不变。
2. 覆盖之后的下一个版本把 CloakBrowser 标为弃用，并提供一条把账号显式迁移到 Firefox 身份的命令。
3. 再下一个版本删除 CloakBrowser 的全部代码与资料：`runtime-manifest.json` 中的浏览器条目、`Browser-Local` 打包、许可文件和相关文档。

## 分阶段计划

| 阶段 | 内容 | 完成条件 |
|---|---|---|
| P1 Camoufox 原型 | `poc/firefox-backends` 分支上用 C++ 实现 BiDi 后端（`GenerationBackend`），使用最新版 Camoufox 与一份预生成的固定身份，在 Windows 上运行 | 在真实账号上可用；验证 AI Studio 接受 Firefox 身份、`bridge.js` 在 Firefox 上可用；决定 Windows 的显示模式 |
| P2 Camoufox 主线 | 身份生成器（fpgen 移植）、`AIS2API_DEV_BUNDLE`、`setup` 下载与字体裁剪、按后端统计与诊断导出、手动选择后端；依次支持 Linux（Xvfb）与 Termux（proot + Xvfb） | 各平台可用后进入 CloakBrowser 弃用与移除流程 |
| P3 SpiderMonkey 原型 | 用 clang-cl 编译 SpiderMonkey 152.0.4，移植宿主，使用参考项目的对象结构表，以 Camoufox 作对照，验证 proof 被接受；调研网络指纹方案 | 生成的 proof 被 AI Studio 接受；决定网络指纹实现方案 |
| P4 SpiderMonkey 主线 | 静态库仓库与四平台 CI、对象结构表采集工具、升到最新版、Firefox 网络指纹、版本同步升级、自动切换后端、Termux NDK 版 | 与 Camoufox 共用身份、同版本运行；决定新账号的默认后端 |

P1 已于 2026-11-06 在 Windows 上用测试账号完成验证，见 [原型报告](reports/camoufox-prototype-2026-11-06.md)。

每项工作单独开分支、提 PR 到 main，由维护者审核合并。

## 低成本改进（独立推进）

与上述阶段并行，在等待使用反馈的空档中进行。

| 批次 | 内容 |
|---|---|
| 第一批 | 非流式保活：配置项，默认关闭；开启后等待超过 10 秒先返回 200，之后每 10 秒写一个换行，之后出现的错误放在 200 响应体中返回 |
| 第一批 | 识别 `GetLoggingContext` 返回的 `[7,"Region not supported."]` 与 `[7,"Account not supported."]`，比只检查是否跳转 `available-regions` 更早、更准 |
| 第一批 | Anthropic Messages 接口，以 Claude Code 能完整使用为验收标准：`/v1/messages` 流式与非流式、工具调用与工具结果、思考块及签名往返、图片输入，`/v1/messages/count_tokens` 使用上游 CountTokens；暂不支持 `context_management` |
| 第二批 | 模型名思考强度后缀（如 `model(low)`）、`MODEL_MAPPING` 模型别名 |
| 第三批 | 代理池：健康检查与账号固定出口 |
| 待数据 | 根据 SpiderMonkey 原型中 Code 7 的实测行为，校准 `include/ais2api/retry_policy.hpp` 中 403 的处理 |

## 明确不做

- 自动同意 AI Studio 条款。遇到需要同意条款的账号，报错让用户自己处理。
- 参考项目的 Chrome DBSC 导入：它向 Chrome 进程注入代码以取出应用绑定加密主密钥，安全风险高，杀毒软件会报。

## 待实测或待立项再定

| 事项 | 决定时机 |
|---|---|
| 新账号的默认后端 | SpiderMonkey 原型结果出来后 |
| Linux 主机是否伪装 Windows | Linux 阶段实测 |
| Firefox 网络指纹的实现方案 | SpiderMonkey 原型阶段调研后 |
| 多账号并发 | SpiderMonkey 后端稳定后 |
| 整个核心拆成库 | 桌面 App 或 Android App 立项时 |
| Android App 的兜底方案（GeckoView） | Android App 立项时 |

## 参考

- [CloakBrowser Releases](https://github.com/CloakHQ/CloakBrowser/releases) 与 [二进制许可](https://github.com/CloakHQ/CloakBrowser/blob/main/BINARY-LICENSE.md)
- [Camoufox](https://github.com/daijro/camoufox)（MPL-2.0；Python 与 TypeScript 启动器为 MIT）
- [AIStudio2API](https://github.com/Mag1cFall/AIStudio2API)（MIT），其中 `docs/protocol.md` 与 `docs/reverse-engineering.md` 可用于核对线路编解码与 WAA 生命周期
- [fpgen](https://github.com/scrapfly/fingerprint-generator)（Apache-2.0）
