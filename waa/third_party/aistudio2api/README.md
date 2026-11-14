# AIStudio2API 的原文件

本目录的文件取自 [AIStudio2API](https://github.com/Mag1cFall/AIStudio2API) 的提交 `d94bf91c071e921ba2d0d8c6d14fef4faae2a469`（MIT License，Copyright (c) 2026 Mag1cFall，许可全文见 [`licenses/AIStudio2API-LICENSE.txt`](../../../licenses/AIStudio2API-LICENSE.txt)），未作修改。`.gitattributes` 关闭了本目录的换行转换，文件与上游逐字节相同。

| 文件 | 上游路径 | SHA-256 |
|---|---|---|
| `dom.js` | `internal/waa/dom.js` | `6467229f4c82a2d076631a03ce639b80b56674e55984709a0447c066087af86c` |
| `smhost.js` | `internal/waa/smhost.js` | `6c3e76a07996774b75983102bc7e8b01c3e8aea4b661dddacf4f64b118cf0edd` |
| `mozjs/mozjs.cpp` | `internal/waa/mozjs/mozjs.cpp` | `ba33500b3b7a00c612cdc276c62dea17b4f1a8ada81dba00204332d513847be9` |
| `mozjs/mozjs.h` | `internal/waa/mozjs/mozjs.h` | `a3b917d3854c5d1e3b4211132a86a963036f3dcb9a7e6a864e8f54b452524993` |

各文件的用途：

- `dom.js`：Firefox 形状的 DOM 宿主，在每个 Realm 里安装。
- `smhost.js`：宿主与 C++ 原生函数之间的接口。`dom.js` 读取的对象结构表不在这里，是本项目自己采集的，见 [`waa/shapes`](../../shapes) 和 [`tools/shape`](../../../tools/shape/README.md)。
- `mozjs/mozjs.cpp`、`mozjs/mozjs.h`：SpiderMonkey 的 C 接口层。它与 SpiderMonkey 一起由 [`tools/spidermonkey`](../../../tools/spidermonkey/README.md) 用 clang-cl 编译成静态库；本项目的 C++ 只通过 `mozjs.h` 的 C 接口调用它。接口层回调的宿主函数 `mzGoNative` 由 `waa/src/js.cpp` 定义。

更新时整组替换，并同步更新本表、`waa/src/host_scripts.hpp` 和 `THIRD_PARTY_NOTICES.md` 中的提交号。`host_scripts.hpp` 是上游 `host.go`、`runtime.go` 中的脚本常量，由脚本从上游源码生成（只读取源码，不运行）：

```sh
python -I scripts/waa-host-scripts.py <上游仓库或其副本> <40 位提交号> waa/src/host_scripts.hpp
```
