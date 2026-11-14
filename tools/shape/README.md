# 对象结构表采集工具

SpiderMonkey 后端用 `waa/third_party/aistudio2api/dom.js` 和 `smhost.js` 模拟 AI Studio 页面的 DOM。模拟出的对象长什么样，由一张“对象结构表”决定：

- 接口有哪些成员、描述符和函数长度；
- 全局对象有哪些键、按什么顺序；
- 各单例与新建实例的属性值；
- 媒体查询的结果；
- 哪些成员返回 Promise。

这里的工具在真实浏览器里采集这张表，并合并成仓库里的 `waa/shapes/firefox<版本>-{windows,linux}.json.gz`。Windows 构建嵌入 Windows 表，其他平台嵌入 Linux 表（`waa/CMakeLists.txt`）。表的格式沿用参考项目 AIStudio2API；它没有公开采集工具，这里的格式说明是按 `dom.js`/`smhost.js` 的读取方式整理出来的，并用它的表逐项核对过。

| 文件 | 作用 |
|---|---|
| `collect.js` | 在页面顶层求值一次，完成值是表的 JSON 文本（Promise）。`window[0]` 必须是同源的子框架（AI Studio 的 bscframe，或 `collect.html` 里的 iframe）。不定义任何全局量。 |
| `collect.html`、`frame.html` | 本地采集页：一个隐藏的同源空白 iframe，和 bscframe 一样。默认自己运行 `collect.js` 并把结果 POST 回去；带 `?manual` 时留给打开它的工具求值。 |
| `serve.py` | 在 127.0.0.1 上提供采集页（Python 标准库），收到结果后写入文件并退出；`--serve-only` 只提供页面。 |
| `run-local.py` | 用任意 Firefox 系浏览器和一个新的临时 profile 打开采集页，结果到达后按进程号结束浏览器。不用 WebDriver。profile 把回环以外的请求都指向一个关闭的端口，并关闭更新、遥测和首次运行页。 |
| `../shape_collect.cpp` | `ais2api_shape_collect`（CMake 选项 `AIS2API_BUILD_SHAPE_COLLECTOR`）：用账号自己的 Camoufox 身份打开页面，通过 BiDi 求值 `collect.js`。 |
| `merge.py` | 把三份采集合并成最终的表，并写一份差异报告。 |
| `check.py` | 检查格式、隐私和大小（压缩后不超过 512 KiB，即 `scripts/embed-waa.cmake` 的上限）。 |

## 表的格式

顶层依次是：

- `userAgent`、`capturedAt`；
- `interfaces`：接口列表，父接口在前；
- `frame`、`top`：两个 Realm 的全局对象；
- `namespaces`、`defaults`、`own`、`singletons`、`topSingletons`；
- `tagMap`、`mediaQueries`、`builtins`、`promises`。

**条目**描述一个属性：

- `n` 是属性名，symbol 写作 `@@` 加描述；
- `f` 是描述符标志，`c`、`e`、`w` 按此顺序；
- `k` 是种类：
  - `a` 访问器，带 `g`/`gl`、`s`/`sl`，即 getter、setter 的名字和长度；
  - `m` 函数，带 `l`、`fn`、`nat`，即长度、名字、是否原生；
  - `i` 原生且 `prototype` 是对象的函数，即接口；
  - `v` 其他值，带 `v`；
  - `slot` 由 WAA 程序自己定义的全局量，只占键顺序中的位置。

**值**写作 `{t, v}`。`t` 的取值：

| `t` | 含义 |
|---|---|
| `u`、`null`、`s`、`b`、`n`、`bigint`、`sym` | 基本类型。`n` 的 NaN、±Infinity、-0 写成字符串 |
| `a` | 只含基本类型和函数的数组 |
| `ref` | 指向单例的路径，如 `top.window`、`frame.document.body` |
| `f` | 函数：名字和是否原生 |
| `o` | 对象：只记 `Object.prototype.toString` 的类名 `c` 和所属接口 `k`，不记内容。含有对象的数组也这样记，`dom.js` 把它解成空数组 |
| `throw` | 读取时抛出的错误 |

各部分的内容：

- **`interfaces`**：每项是 `{n, cn, p, pp, l, pw, st, pr, call, construct}`，含义如下：
  - `n` 是全局名，`cn` 是构造器自己的名字（别名如 `webkitURL` 的 `cn` 是 `URL`）；
  - `p` 是构造器的原型链父级，`pp` 是 `prototype` 的父级；
  - `st`、`pr` 是构造器和 `prototype` 的自有属性；
  - `call`、`construct` 是不带参数调用和 `new` 的结果。

  没有接口对象的接口（如 `External`）排在最后。下列构造器不会真的被 `new`，以免开始网络、媒体或 worker 活动：
  - 网络：WebSocket、WebTransport、EventSource、Request、Response、XMLHttpRequest；
  - 媒体：AudioContext、MediaStream、各类编解码器；
  - Worker 与流：Worker、SharedWorker、各类 Stream；
  - 通知：Notification。
- **`top`/`frame`**：
  - `global`：全局对象的全部自有键，按顺序列出；
  - `chain`：`Window.prototype` → `WindowProperties` → `EventTarget.prototype`；
  - `windowValues`：全局访问器和值在该窗口上读出的结果；
  - `frame.freshKeys`：子框架里新建 iframe 的窗口载入后的键顺序。
- **`singletons`**（子框架的 48 个路径）、**`topSingletons`**（顶层的 13 个）：`{iface, values, own}`。`values` 包括：
  - 实例自有的访问器和非函数值；
  - 原型链上每一级的访问器，从近到远。
