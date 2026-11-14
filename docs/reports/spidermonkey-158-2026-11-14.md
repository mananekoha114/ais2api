# SpiderMonkey 升级到 158.0.1（P4 第二项）

日期：2026-11-14。[Firefox 后端迁移计划](../firefox-backends.md) P4 的第二项：SpiderMonkey 后端从 152.0.4 升到 158.0.1，与 Camoufox 后端声称同一个 Firefox 大版本。本文记录构建与引擎补丁、宿主和页面行为的移植、对象结构表的采集工具及核对结果、采集中的发现，以及账号重新导入后的在线采集和线上验证。

## 结论

- **版本对齐**：SpiderMonkey 升到 158.0.1，与 Camoufox 158.0.1-beta.41 是同一份 Firefox 源码。两个后端都声称 Firefox 158，同一账号在两者之间切换，不会再出现浏览器版本倒退。
- **引擎补丁由本项目维护**：152 的补丁移植到 158.0.1，语言区域改成只覆盖默认区域。参考项目的 158 补丁只用来对照。
- **宿主文件按参考项目的 158 版取用**，页面行为一起移植：
  - 可信的鼠标点击（Run、Rerun）；
  - 按真实时间线创建 WAA 程序；
  - 官网 BotGuard 包装层的计时指标。
- **对象结构表换成了本项目自己采集的 Windows 与 Linux 表**（`waa/shapes/`），参考项目的表已删除：
  - 页面部分来自测试账号的一次在线采集，只加载，不生成；
  - 浏览器部分以官方 Firefox 158.0.1 为准；
  - 去掉了 Camoufox 的痕迹（见“发现”）。

  参考项目的表用官方 Firefox 158.0.1 核对过：接口和属性描述与官方版一致，但全局键顺序带有同样的 Camoufox 痕迹。
- **Camoufox 158 没有把 `set*` 指纹接口暴露给页面**（计划的第 0 步）。
- **线上验证通过**：用自己的表生成 1 次，返回 200；同时抓包，握手和请求头与 Camoufox 158 一致（已知差异除外）。
- **离线测试全部通过**（预期失败的 1 个除外）。

## 构建与引擎补丁

| 项目 | 版本 |
|---|---|
| 源码 | `firefox-158.0.1.source.tar.xz`，与 Camoufox 158.0.1 相同 |
| Rust | 1.95.0（158 源码 `taskcluster/kinds/toolchain/rust.yml` 指定的版本） |
| clang | 22.1.8 |
| MSVC | 14.51.36231，Windows SDK 10.0.28000.0，静态运行库 `/MT` |
| 产物 | `js_static.lib` 约 394 MB，`jsrust.lib` 约 38 MB，`mozjs_shim.lib`；CMake 按 `BUILDINFO.json` 核对 SHA-256 |

配方（`tools/spidermonkey/`）的修正：

- **Rust**：以前只在没有 `rustc` 时才安装，已有旧版本就不会装新的。现在按版本安装，并用 `RUSTUP_TOOLCHAIN` 固定 mach 使用的版本。
- **构建目录按版本分开**（`obj-win64-<版本>`），不会和 152 的构建混用。
- **解压源码**：改为 `xz` 通过管道交给 `tar`。原因是在 MSYS2 下，`tar -J` 派生 `xz` 会随机失败。
- **启动 MSYS2 shell 时不再嵌套引号**，因为 `msys2_shell.cmd` 处理不了嵌套的引号。

引擎补丁 `0001-aistudio2api-embedding.patch` 基于 `FIREFOX_158_0_1_RELEASE`，改动 8 个文件：`GlobalObject.*`、`FrameIter.cpp`、`jsapi.cpp`、`Locale.*`、`flags.configure`、`js/src/build/moz.build`。

与 152 的不同在语言区域：

