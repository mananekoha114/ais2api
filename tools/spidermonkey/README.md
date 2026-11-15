# SpiderMonkey 构建配方

实验性的 SpiderMonkey 后端（`browser_backend: "spidermonkey"`）在进程内运行 Google 的 WAA 虚拟机。这里的文件把 Firefox 158.0.1 的 JavaScript 引擎编成 ais2api 能链接的静态库，产物目录由 CMake 选项 `AIS2API_SPIDERMONKEY_DIR` 指定。

| 文件 | 作用 |
|---|---|
| `mozconfig-windows` | windows-x64 的构建选项：只编引擎，优化、无断言，静态库，clang-cl，静态运行库 |
| `patches/0001-aistudio2api-embedding.patch` | 嵌入所需的引擎改动，源自参考项目 AIStudio2API 的补丁（`Mag1cFall/firefox` 提交 `e297ff009d`），由本项目移植到 158.0.1 并维护：默认区域覆盖（`mozjs_set_camou_locale`，只替换默认区域，`new Intl.Locale("fr-FR")` 仍是它自己）、调用栈隐藏名为 `self-hosted` 的宿主脚本、按使用顺序暴露标准构造器、删除全局属性 |
| `patches/0002-static-msvcrt.patch` | 独立的 js 构建在 Windows 上一律用静态运行库（`/MT`），与 ais2api 一致 |
| `build-windows.cmd` | 入口：在 MozillaBuild 的 shell 里运行 `build-windows.sh`，所有下载、缓存和工具链都放在工作目录下 |
| `build-windows.sh` | 下载并校验源码包（官方 `SHA512SUMS`），安装 Rust，解压、打补丁、`mach configure` 和 `mach build`（构建目录按版本分开，`obj-win64-<版本>`），最后调用打包脚本 |
| `package-windows.sh` | 把构建结果整理成产物目录并写 `BUILDINFO.json` |

## Windows 上构建

需要：

- 一个路径短、没有空格的工作目录，例如 `E:\sm`。约占 18 GB（`mozbuild` 约 12 GB，解压的源码约 4 GB，构建目录约 1.2 GB，产物约 420 MB）。
- 把 [MozillaBuild](https://wiki.mozilla.org/MozillaBuild) 装到 `<工作目录>\mozilla-build`。

其余工具都由构建自己下载：clang-cl、Visual Studio 与 Windows SDK 的文件由 `--enable-bootstrap` 放进 `<工作目录>\mozbuild`，不依赖本机装了哪个版本的 Visual Studio；Rust 1.95.0（Firefox 158 的 CI 所用版本，见源码的 `taskcluster/kinds/toolchain/rust.yml`）由 `rustup` 装到 `<工作目录>\cargo` 和 `<工作目录>\rustup`。为旧版本准备过的工作目录会在旁边装上新版本，并用 `RUSTUP_TOOLCHAIN` 固定给 `mach` 用。

```bat
tools\spidermonkey\build-windows.cmd E:\sm
```

产物在 `<工作目录>\dist\windows-x64`：

```
include/mozjs.h        接口层的 C 接口
lib/mozjs_shim.lib     接口层（waa/third_party/aistudio2api/mozjs/mozjs.cpp）
lib/js_static.lib      SpiderMonkey 的 C++ 部分
lib/jsrust.lib         SpiderMonkey 的 Rust 部分
BUILDINFO.json         版本、源码包与补丁的摘要、工具链、各文件的 SHA-256、要链接的库
```

然后构建 ais2api：

```bat
cmake -S . -B build/windows-sm -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DAIS2API_SPIDERMONKEY=ON -DAIS2API_SPIDERMONKEY_DIR=<产物目录>
cmake --build build/windows-sm
```

CMake 会核对 `BUILDINFO.json` 里接口层两个文件的 SHA-256 与仓库中 `waa/third_party/aistudio2api/mozjs/` 的是否一致。

这样的构建默认同时开启 `AIS2API_CURL_IMPERSONATE`，即 SpiderMonkey 后端的网络层，见 [SpiderMonkey 后端](../../docs/browser.md#spidermonkey-后端实验性原型)：

- 配置时下载 `scripts/curl-impersonate.cmake` 锁定的 curl-impersonate 并校验 SHA-256；
- 构建后把 `libcurl-impersonate.dll` 复制到程序旁边；
- 不能联网时，用 `-DAIS2API_CURL_IMPERSONATE_DIR=<已解压的包>` 指定本地的包。

## 说明

- Mozilla 生成的 `js_static.lib` 是 thin archive，只记录目标文件的路径，MSVC 的链接器读不了；打包脚本用构建自带的 `llvm-lib` 按同一份目标文件列表重新打成完整的静态库。
- 接口层用构建自带的 clang-cl 编译，参数与引擎一致（C++20、关闭 RTTI 和异常、`STATIC_JS_API`、`JS_STANDALONE`、`XP_WIN`、`/MT`）。ais2api 自己仍用 MSVC 编译，只通过 `mozjs.h` 的 C 接口调用；接口层回调的 `mzGoNative` 由 `waa/src/js.cpp` 定义。
- 引擎和 Rust 部分的目标文件都带 `/FAILIFMISMATCH:RuntimeLibrary=MT_StaticRelease`，与主程序的 `/MT` 相符，不需要改成 DLL。
- 要链接的系统库取自同一次构建里 js shell 的链接列表（`js/src/shell/backend.mk`）。
- 源码包用 `xz -dc | tar -xf -` 解压，不用 `tar -xJf`：后者由 tar 自己 fork 出 xz，在 MSYS2 下会随机失败（子进程里的 DLL 加载到了不同地址）。
- `msys2_shell.cmd` 自己解析参数，命令里嵌套的引号会被拆坏，所以 `build-windows.cmd` 先切换到本目录，再以相对路径运行脚本。
- 2026-11-12 在本机编出了 152.0.4 的产物。2026-11-14 用 `build-windows.cmd` 编出了 158.0.1（clang 22.1.8、Rust 1.95.0、MSVC 14.51），`ais2api_tests` 全部通过（预期失败的一项除外）。这次复用了已有的 `mozbuild` 和 `cargo`，所以还不算在全新工作目录里完整跑过。
- Linux 版（WSL）的 `build-linux.sh` 之后再加。