- **`defaults`**：每个接口第一个实例的 `values`，**`own`** 是它们的自有属性。实例取自单例、各种构造器和 `document.createElement`。
- **`mediaQueries`**：`文本 → [顶层是否匹配, 规范化后的文本, 子框架是否匹配]`。
- **`promises`**：对品牌检查不通过的 `this` 返回（已拒绝的）Promise 的成员，即返回 Promise 的方法和 getter（`接口.成员`、`接口.成员:get`）。
- **`builtins`**：ES 内置对象和内部对象的自有属性，`dom.js` 不读，留作版本升级时核对 `dom.js` 中的内置对象列表。

## 采集不读什么

- 不读 `document.cookie`，直接记为空串；
- 不列 Web Storage 的内容；
- 已连接的 `html`/`head`/`body` 不读 `innerHTML`、`outerHTML`、`innerText`、`outerText`、`textContent`；
- 对象只记类名。

在线采集只加载一次账号的 AI Studio 新对话页（与 SpiderMonkey 后端打开的地址相同：`/prompts/new_chat`，authuser 不为 0 时是 `/u/N/prompts/new_chat`），不生成，不点击，不预加载 `bridge.js` 或任何脚本。

## 采集步骤

以下命令都在仓库根目录运行，采集结果放在被 Git 忽略的目录里（例如 `.tools/scratch/shape/`）。

### 1. 官方 Firefox

从 `archive.mozilla.org/pub/firefox/releases/<版本>/` 下载安装包、`SHA512SUMS`、`SHA512SUMS.asc` 和 `KEY`。用临时的 `GNUPGHOME` 导入 `KEY`，确认签名有效，且主密钥指纹是 `14F26682D0916CDD81E37B6D61B7B526D98F0353`（Mozilla Software Releases）；再核对安装包的 SHA-512。

Windows 的安装包用 7-Zip 解出 `core/`，Linux 的 `.tar.xz` 在 WSL 的本地文件系统里解开。然后：

```sh
python -I tools/shape/run-local.py <firefox 可执行文件> official-windows.json
# WSL 中，官方 Linux 版：
python3 -I tools/shape/run-local.py <firefox 可执行文件> official-linux.json
```

### 2. Camoufox，本地采集页，账号的身份

服务停止时运行，账号的身份与在线采集用同一个：

```sh
python -I tools/shape/serve.py --serve-only --timeout 600 &   # 打印 "url http://127.0.0.1:<端口>/collect.html"
ais2api_shape_collect --config <config.json> --account <账号 ID> --script tools/shape/collect.js \
  --out camoufox-local.json --local "http://127.0.0.1:<端口>/collect.html?manual"
```

### 3. Camoufox，在线页面

账号的 Cookie 要有效。服务停止时运行；遇到 401、403 就停，不重试：

```sh
ais2api_shape_collect --config <config.json> --account <账号 ID> --script tools/shape/collect.js --out live.json
```

### 4. 合并与检查

```sh
python -I tools/shape/merge.py --base live.json --official official-windows.json --camoufox camoufox-local.json \
  --slot botguard --out firefox<版本>-windows.json.gz --report merge-windows.md
python -I tools/shape/merge.py --base firefox<版本>-windows.json.gz --official official-linux.json \
  --camoufox official-windows.json --labels "Windows,Linux" --slot botguard \
  --out firefox<版本>-linux.json.gz --report merge-linux.md
python -I tools/shape/check.py firefox<版本>-windows.json.gz --forbid <本机用户名>
python -I tools/shape/check.py firefox<版本>-linux.json.gz --forbid <本机用户名>
```

合并规则（详见 `merge.py` 开头）：

- **基础**：在线采集提供页面全局量、页面结构和全局键的顺序。
- **Camoufox 的痕迹**：同一身份的 Camoufox 与官方 Firefox 在本地采集页上的差异。
  - 只有 Camoufox 有的键删掉；
  - 只有官方有的键插到官方顺序中前一个共有键之后；
  - 元数据不同的条目改用官方的；
  - 在新窗口里次序不同的键，移到官方顺序中的位置。Camoufox 158 的 `Event`、`Location` 就是这样。顶层窗口的全局量什么时候解析，取决于页面先碰了什么，所以官方 Firefox 在本地采集页顶层的顺序每次运行略有不同，移动后的位置也会差几个；报告里写明了每个键移到了哪个键之后。
- **只和浏览器有关的部分直接取官方的**：新 iframe 的键、`builtins`、`promises`、`tagMap`。
- **固定值**：凡是运行时由身份覆盖、或描述本机的值，一律换成固定值：
  - navigator 的平台字符串，取自官方采集；核心数、语言用固定值；
  - 屏幕、窗口的尺寸和位置，`devicePixelRatio`；
  - 媒体查询；
  - 时间戳；
  - 页面地址，规范成 `/prompts/new_chat`，去掉 `/u/N/`；加载计时里的重定向置 0（后端直接打开 `/u/N/`）；
  - 页面的 `<html lang>`，它跟着账号的界面语言走；
  - Cookie 与存储；
  - 错误对象的文件名与调用栈。
- **Linux 表**：在 Windows 表上套用官方 Windows 版与官方 Linux 版之间的差异。

最后人工看一遍两份差异报告，每一处都要能解释。

## 升级 Firefox 版本时

1. 三方（官方 Firefox、Camoufox、SpiderMonkey）升到同一个大版本后，按上面的步骤重新采集。
2. 用 `check.py` 检查新表；把新表的 `builtins` 与 `dom.js` 开头的 ES 内置对象列表对照。
3. 新增的接口若需要构造器以外的实例（`defaults`），在 `collect.js` 的 `instances` 里补上。会开始网络、媒体或 worker 活动的新构造器，补进 `NOT_CONSTRUCTED`。
4. 用新表跑 `ais2api_tests "[waa],[spidermonkey]"`。