- **152 的做法**：把每个 `Locale` 对象的 language 和 region 都换掉了，所以 `new Intl.Locale("fr-FR").language` 也会返回账号的语言，页面能检测出这个异常。
- **158 的做法**：只覆盖默认区域，接口改为单参数的 `mozjs_set_camou_locale(locale)`。参考项目的新版接口层也要求这个接口。

152 的产物改名为 `dist/windows-x64-152.0.4`，留作对照和回滚。

## 宿主与页面行为

**逐字节取自参考项目 AIStudio2API（提交 `d94bf91c07`）**：

- `dom.js`、`smhost.js`；
- 接口层 `mozjs/mozjs.{cpp,h}`。

参考项目的对象结构表 `firefox158.json.gz` 起初也照取，换成自己的表后已删除。

`waa/src/host_scripts.hpp` 由 `scripts/waa-host-scripts.py` 从它的 Go 源码生成。新版带来：

- 接口原型成员的延迟定义；
- `UserActivation`、可信的鼠标输入；
- WebIDL 默认的 `toJSON`；
- `PerformanceTiming`。

**移植的页面行为**（照参考项目的 `runtime.go`、`host.go`、`challenge.go` 改写）：

- **点击 Run**：生成前先用可信的鼠标事件点击 Run，再取 proof。时间间隔按 Camoufox 实测：移动后 5 ms 按下，再过 60 ms 松开，点击后 50 ms 取 proof。
- **点击 Rerun**：客户端重试同一段对话时，如果这个页面上已经点击过，就像页面一样点击最后一轮的 Rerun。此时提示词框是空的，所以不发 `CountTokens`。其他情况的 proof 跟在一次用户激活之后。
- **指针事件**：页面上的程序共用一个指针。响应替换掉指针下方的元素时，以及响应结束时，都会收到 `pointerover` 和 `mouseover`。
- **页面加载的时间线**：
  - 页面的 `PerformanceNavigationTiming` 按 Firefox 158 的格式生成，数值由 libcurl 实测的文档加载计时换算；
  - 页面的 `load` 结束后才创建 WAA 程序。
- **计时指标**：官网 BotGuard 包装层记录的计时指标保存在运行时中，目前不上报。
- **声称的版本**：和 Camoufox 后端一样声称 Firefox 158：
  - User-Agent；
  - 图片请求的 `Accept`，带 `image/jxl`；
  - 身份没有给出内容区高度时，按窗口高度减 85 px 计算。

**没有移植的**：参考项目新增的页面遥测和子资源加载。

## 对象结构表

### 采集工具

参考项目没有公开采集工具，格式是按 `dom.js`、`smhost.js` 的读取方式整理出来的，再用它的表逐项核对。工具和格式说明见 [`tools/shape`](../../tools/shape/README.md)：

- `collect.js`：在页面顶层求值一次，导出整张表；
- 本地采集页与 `serve.py`；
- `run-local.py`：用临时 profile 打开本地采集页，不用 WebDriver；
- `ais2api_shape_collect`：用账号自己的 Camoufox 身份，通过 BiDi 采集；
- `merge.py`、`check.py`：合并、检查。

表分三部分采集：

| 部分 | 来源 |
|---|---|
| 页面部分（页面全局量、页面结构、全局键顺序） | 账号的 Camoufox 打开真实的 AI Studio 页面，只加载，不生成 |
| 浏览器部分 | 官方 Firefox 158.0.1 打开本地采集页 |
| Camoufox 的痕迹 | 同一身份的 Camoufox 与官方 Firefox 在本地采集页上的差异，合并时去掉 |

Linux 表在 Windows 表上套用官方 Windows 版与官方 Linux 版的差异。凡是运行时由身份覆盖、或描述本机的值，合并时一律换成固定值：

- 屏幕和窗口；
- navigator 的核心数和语言；
- 媒体查询；
- 时间戳；
- 页面地址；
- Cookie 与存储；
- 错误对象的文件名与调用栈。

