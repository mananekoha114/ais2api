# SpiderMonkey 构建配方

SpiderMonkey 后端（`browser_backend: "spidermonkey"`）在进程内运行 Google 的 WAA 虚拟机。本目录存放将 Firefox 158.0.1 的 JavaScript 引擎编译为 `ais2api` 静态库的跨平台构建配方；独立的四平台预编译产物与自编译静态 `curl-impersonate` 产物同时由仓库 [`mananekoha114/ais2api-spidermonkey`](https://github.com/mananekoha114/ais2api-spidermonkey) 的 GitHub Actions 自动构建并发布。

| 文件 | 作用 |
|---|---|
| `mozconfig-windows` | `windows-x64` 构建选项：仅编译 JS 引擎，优化、无断言，静态库，`clang-cl`，静态运行库（`/MT`） |
| `mozconfig-linux-x64` | `linux-x64` 构建选项：基于 Ubuntu 24.04（`glibc 2.39`）与引导的 Clang/LLD，编译 `-fPIC` 静态库 |
| `mozconfig-linux-arm64` | `linux-arm64` 构建选项：基于 Ubuntu 24.04 ARM64（`glibc 2.39`）与引导的 Clang/LLD，编译 `-fPIC` 静态库 |
| `mozconfig-android-arm64` | `android-arm64` 构建选项：基于 Android NDK r27 LTS（`aarch64-linux-android24`），面向原生 Termux Bionic 编译 `-fPIC` 静态库 |
| `patches/0001-aistudio2api-embedding.patch` | 嵌入所需的引擎改动，源自参考项目 AIStudio2API 的补丁（`Mag1cFall/firefox` 提交 `e297ff009d`），移植到 158.0.1 并维护：默认区域覆盖（`mozjs_set_camou_locale`，只替换默认区域，`new Intl.Locale("fr-FR")` 保持不变）、调用栈隐藏名为 `self-hosted` 的宿主脚本、按使用顺序暴露标准构造器、删除全局属性 |
| `patches/0002-static-msvcrt.patch` | 独立的 `js` 构建在 Windows 上统一使用静态运行库（`/MT`），与 `ais2api` 一致 |
| `patches/0003-android-ndk-r27-standalone-js.patch` | 独立 `js` 构建在 Android 上允许使用 Android NDK r27 LTS（Clang 18、API 24），并跳过 APK 专用的 `BionicGlue.cpp` |
| `build-windows.cmd` / `build-windows.sh` | Windows 构建入口：在 MozillaBuild 4.2.1 shell 中下载校验源码包（官方 `SHA512SUMS`）、安装 Rust 1.95.0、打补丁、执行 `mach configure` 与 `mach build`，并调用打包脚本 |
| `package-windows.sh` | 将 Windows 构建结果重打包为完整静态库、编译 `mozjs_shim.lib`、附带许可证与 `BUILDINFO.json`（可选输出 `.tar.xz`） |
| `build-unix.sh` / `package-unix.sh` | Linux 与 Android NDK 构建及打包脚本：重打包完整非 thin 的 `libjs_static.a`、编译 `-fPIC` 的 `libmozjs_shim.a`、附带许可证与 `BUILDINFO.json`（可选输出 `.tar.xz`） |
| `licenses/` | 随预编译归档分发的许可证文本（MPL-2.0、ICU Unicode v3 与第三方组件声明） |

## Windows 构建（`windows-x64`）

准备：

- 一个路径短、不含空格的工作目录，例如 `D:\sm`（约占 18 GB：`mozbuild` 约 12 GB，解压源码约 4 GB，构建目录约 1.2 GB，产物约 420 MB）。
- 将 [MozillaBuild](https://archive.mozilla.org/pub/mozilla/libraries/win32/MozillaBuildSetup-4.2.1.exe) 安装到 `<工作目录>\mozilla-build`。

其余工具（`clang-cl`、Visual Studio / Windows SDK sysroot、Rust 1.95.0）均由构建脚本自动下载到 `<工作目录>` 下：

```bat
tools\spidermonkey\build-windows.cmd D:\sm
```

产物位于 `<工作目录>\dist\windows-x64`。

## Linux 与 Android NDK 构建（`linux-x64` / `linux-arm64` / `android-arm64`）

在 Ubuntu 24.04（x64 或 ARM64）主机安装基础构建包（`build-essential curl lld m4 patch pkg-config python3 python3-pip python3-venv unzip xz-utils zlib1g-dev`；交叉编译 `android-arm64` 还需主机 `clang` 与 `ANDROID_NDK_HOME` 指向 Android NDK r27c）：

```sh
# Linux x64 / Linux ARM64
sh tools/spidermonkey/build-unix.sh linux-x64 /tmp/sm /tmp/spidermonkey-158.0.1-linux-x64.tar.xz

# Android ARM64（Termux 原生 Bionic API 24）
ANDROID_NDK_HOME=/path/to/android-ndk-r27c \
  sh tools/spidermonkey/build-unix.sh android-arm64 /tmp/sm /tmp/spidermonkey-158.0.1-android-arm64.tar.xz
```

## 产物目录结构

```text
include/mozjs.h                    C 嵌入接口层头文件
lib/mozjs_shim.lib | libmozjs_shim.a   C 嵌入接口层静态库（waa/third_party/aistudio2api/mozjs/mozjs.cpp）
lib/js_static.lib  | libjs_static.a    SpiderMonkey C++ 与 ICU 完整静态库（非 thin archive）
lib/jsrust.lib     | libjsrust.a       SpiderMonkey Rust 静态库
licenses/LICENSE                   Mozilla Public License 2.0
licenses/ICU-LICENSE.txt           Unicode License v3
licenses/THIRD_PARTY_NOTICES.txt   第三方组件许可证声明
BUILDINFO.json                     版本、源码与补丁摘要、工具链、各文件 SHA-256 与链接参数列表
```

## 链接到 `ais2api`

默认配置下，CMake 会自动下载 `scripts/spidermonkey.cmake` 与 `scripts/curl-impersonate.cmake` 锁定的四平台静态预编译包并校验 SHA-256，编译出的 `ais2api` 单文件可执行程序不携带任何 `.dll` 或 `.so` 运行库。离线或使用本地构建产物时，可通过 `-DAIS2API_SPIDERMONKEY_DIR=<目录>` 与 `-DAIS2API_CURL_IMPERSONATE_DIR=<目录>` 指定已解压目录：

```bat
cmake -S . -B build/windows-sm -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DAIS2API_SPIDERMONKEY=ON -DAIS2API_SPIDERMONKEY_DIR=D:/sm/dist/windows-x64
cmake --build build/windows-sm
```

CMake 配置阶段会核对 `BUILDINFO.json` 中 `mozjs.h` 与 `mozjs.cpp` 的 SHA-256 与仓库内 `waa/third_party/aistudio2api/mozjs/` 完全一致。

## 说明

- Mozilla 默认生成的 `js_static.lib` / `libjs_static.a` 为 thin archive（仅记录目标文件路径），打包脚本使用同工具链的 `llvm-lib` / `llvm-ar` 按目标文件列表重新归档为完整静态库并校验 `!<arch>` 魔数。
- 接口层 `mozjs.cpp` 使用 SpiderMonkey 构建工具链编译（C++20、关闭 RTTI 与异常、`STATIC_JS_API`、`JS_STANDALONE`；Windows 下 `/MT`，Linux / Android 下 `-fPIC`），与主程序仅通过纯 C ABI 边界（`mozjs.h` 与 `mzGoNative`）交互，隔离 MSVC / `libstdc++` / NDK `libc++` 的 C++ ABI 差异。
- 源码包使用 `xz -dc | tar -xf -` 解压，避免 MSYS2 下 `tar -xJf` 子进程 fork 偶发失败。
