# SpiderMonkey 升级到 158.0.1（P4 第二项）

日期：2026-11-14。[Firefox 后端迁移计划](../firefox-backends.md) P4 的第二项：SpiderMonkey 后端从 152.0.4 升到 158.0.1，与 Camoufox 后端声称同一个 Firefox 大版本。本文记录构建与引擎补丁、宿主和页面行为的移植、对象结构表的采集工具及核对结果，以及采集中的发现。这一项没有发实盘请求，线上验证等账号重新导入后再做。

## 结论

- **版本对齐**：SpiderMonkey 升到 158.0.1，与 Camoufox 158.0.1-beta.41 是同一份 Firefox 源码。两个后端都声称 Firefox 158，同一账号在两者之间切换，不会再出现浏览器版本倒退。
- **引擎补丁由本项目维护**：152 的补丁移植到 158.0.1，语言区域改成只覆盖默认区域。参考项目的 158 补丁只用来对照。
- **宿主文件按参考项目的 158 版取用**，页面行为一起移植：
  - 可信的鼠标点击（Run、Rerun）；
  - 按真实时间线创建 WAA 程序；
  - 官网 BotGuard 包装层的计时指标。
- **对象结构表暂用参考项目的 158 表**。采集工具已完成，用官方 Firefox 158.0.1 核对过：
  - 接口和属性描述与官方版一致；
  - 全局键顺序带有 Camoufox 的痕迹（见“发现”）。

  账号重新导入后，再换成自己采集的 Windows 与 Linux 表。
- **Camoufox 158 没有把 `set*` 指纹接口暴露给页面**（计划的第 0 步）。
- **离线测试全部通过**（预期失败的 1 个除外）。线上验证还没有做。

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
- 接口层 `mozjs/mozjs.{cpp,h}`；
- 对象结构表 `firefox158.json.gz`。

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

### 与计划的出入

- **本地采集页没有跨源 iframe**：计划以为 AI Studio 页面上还有一个跨源 iframe，实际检查发现顶层的 `window.length` 是 1，只有同源的 bscframe。
- **脚本改名**：计划里的 `run-official.py` 改叫 `run-local.py`，因为它能打开任何 Firefox 系浏览器，不只是官方版。

## 发现

1. **Camoufox 158 没有把 `set*` 接口暴露给页面**。用生产环境的启动方式检查：测试账号的身份、BiDi 会话、不预加载 `bridge.js`。结果：
   - 与官方 Firefox 相比，页面上没有多出任何全局量；
   - `navigator.webdriver` 是 `false`。

   参考项目 152 表里的 15 个 `set*` 函数，在 158 表里也没有了。
2. **Camoufox 的全局键顺序与官方 Firefox 不同**：
   - **现象**：新建的 iframe 窗口里，官方 Firefox 的 `Event`、`Location` 在接口中间各自的位置，Camoufox 却把它们排在末尾附近，接在 `Performance` 之后。页面脚本用 `Object.getOwnPropertyNames(window)` 就能看出来。
   - **来源在浏览器内部**：这个窗口除了采集脚本，没有别的脚本碰过，所以顺序变化不是页面脚本造成的。具体原因还没查，可能是 Camoufox 或 juggler 在新窗口上提前访问了这两个对象。
   - **参考表里也有**：参考项目的 158 表有同样的痕迹，新窗口里的 `Array`、`PageTransitionEvent` 也移了位置。这说明它也是用 Camoufox 一类的浏览器采集的。
   - **两个后端暂时一致**：现在两个后端都带着这个痕迹；换成自己的表后，SpiderMonkey 一侧就没有了。
   - **修复另开一项**：Camoufox 后端本身怎么修，另开一项处理。
3. **Camoufox 后端的 `bridge.js` 在页面上定义了可枚举的全局量 `__ais2bridge`**，页面脚本能看到它。CloakBrowser 时就是这样设计的，不是这次的改动。修复另开一项处理。
4. **Linux 版 Firefox 158 默认不提供 WebGPU**：
   - Linux 表要按官方 Linux 版采集，不能直接复用 Windows 表；
   - 两个平台上，`-moz-platform` 媒体查询在页面中都不匹配。

## 测试

| 构建 | 结果 |
|---|---|
| 普通构建，完整测试 | 494 个用例全部通过（19,510 个断言） |
| SpiderMonkey 构建，完整测试 | 518 个用例，517 个通过，另 1 个是预期失败：流级 WINDOW_UPDATE，见 [网络指纹实现报告](firefox-network-stack-2026-11-14.md) |
| SpiderMonkey 构建，嵌入 Windows 试验表，`[waa],[spidermonkey]` | 49 个用例全部通过 |

升级时还修好了一个偶发失败的测试：GOAWAY 测试的 HTTP/2 服务端在 GOAWAY 后立即关闭连接，客户端的最后几帧还没读完，Windows 因此重置连接，大约三次失败一次。现在服务端会等客户端先关闭。

## 后续

1. **账号重新导入后，经同意**：
   - 用测试账号在线采集一次，只加载，不生成，服务停止时运行；
   - 用同一身份的 Camoufox 采集本地页；
   - 合并出 `waa/shapes/firefox158-windows.json.gz` 和 `firefox158-linux.json.gz`；
   - CMake 改为 Windows 嵌入 Windows 表，其他平台嵌入 Linux 表；
   - 更新第三方说明。
2. **线上验证**：SpiderMonkey 后端用 `gemini-3.1-flash-lite` 非流式生成 1 次，同时用 netfp 抓包。要确认的有：
   - proof 被接受；
   - 两个后端的 UA 都是 `rv:158`；
   - 图片的 `Accept`、页面 RPC 的请求头。
3. **另开的项**：Camoufox 的全局键顺序（发现 2），`bridge.js` 的全局量（发现 3）。

## 资料

- 构建配方：[`tools/spidermonkey/`](../../tools/spidermonkey/README.md)。
- 采集工具：[`tools/shape/`](../../tools/shape/README.md)，以及 `tools/shape_collect.cpp`、`CamoufoxGateway::experiment_collect()`。
- 参考项目的文件：`waa/third_party/aistudio2api/`。
- 上一项：[Firefox 网络指纹实现（P4 第一步）](firefox-network-stack-2026-11-14.md)；原型：[SpiderMonkey 原型（P3）](spidermonkey-prototype-2026-11-12.md)。