官方 Firefox 从 archive.mozilla.org 下载，核对 GPG 签名（主密钥 `14F26682D0916CDD81E37B6D61B7B526D98F0353`）和 SHA-512。Windows 版直接运行，Linux 版在 WSL 中运行，都只打开本地采集页，不接触 Google。

### 核对结果

**参考表与官方 Firefox 158.0.1（Windows）**：

- 接口列表、接口成员与描述符、原生函数标记、Window 原型链都一致。
- 不同之处：
  - 页面自己的全局量，和 zone.js 对原型的改动：属于页面部分，本来就该不同；
  - 环境值：屏幕、窗口、时间戳等；
  - 两个实例的默认值：`FocusEvent` 的 `composed`、`MessageEvent` 的 `data`。看起来参考表取的是页面里真实发生的事件，而不是新建的事件；
  - 全局键顺序：见“发现”的第 2 条；
  - `builtins` 里 4 个构造器的标记（`WebAssembly.Module`、`Tag`、`Suspending` 与 `AsyncFunction`）。参考表记为不能构造，我们按引擎的判断如实记录。`dom.js` 不读这一部分。

**同一本地页面上的 Camoufox 与官方 Firefox**：只在全局键顺序上不同，见“发现”的第 2 条。

**官方 Windows 版与官方 Linux 版**：

- Linux 版默认不提供 WebGPU，所以少了：
  - 36 个接口和 5 个常量对象；
  - `navigator.gpu`；
  - 9 个返回 Promise 的成员。
- `Navigator` 接口的成员也因此不同；
- 平台字符串不同。

**端到端试验**：还没有在线采集，于是拿参考表代替在线采集，按完整流程合并出两张试验表：

| 表 | 压缩后大小 | 结果 |
|---|---|---|
| Windows | 181,689 字节 | 通过 `check.py` |
| Linux | 176,918 字节 | 通过 `check.py` |

参考表本身是 181,746 字节，上限是 512 KiB。合并报告中，Windows 一侧只有两项改动：

- `Event`、`Location` 移回官方顺序；
- `botguard` 保留为程序自己定义的全局量。

把 Windows 试验表嵌入 SpiderMonkey 构建后，`[waa],[spidermonkey]` 的 49 个用例全部通过（5857 个断言）。试验后已换回参考表。

### 自己的表

账号重新导入后采集（2026-11-14 13:30 UTC 前后）：

- **在线采集**：测试账号的 Camoufox 打开一次新对话页，只加载，不生成，没有遇到拒绝。
  - 采到的全局键有 1052 个，与参考表一样多；
  - 名字上只差页面库随机生成的 `closure_lm_<随机数>`，每次加载都会变。
- **本地采集页**：同一身份的 Camoufox 重新采集一次，与之前的结果结构相同。
- **合并结果**：

  | 表 | 压缩后大小 | 合并报告 |
  |---|---|---|
  | `firefox158-windows.json.gz` | 182,424 字节 | `Event`、`Location` 移回官方顺序；`botguard` 保留为程序自己定义的全局量 |
  | `firefox158-linux.json.gz` | 177,494 字节 | 去掉 WebGPU 相关的接口、全局量和 `navigator.gpu`；`Navigator` 接口改用 Linux 版的 |

  两张表都通过 `check.py`。另外检查了不能出现的内容：本机用户名、账号 ID、账号的界面语言。
- **嵌入方式**：Windows 嵌入 Windows 表，其他平台嵌入 Linux 表。Android 暂时也用 Linux 表。

合并前对采集工具做了三处修正：

- **页面的 `<html lang>`**：这个值跟着账号的界面语言走，现在合并时规范成固定的 `en-US`，与表里 navigator 的固定语言一致。
- **加载计时里的重定向**：
  - 原因：在线采集原来打开 `?authuser=N`，页面会跳转一次到 `/u/N/`，加载计时里因此留下了重定向；
  - 为什么要改：SpiderMonkey 直接打开 `/u/N/`，没有跳转，运行时也不覆盖重定向的三个字段，保留下来会让时间线自相矛盾；
  - 修正：合并时把这三个字段置 0，采集器也改为直接打开 `/u/N/`。
