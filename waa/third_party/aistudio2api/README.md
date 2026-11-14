# AIStudio2API 的原文件

本目录的文件取自 [AIStudio2API](https://github.com/Mag1cFall/AIStudio2API) 的提交 `e38957a084c8928977e74a1d4faba80e5e852950`（MIT License，Copyright (c) 2026 Mag1cFall，许可全文见 [`licenses/AIStudio2API-LICENSE.txt`](../../../licenses/AIStudio2API-LICENSE.txt)），未作修改。`.gitattributes` 关闭了本目录的换行转换，文件与上游逐字节相同。

| 文件 | 上游路径 | SHA-256 |
|---|---|---|
| `dom.js` | `internal/waa/dom.js` | `c0640903525052cf8c5e70aa5cd3d60184c5a496cff95b4d5899d39d774e6859` |
| `smhost.js` | `internal/waa/smhost.js` | `271e8a2a1a2dcd3e572ee2e8a1573dca1ac491bf55b0bcdbef0c7f365ea37d45` |
| `firefox152.json.gz` | `internal/waa/firefox152.json.gz` | `8477c8a2838824201fc6e034d139d6207eed633d2618e49864e9c6d879eb463d` |
| `mozjs/mozjs.cpp` | `internal/waa/mozjs/mozjs.cpp` | `ef70ce5b33fac9047a88a37026e2fac2364825297340be507a2d6980d6ccce2b` |
| `mozjs/mozjs.h` | `internal/waa/mozjs/mozjs.h` | `55f571210f6525d1ee0af192ef116df1be68325ebb685de39f691cc448d5dacd` |

各文件的用途：

- `dom.js`：Firefox 形状的 DOM 宿主，在每个 Realm 里安装。
- `smhost.js`：宿主与 C++ 原生函数之间的接口。
- `firefox152.json.gz`：从 Firefox 152 采集的对象结构表，运行时解压。
- `mozjs/mozjs.cpp`、`mozjs/mozjs.h`：SpiderMonkey 的 C 接口层。它与 SpiderMonkey 一起由 [`tools/spidermonkey`](../../../tools/spidermonkey/README.md) 用 clang-cl 编译成静态库；本项目的 C++ 只通过 `mozjs.h` 的 C 接口调用它。接口层回调的宿主函数 `mzGoNative` 由 `waa/src/js.cpp` 定义。

更新时整组替换，并同步更新本表、`waa/src/host_scripts.hpp`（由上游 `host.go`、`runtime.go` 中的脚本常量生成）和 `THIRD_PARTY_NOTICES.md` 中的提交号。
