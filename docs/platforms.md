# 构建与打包

本文说明各平台的构建、安装、打包与验证方法。只想使用程序时，直接下载 CI 运行包即可，见 [快速开始](getting-started.md)。

## 支持的平台

| 平台 | 工具链 |
|---|---|
| Windows 10/11 x64 | MSVC（Visual Studio C++ 工具与 Windows SDK） |
| Ubuntu 24.04 / Debian 13 x86_64 | GCC 或 Clang |
| Ubuntu 24.04 / Debian 13 ARM64（只能用 Camoufox 后端） | GCC 或 Clang |
| Android ARM64（Termux 宿主） | Termux Clang |

通用要求：CMake 3.25 及以上、C++20、Ninja、Git。桌面依赖由 `vcpkg.json` 中固定的 baseline（`664933b8d385dfc01f4e0cdff4c7c74e71f6a740`）锁定，源码下载由 vcpkg 校验；首次构建可能较慢。桌面构建或 GitHub Actions 任务的成功不代表 Android 上的行为已经验证。

服务本身是纯 C++ 程序。Node.js 与 Python 只是可选的开发测试工具（构建 WebUI 时需要 Node.js）。Termux 官方的 proot-distro 自身依赖 Python 与 python-pip，这属于浏览器容器管理工具的依赖，而不是本服务的依赖，见 [Termux proot-distro 配方](https://github.com/termux/termux-packages/blob/master/packages/proot-distro/build.sh)。

## Windows x64

安装 Visual Studio C++ 构建工具（含 Windows SDK）、Git、CMake 与 Ninja，并确保 CMake、CTest、Ninja 在 PATH 中。在 PowerShell 中执行：

```powershell
./scripts/build.ps1 -Package          # 加 -WebUI 嵌入网页控制台
```

脚本会在需要时导入已安装的 Developer PowerShell，使用 `x64-windows-static` triplet，检查 vcpkg baseline，然后构建并运行 CTest。安装目录为 `build/windows/install`，ZIP 位于 `build/windows/packages`。

| 参数 | 说明 |
|---|---|
| `-VcpkgRoot PATH` | 复用已有的、处于固定版本的 vcpkg checkout（不会重置它） |
| `-BuildDirectory PATH` | 使用独立的构建目录 |
| `-Triplet x64-windows-static-release` | 只构建 Release 依赖，冷构建时间约减半；仅用于非 Debug 配置，CI 使用此项 |
| `-Configuration` | `Release`（默认）、`Debug`、`RelWithDebInfo` |
| `-Jobs N` | 并行数，默认 2 |
| `-WebUI` | 构建并嵌入 WebUI |
| `-BrowserBundleDirectory PATH` | 打包本地浏览器，见下文 |
| `-SkipTests` | 跳过测试 |

`build.ps1` 和 `build.sh` 只做正式版构建：构建目录如果已经被配置为开发版（见 [构建开发版](#构建开发版)），脚本会报错退出。

项目与 vcpkg triplet 都使用 `/MT` 静态运行库，运行包不依赖单独安装的 MSVC 运行库。

也可以使用 CMake 预设（需先在 `.tools/vcpkg` 准备固定版本的 vcpkg）：

```powershell
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout 664933b8d385dfc01f4e0cdff4c7c74e71f6a740
.tools/vcpkg/bootstrap-vcpkg.bat -disableMetrics
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

## Ubuntu 24.04 / Debian 13 x86_64

用系统包管理器安装构建前提，然后执行构建脚本：

```sh
sudo apt-get update
sudo apt-get install -y build-essential clang cmake ninja-build git curl zip unzip tar pkg-config autoconf automake libtool
sh scripts/build.sh linux --package       # 加 --webui 嵌入网页控制台
```

脚本会获取 `vcpkg.json` 中指定提交的 vcpkg，完成配置、构建和 CTest，安装到 `build/linux/install`，并在 `build/linux/packages` 生成 TGZ。在 ARM64（aarch64）上脚本使用 `arm64-linux` 三元组；CloakBrowser 没有 Linux ARM64 版本，这里只能使用 Camoufox 后端（见下文），也不能打包本地浏览器。新的依赖工具放在 `build/deps`；脚本不会把已有的 checkout 更新到其他提交。要复用已准备好的工具，把 `VCPKG_ROOT` 设为处于固定版本的 checkout。依赖下载仍需要网络或已填充的 vcpkg 缓存。

`scripts/build.sh` 的选项：

| 选项 | 说明 |
|---|---|
| `--package` | 测试后生成 CPack TGZ |
| `--webui` | 构建并嵌入 WebUI（需要 Node.js 20.19+ 或 22.12+） |
| `--browser-bundle PATH` | 打包已校验的本地浏览器（仅桌面 x86_64） |
| `--sanitizers` | 启用 ASan/UBSan（仅桌面 Linux） |
| `--fuzzer` | 构建 Clang libFuzzer 目标（同时启用 sanitizer） |
| `--skip-tests` | 不构建测试目标 |
| `--build-dir PATH` | 默认 `build/linux` 或 `build/termux` |
| `--jobs N` | 默认 2，Termux 默认 1 |
| `--install-deps` | 安装 Termux 宿主包（会修改共享包） |

环境变量：`CC`、`CXX`、`VCPKG_ROOT`、`BUILD_TYPE`。

### 正常使用

```sh
build/linux/install/bin/ais2api init
build/linux/install/bin/ais2api setup --yes --headless=true
build/linux/install/bin/ais2api doctor
```

- 以普通用户运行服务；`setup --yes` 会通过 `sudo` 安装列出的系统包，需要可用的 sudo 认证会话或交互终端。
- Debian 13 与 Ubuntu 24.04 使用 `t64` 版本的浏览器依赖包名；其他发行版或版本会被拒绝，而不是猜测包名。
- 无界面浏览器准备不需要 WSLg、桌面会话或 Windows Chrome。`setup` 只验证本地 CDP 与隔离上下文的创建，不登录 Google。
- Debian 构建的可执行文件面向 Debian 的 glibc ABI，不能替代 Ubuntu 运行包。

### Camoufox 后端

默认后端为 `camoufox`，或者有账号使用 Firefox 后端时，`setup` 会安装 Camoufox：

```sh
build/linux/install/bin/ais2api setup --yes
```

- 系统包：`setup` 先检查下列包，只安装缺少的。缺包时需要 `--yes`，通过 `sudo apt-get` 安装；都已安装时既不询问也不运行 apt，结果中的 `system_packages.status` 为 `ready`。
  - 包列表：`xvfb`、`libgtk-3-0t64`、`libx11-xcb1`、`libasound2t64`、`libdbus-1-3`、`libgl1-mesa-dri`、`libegl1`、`fontconfig`、`ca-certificates`。
  - 在其他发行版上跳过这一步（`status` 为 `not_checked`），需要自己安装对应的包。缺什么，验证 Camoufox 时会报出来。
- 显示：Linux 上的 Camoufox 总是以有界面模式运行。
  - `headless=true`（默认）：每次启动浏览器时开一个 Xvfb 虚拟显示（1×1 像素，Mesa 软件 OpenGL），浏览器关闭时一起停止。所以在 Linux 上，`headless` 表示“用 Xvfb 虚拟显示”。
  - `headless=false`：使用桌面会话的显示（`DISPLAY` 或 `WAYLAND_DISPLAY`），都没有时返回 503 `display_unavailable`。
- 身份与字体：
  - 身份与 Windows 上一样呈现本机 OS，即 Linux。
  - 字体只用 Camoufox 自带的、Linux 身份会读取的四组字体（约 0.9 GB），通过 Camoufox 的 Linux fontconfig 读取，不使用系统字体。`setup` 只解压这几组字体和 Linux 的 fontconfig，安装后约 1.3 GB。
  - 字体缓存放在 `<data_dir>/runtime/camoufox-fontconfig/`，第一次启动时扫描一次。
- ARM64：CloakBrowser 对这个平台返回 `unsupported_platform`。要先把 `browser_backend` 设为 `camoufox`（或让账号使用 Firefox 后端），再运行 `setup`。

### WSL 2 中的 Debian 13

在 Windows 中用 `wsl.exe -d Debian` 进入发行版，在该 shell 中使用 Linux 编译器和程序。源码、构建目录、依赖和服务数据都应放在 Linux 文件系统内（例如 `~/src/ais2api`），而不是 `/mnt/c` 或 `/mnt/d`。这样数据目录和 Unix 控制套接字也能获得正常的 Linux 权限。参见 [WSL 文件系统建议](https://learn.microsoft.com/en-us/windows/wsl/interop)。

```sh
env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  sh scripts/build.sh linux --package --jobs 2
python3 tests/cli_smoke.py --exe build/linux/install/bin/ais2api
python3 scripts/check-package.py build/linux/packages/*.tar.gz
```

构建命令把 `PATH` 限定为 Linux 工具：WSL 继承的 Windows 路径会让 CMake 反复搜索工具，明显变慢。这不会修改全局 shell 或 WSL 设置；如有自定义 Linux 工具链目录，请自行加入。

### 真实浏览器集成测试

不安装系统包、也不访问 Google 的前提下，可以用已准备好的浏览器显式运行隐藏的集成测试：

```sh
AIS2API_TEST_BROWSER=/absolute/path/to/chrome \
  build/linux/ais2api_tests '[.browser-runtime]'
```

它使用临时资料目录，验证 CDP 与隔离上下文，并检查进程和资料目录的清理。可执行文件不存在时测试失败。普通 CTest 不包含此测试，不依赖已安装的浏览器。

### 其他编译器、sanitizer 与模糊测试

在独立的构建目录中选择其他编译器或 sanitizer：

```sh
CC=clang CXX=clang++ BUILD_TYPE=RelWithDebInfo sh scripts/build.sh linux --sanitizers --build-dir build/linux-clang-asan
CC=clang CXX=clang++ BUILD_TYPE=RelWithDebInfo sh scripts/build.sh linux --fuzzer --build-dir build/linux-fuzz
build/linux-fuzz/ais2api_fuzz_wire -max_total_time=30 -max_len=65536
```

`AIS2API_BUILD_FUZZER=ON` 需要桌面 Linux Clang 并同时启用 `AIS2API_SANITIZE=ON`，它会插桩核心库并把线路解析器测试链接到 LLVM libFuzzer。sanitizer 与短时模糊测试只能证明所覆盖的输入，不能证明不存在内存错误。

## ARM64 Termux

在 proot 之外进行原生构建，并把项目放在 Termux 私有存储内。`--install-deps` 会修改共享的 Termux 包：

```sh
pkg install boost-headers
CXXFLAGS=-fexperimental-library sh scripts/build.sh termux --install-deps --build-dir build/termux-native --jobs 1
```

- 不加 `--install-deps` 时使用已安装的依赖。官方包名为 `clang`、`cmake`、`ninja`、`git`、`curl`、`pkg-config`、`boost`、`boost-headers`、`libcurl`、`openssl`、`libarchive` 与 `libsqlite`，其中已包含原生 CMake 构建所需的头文件；不要替换为桌面的 `*-dev` 包名。`boost-headers` 是拆分包，脚本的依赖列表不包含它，需要单独安装。参考官方配方：[Clang](https://github.com/termux/termux-packages/blob/master/packages/libllvm/clang.subpackage.sh)、[Boost](https://github.com/termux/termux-packages/blob/master/packages/boost/build.sh)、[libcurl](https://github.com/termux/termux-packages/blob/master/packages/libcurl/build.sh)、[libarchive](https://github.com/termux/termux-packages/blob/master/packages/libarchive/build.sh)、[libsqlite](https://github.com/termux/termux-packages/blob/master/packages/libsqlite/build.sh)。
- 构建网页管理界面时，另行安装 `nodejs` 和 `npm`，并在构建命令中加入 `--webui`。它们仅用于构建，运行包不需要 Node.js 或单独的前端目录；不带该选项时仍可构建纯后端程序。
- 测试会从官方源码归档准备 [Catch2 3.16.0](https://github.com/catchorg/Catch2/releases/tag/v3.16.0)，并校验与固定 vcpkg port 相同的 SHA-512；准备目录为 `build/deps`。只构建生产程序时可以用 `--skip-tests`。不会把 vcpkg 的 Android 交叉编译当作原生 Termux 测试。
- 使用 Termux Clang 21 时，`std::osyncstream` 需要 `-fexperimental-library`（包括 `ndk-sysroot` 提供的实验性静态库）。上面的命令新建构建目录，使 CMake 读取 `CXXFLAGS`；对于已配置的目录，请通过 CMake 设置 `-DCMAKE_CXX_FLAGS=-fexperimental-library`，只改环境变量不会替换缓存的参数。

### 浏览器容器

C++ 服务运行在 Termux 宿主上，浏览器运行在 Ubuntu 用户空间中。`setup` 会创建项目专属的 proot Ubuntu 24.04 容器和启动器。当前官方 proot-distro 使用 OCI 镜像和命名容器，以下仅演示语法：

```sh
pkg install proot-distro
proot-distro install ubuntu:24.04 --name example-browser
proot-distro login example-browser -- /bin/true
# 仅为语法示例；实际的绑定启动器由 setup 创建：
proot-distro login example-browser --bind /absolute/host/path:/absolute/guest/path -- /bin/true
```

请使用 `--name` 而不是已废弃的 `--override-alias`。当前 rootfs 位于 `$PREFIX/var/lib/proot-distro/containers/NAME/rootfs`，官方工具会在登录时迁移旧的 `installed-rootfs/NAME`。不要用目录布局检查代替一次成功的容器登录。参见 [proot-distro 官方说明](https://github.com/termux/proot-distro/blob/master/README.md)。

### GitHub Actions 的 Termux 运行包

`.github/workflows/termux.yml` 在 `ubuntu-24.04-arm` 上、官方 Termux ARM64 容器内构建当前提交（包括 PR 的合并预览）。镜像按摘要固定；滚动更新的 apt 依赖会一起升级，并记录实际版本——这是有记录的构建环境，而不是完整的依赖快照。它复用 `scripts/build.sh termux --webui` 和 CMake 安装清单，运行 CTest 以及隔离的 CLI / 便携目录冒烟测试（不访问 Google）。便携目录检查显式启用 `--require-webui`，在原位置和移动到含空格目录后验证首页、登录路由及页面引用的 JS/CSS，缺失网页或资源时失败。参见 [Termux 官方容器](https://github.com/termux/termux-docker/blob/master/README.md)。

该工作流与桌面工作流一样在 push 与 PR 时运行，也可以在 Actions 中选择 **Termux ARM64** 手动运行（GitHub 要求工作流文件已存在于默认分支，见 [手动运行工作流](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)）。

产物 `termux-aarch64-COMMIT` 包含 `.tar.gz` 运行包（内嵌网页管理界面）及其 `.sha256`，保留 28 天。请解压到 Termux 私有存储中的新目录（Android 共享存储不能作为可执行安装目录），使用前先校验：

```sh
sha256sum --check ais2api-termux-aarch64-COMMIT.tar.gz.sha256
python scripts/check-package.py --target termux-aarch64 ais2api-termux-aarch64-COMMIT.tar.gz
```

- 运行包内的 `share/ais2api/termux-build.json` 记录源码提交、镜像摘要、包版本、ELF 加载器和链接库清单。动态库由 Termux 包管理器另行提供，Android 提供系统库；包版本描述的是构建环境，不保证兼容所有旧版本。
- 运行包不包含配置、账号、系统库或浏览器。依赖声明位于 `share/licenses/ais2api-dependencies/PACKAGE/copyright`，包括链接的实验性静态库所属 LLVM 的声明。部分拆分的动态库包不带版权文件，其 `dependency_notices` 条目记为 `external-system-package`；这只适用于另行安装的共享库，嵌入的 Boost/libc++ 代码与实验性静态库的声明必须存在。
- 校验器会拒绝错误的 CPU 或加载器、缺少运行文件、未报告的依赖、缺少适用声明、捆绑的共享库以及已知的私有路径。
- 替换已有安装前，请先在目标设备上用临时配置、数据目录和独立端口试运行。容器任务成功不代表 Google 请求、Android 浏览器集成或后台存活已经验证。

### 设备验收清单

Termux 的验收必须在真实 ARM64 Android 设备上完成：原生编译与 CTest；运行时准备与浏览器 `--version`；使用授权测试账号进行账号探测；真实的文本与多模态生成；增量流式、客户端断开与取消；账号切换；服务重启；下载/准备中断后的恢复；以及在该设备电池策略下的后台存活。请保留平台与工具版本、命令、退出码和脱敏后的结果。本文档不声称这些检查已经完成。

## 安装与运行包结构

两种桌面构建都支持标准 CMake 操作（Windows 使用 `build/windows`）：

```sh
cmake --install build/linux --prefix out/linux
cmake --build build/linux --target package
```

运行包内容：

- `bin/ais2api`（或 `.exe`）、`bin/browser/bridge.js`、`bin/runtime-manifest.json`、`bin/userscript/`。
- 根目录的一键启动脚本：Linux 与 Termux 为可执行的 `start.sh`，Windows 为 `start.cmd`。
- `share/ais2api/schemas/`、`share/doc/ais2api/`（许可证与文档）、桌面 vcpkg 依赖的版权文件，以及运行包的 SHA-256 旁文件。
- 不包含 `.tools`、源码分析副本、测试程序、账号数据、浏览器下载和配置密钥。CPack 使用 CMake 中显式的安装清单，ZIP/TGZ 由其归档生成器生成（见 [CPack Archive](https://cmake.org/cmake/help/latest/cpack_gen/archive.html)）。

关于一键启动脚本的实现细节：`start.sh` 在运行可能安装系统包的 `setup --yes` 前会询问（非交互 shell 中拒绝）；Windows 的 setup 只下载浏览器，因此 `start.cmd` 不询问。`start.cmd` 切换到 UTF-8 代码页 65001 以便在 GBK 系统上显示中文，退出时恢复；由于 65001 下 cmd 会误读含多字节字符的批处理文件，`start.cmd` 必须保持纯 ASCII 和 CRLF 换行，提示文字放在 `share/ais2api/start-messages.txt` 中，校验器会检查这两点。服务未运行时，Windows 上的 `init` 和 `start` 各自会等待约 5 秒确认本地控制管道，因此首次启动会多花几秒。

Linux 可执行文件依赖目标系统的 ABI：Ubuntu 构建的程序不保证能在更旧的 glibc、musl 或 Android 上运行。可在目标机器上用 `ldd bin/ais2api` 检查依赖。

### 校验运行包

```sh
python3 scripts/check-package.py PATH_TO_ARCHIVE
```

在不解压的情况下检查必需的运行文件、依赖许可证、SHA-256，以及是否混入已知的私有或构建路径；可执行文件必须是正式版构建，包中不能含有 Camoufox。需要 Python 3.11+（仅开发使用）。

## 便携数据与本地浏览器包

默认情况下，可执行文件所在目录拥有 `config.json` 和 `data/`：下载、SQLite、准备日志、服务日志和浏览器资料都在 `data/` 中。配置中保存的本地路径相对于配置文件，因此可以整体移动目录；显式的外部路径继续有效。程序目录不可写时直接失败，而不是改写到用户目录。浏览器子进程使用私有的 HOME/XDG/AppData/temp/cache/log 路径，不影响父进程环境。系统包管理器、系统服务以及 Termux/proot 容器状态由各自的工具管理。

### 构建 Browser-Local 包

`Browser-Local` 包附带经过校验的浏览器，仅供本人或内部使用，受 [CloakBrowser 二进制许可](https://github.com/CloakHQ/CloakBrowser/blob/main/BINARY-LICENSE.md) 约束。**不要把带浏览器的包作为不受限制的公开发行版上传。** 用于整理和检查的 Python 脚本是开发工具，不是运行依赖。

从 SHA-256 已锁定在 `runtime-manifest.json` 中的原始归档开始：

```sh
python3 scripts/browser-bundle.py stage \
  --archive /path/to/cloakbrowser-linux-x64.tar.gz --platform linux-x64 \
  --output build/browser-bundle-linux \
  --license-file licenses/CloakBrowser-BINARY-LICENSE.md
sh scripts/build.sh linux --package --browser-bundle build/browser-bundle-linux
python3 scripts/check-package.py --with-browser build/linux/packages/*Browser-Local.tar.gz
```

Windows 使用官方 ZIP 和 `--platform windows-x64`，然后：

```powershell
./scripts/build.ps1 -Package -BrowserBundleDirectory build/browser-bundle-windows
python scripts/check-package.py --with-browser build/windows/packages/*Browser-Local.zip
```

- 同时保留两种运行包时请使用不同的构建目录。CMake 的等价参数为 `-DAIS2API_BROWSER_BUNDLE_DIR=/absolute/clean/staged/runtime`。
- 配置和安装阶段会校验浏览器目录的完整文件清单；CPack 只包含该浏览器目录，并在归档名后加 `-Browser-Local`。含有资料目录、配置或下载缓存的目录会被拒绝，运行时数据库和 API Key 不会被打包。
- `init` 与 `start` 会自动使用程序旁的锁定浏览器；`setup` 可以校验每个文件并准备系统库。打包浏览器并不能免除 Linux 的系统依赖，也不会登录 Google。
- `Browser-Local` 不能与开发版同时构建，将随 CloakBrowser 退役一起删除。

### 构建开发版

开发版（CMake 选项 `AIS2API_DEV_BUNDLE`）嵌入 fpgen 模型，并附带锁定版本的 Camoufox，解压后不用再运行 `setup` 下载，新建的配置默认使用 `camoufox` 后端。开发版只在本机构建，供本人或内部使用，**不要作为公开发行附件上传**。支持 Windows x64 和 Linux x64：

```powershell
./scripts/build-dev.ps1 -Proxy http://127.0.0.1:7890   # 不需要代理时省略 -Proxy
```

```sh
sh scripts/build-dev.sh --yes --proxy http://127.0.0.1:7890   # 不需要代理时省略 --proxy
```

脚本依次完成：

1. 下载 `runtime-manifest.json` 锁定的 fpgen 模型并逐个文件核对，放在 `build/windows-dev/dev-bundle/fpgen/`；
2. 以 `-DAIS2API_DEV_BUNDLE=ON` 配置并构建（默认嵌入 WebUI，`-SkipWebUI` 跳过）；
3. 用刚构建的程序运行 `setup`，把 Camoufox 安装到 `build/windows-dev/dev-bundle/data/`，裁剪方式与正式版相同，并在无界面模式下用本地页面验证。第一次运行要下载约 1.3 GB，之后复用已经装好的版本；
4. 运行 CTest（`-SkipTests` 跳过）并安装；
5. 用一份全新的配置运行安装目录中的 `doctor`，确认使用打包的 Camoufox 和嵌入的模型，不需要 `setup`；
6. 生成 `build/windows-dev/packages/ais2api-<版本>-Windows-AMD64-Dev.zip`，并用 `check-package.py --dev` 检查。

其余参数与 `build.ps1` 相同（`-BuildDirectory`、`-VcpkgRoot`、`-Configuration`、`-Triplet`、`-Jobs`）。开发版必须使用单独的构建目录：脚本拒绝已配置为正式版的目录，`build.ps1` 也拒绝开发版的目录。

`build-dev.sh` 的步骤相同：构建目录默认为 `build/linux-dev`，生成 `ais2api-<版本>-Linux-x86_64-Dev.tar.gz`。`--yes` 允许 `setup` 安装 Camoufox 需要的系统包（见 [Camoufox 后端](#camoufox-后端)）。其他选项有 `--build-dir`、`--jobs`、`--skip-tests`、`--skip-webui`。

Camoufox 位于运行包的 `bin/camoufox-runtime/<版本>-<平台>/`。`doctor` 的 `build_mode` 为 `dev`，`camoufox.source` 为 `bundle`，`fpgen_model.source` 为 `embedded`。

## 持续集成

`.github/workflows/build.yml` 在 PR、推送到 `main` 和手动触发时运行：

| 任务 | 内容 |
|---|---|
| bridge | 脚本语法检查、启动器与打包校验器的单元测试、页面桥接与 userscript 的 Node 测试 |
| Ubuntu 24.04 / gcc | Release 构建（含 WebUI）、CTest、CLI 与便携目录冒烟、一键脚本冒烟、运行包校验与上传 |
| Ubuntu 24.04 / clang + sanitizer | ASan/UBSan 构建与测试、短时线路解析器模糊测试 |
| Debian 13 / GCC | 容器内构建（含 WebUI）、测试、冒烟与运行包上传 |
| Windows / MSVC x64 | `x64-windows-static-release` 构建（含 WebUI）、测试、冒烟与运行包上传 |

`.github/workflows/camoufox-runtime.yml` 只在 Camoufox 相关文件改动的 PR 中运行，也可以手动运行。每次运行要多次下载约 1.3 GB 的 Camoufox。它在 Ubuntu 24.04 x64 与 ARM64 上依次：

1. 构建并运行 CTest；
2. 用 `setup` 安装 Camoufox 及其系统包：先确认缺包时没有 `--yes` 会拒绝，装好后再运行一次，确认不再安装任何东西；
3. 在 Xvfb 下运行隐藏测试 `[.camoufox-runtime]`、`[.camoufox-fonts]`（与 `camoufox-dev.py fetch` 取得的完整版比较）和 `[.fpgen-model]`；
4. 用 camoufox Python 包的 `check-identities` 检查生成的身份；
5. 构建容器镜像，确认第一次启动时会安装并验证 Camoufox。

全程只打开本地页面，不使用账号，也不访问 Google。

非 sanitizer 任务上传的运行包为 `ubuntu24-gcc-runtime`、`debian13-gcc-runtime` 与 `windows-msvc-x64-runtime`，保留 28 天。vcpkg 二进制缓存按运行器镜像（或 Debian 工具链包）划分，因为 vcpkg 的 ABI 哈希包含编译器；有新构建的包时才保存缓存。工作流没有路径过滤，脚本或文档的改动也会触发完整检查。

工作流定义本身不等于远端运行成功。真实的 Google 登录、私有 RPC 兼容性、浏览器下载和 Android 进程生命周期仍需单独验收。Debian 容器任务不是 WSL 安装测试，本机 WSL 的结果另见 [验证记录](validation.md)。