- **数组的写法**：改成和参考表一样，只有元素全是基本类型或函数的数组才逐项记下，其余只记类名，`dom.js` 会把它解成空数组。原来逐项记下的话，`dom.js` 会解出一组假的 `Arguments` 对象。
  - 受影响的只有页面的 `dataLayer` 和 `Aᶜstate`；
  - 已有的在线采集按同一规则离线重新编码，没有为此再访问 Google。

与参考表相比，剩下的差异都能解释：

- **页面部分**：
  - AI Studio 新部署带来的页面结构和构建标识变化，例如 `head` 的子元素个数、`_F_*` 全局量；
  - `closure_lm_` 的随机名。
- **页面加载完时的焦点元素**：我们的表里是一个 `HTMLElement`，参考表里是提示词输入框。无界面运行的 Camoufox 后端，页面上就是这样；`dom.js` 直接返回表里的值。影响很小，先记下观察。
- **实例的默认值**：`FocusEvent.composed`、`MessageEvent.data` 取官方 Firefox 新建事件的值。
- **html、head、body 的内容属性**：采集时不读。`dom.js` 自己计算 `innerHTML`、`outerHTML`、`textContent`；`innerText`、`outerText` 回退到 `HTMLElement` 的默认值（空串），和参考表的效果相同。

### 与计划的出入

- **本地采集页没有跨源 iframe**：计划以为 AI Studio 页面上还有一个跨源 iframe，实际检查发现顶层的 `window.length` 是 1，只有同源的 bscframe。
- **脚本改名**：计划里的 `run-official.py` 改叫 `run-local.py`，因为它能打开任何 Firefox 系浏览器，不只是官方版。

## 线上验证

2026-11-14 13:41 UTC，在自己的表嵌入后进行：测试账号、SpiderMonkey 后端、`gemini-3.1-flash-lite` 非流式生成 1 次，同时用 [`tools/netfp`](../../tools/netfp/README.md) 抓包。

- 这次 Camoufox 没有生成，Camoufox 一侧用的是 [上一次实盘对照](firefox-network-stack-2026-11-14.md) 的抓包；
- 原始抓包和密钥文件分析完即删除。

| 项目 | 结果 |
|---|---|
| 生成 | 200，回复正常；耗时 7.1 秒（含加载页面），WAA 程序启动 1.6 秒。页面直接打开 `/u/N/`，没有跳转 |
| TLS | JA4 `t13d1517h2_8daaf6152771_3cbfd9057e0d`，6 条连接都是完整握手，与 Camoufox 158 相同 |
| 文档、bscframe、图片 | 请求头顺序、`priority` 和权重都与 Camoufox 158 相同。User-Agent 是 `rv:158`；图片的 `Accept` 带 `image/jxl`，权重 12 |
| 页面 RPC | GetLoggingContext、GetAiStudioBenefitTier、Waa/Create、CountTokens 与 Camoufox 页面自己的 XHR 相同。GenerateContent 与按 CountTokens 推断的顺序相同，[网络指纹实现报告](firefox-network-stack-2026-11-14.md) 里请求头的修正因此在线上得到确认 |
| 解释器脚本 | 解释器换了新版本，所以下载了一次：`www.google.com/js/bg/…`，`sec-fetch-dest=script`，不带 `priority`，权重 22。上次 Camoufox 用的是缓存，没有样本可以对照 |
| 已知差异 | 流级 WINDOW_UPDATE；ECH GREASE 长度（186、218、250 字节，Firefox 是 282）；第一个 GetAiStudioBenefitTier 带 `x-goog-ext-519733851-bin` |

Cookie 个数比上次 Camoufox 的抓包少 2 个，但两次是不同的导入，没法比较。

## 发现

1. **Camoufox 158 没有把 `set*` 接口暴露给页面**。用生产环境的启动方式检查：测试账号的身份、BiDi 会话、不预加载 `bridge.js`。结果：
   - 与官方 Firefox 相比，页面上没有多出任何全局量；
   - `navigator.webdriver` 是 `false`。

   参考项目 152 表里的 15 个 `set*` 函数，在 158 表里也没有了。
2. **Camoufox 的全局键顺序与官方 Firefox 不同**：
   - **现象**：新建的 iframe 窗口里，官方 Firefox 的 `Event`、`Location` 在接口中间各自的位置，Camoufox 却把它们排在末尾附近，接在 `Performance` 之后。页面脚本用 `Object.getOwnPropertyNames(window)` 就能看出来。
   - **来源在浏览器内部**：这个窗口除了采集脚本，没有别的脚本碰过，所以顺序变化不是页面脚本造成的。具体原因还没查，可能是 Camoufox 或 juggler 在新窗口上提前访问了这两个对象。
   - **参考表里也有**：参考项目的 158 表有同样的痕迹，新窗口里的 `Array`、`PageTransitionEvent` 也移了位置。这说明它也是用 Camoufox 一类的浏览器采集的。
   - **SpiderMonkey 一侧已经去掉**：换成自己的表后，SpiderMonkey 后端没有这个痕迹了，Camoufox 后端仍然有。
   - **修复另开一项**：Camoufox 后端本身怎么修，另开一项处理。
3. **Camoufox 后端的 `bridge.js` 在页面上定义了可枚举的全局量 `__ais2bridge`**，页面脚本能看到它。CloakBrowser 时就是这样设计的，不是这次的改动。修复另开一项处理。
4. **Linux 版 Firefox 158 默认不提供 WebGPU**：
   - Linux 表要按官方 Linux 版采集，不能直接复用 Windows 表；
   - 两个平台上，`-moz-platform` 媒体查询在页面中都不匹配。

## 测试

| 构建 | 结果 |
|---|---|
| 普通构建，完整测试 | 494 个用例全部通过（19,510 个断言） |
| SpiderMonkey 构建，完整测试，参考表 | 518 个用例，517 个通过，另 1 个是预期失败：流级 WINDOW_UPDATE，见 [网络指纹实现报告](firefox-network-stack-2026-11-14.md) |
| SpiderMonkey 构建，嵌入 Windows 试验表，`[waa],[spidermonkey]` | 49 个用例全部通过 |
| SpiderMonkey 构建，完整测试，自己的 Windows 表 | 518 个用例，517 个通过，另 1 个是同一个预期失败 |

升级时还修好了一个偶发失败的测试：GOAWAY 测试的 HTTP/2 服务端在 GOAWAY 后立即关闭连接，客户端的最后几帧还没读完，Windows 因此重置连接，大约三次失败一次。现在服务端会等客户端先关闭。

## 后续

1. **另开的项**：
   - Camoufox 的全局键顺序（发现 2）；
   - `bridge.js` 的全局量（发现 3）。
2. **SpiderMonkey 的 Linux 版做好后**，用 Linux 表在 Linux 上跑一次测试和线上验证。
3. **以后升级 Firefox 版本时**，按 [`tools/shape`](../../tools/shape/README.md) 的步骤重新采集。

## 资料

- 构建配方：[`tools/spidermonkey/`](../../tools/spidermonkey/README.md)。
- 采集工具：[`tools/shape/`](../../tools/shape/README.md)，以及 `tools/shape_collect.cpp`、`CamoufoxGateway::experiment_collect()`。
- 对象结构表：`waa/shapes/`。
- 参考项目的文件：`waa/third_party/aistudio2api/`。
- 上一项：[Firefox 网络指纹实现（P4 第一步）](firefox-network-stack-2026-11-14.md)；原型：[SpiderMonkey 原型（P3）](spidermonkey-prototype-2026-11-12.md)。
