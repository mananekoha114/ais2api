# 第三方软件及参考来源

ais2api 的自有实现采用 [MPL 2.0](LICENSE)。该许可不替换第三方软件已有的许可或版权声明。

## 构建依赖

桌面依赖的源码版本及摘要由 `vcpkg.json` 中固定的 baseline 和对应 port 锁定。主要依赖包括：

| 依赖 | 许可 |
|---|---|
| Boost.Asio / Beast / JSON 及 Boost 组件 | Boost Software License 1.0 |
| Catch2（开发测试） | Boost Software License 1.0 |
| libcurl | curl license |
| OpenSSL | Apache License 2.0 |
| SQLite | 上游 public-domain dedication |
| libarchive | 上游 BSD 及各组件对应条款 |
| Zstandard (zstd) | BSD License |
| zlib 等传递依赖 | 各自的上游许可 |

此表不是传递依赖的完整清单。桌面 CMake 安装和 CPack 流程会保留 vcpkg 提供的 `copyright` 文件，位置为 `share/licenses/ais2api-dependencies/`。修改依赖或分发方式时，应一并检查对应声明。Termux 系统依赖由其包管理器分发，不属于本仓库的 MPL 源码。

Termux 后端包保留编入程序的代码（Boost、libc++ 及实验静态归档）所属软件包和程序实际链接的动态库所属软件包提供的 `share/doc/PACKAGE/copyright`，归入包内的 `share/licenses/ais2api-dependencies/PACKAGE/copyright`；仅用于构建的工具软件包不随包分发，不收录其声明。动态运行库由 Termux 包管理器和 Android 系统提供，不随后端包捆绑；构建时的包版本与实际动态依赖记录在 `share/ais2api/termux-build.json`。Boost 软件包的声明涵盖其头文件。使用 `-fexperimental-library` 链接的 `libc++experimental.a` 属于 LLVM 运行库；包内保留其提供者 `ndk-sysroot` 的版权和许可声明，并记录该静态归档的摘要。上述声明不将这些第三方组件改为 MPL 2.0。

部分拆分的 Termux 动态库包未提供版权文件时，产物中的 `dependency_notices` 将其明确记为 `external-system-package`，表示由系统包管理器另行分发；可取得的声明仍保留。此项不适用于嵌入程序的 Boost/libc++ 代码及实验静态归档，它们的声明必须存在。

## Web 管理界面

可选 WebUI 的源码位于 `webui/`，构建依赖及完整性摘要由 `webui/package-lock.json` 锁定。嵌入程序的 Vue、Vue Router 与 Microsoft Fluent System Icons 均采用 MIT 许可；原始版权声明和许可文本保留在 `webui/public/THIRD_PARTY_LICENSES.txt`，同时嵌入 `/THIRD_PARTY_LICENSES.txt` 并安装到 `share/licenses/ais2api-webui/`。Vite、TypeScript、vue-tsc 等仅用于构建，不作为运行服务依赖。

## 浏览器

CloakBrowser/Chromium 二进制不纳入后端源码仓库。精简包按锁定清单从官方渠道下载浏览器；可选 `Browser-Local` 包供本机个人或内部使用，包含未修改的浏览器及其单独许可。浏览器没有改为 MPL 2.0。

官方 [CloakBrowser Binary License](https://github.com/CloakHQ/CloakBrowser/blob/main/BINARY-LICENSE.md) 允许个人及内部使用，向第三方再分发、嵌入产品或提供相应服务需要另行授权。内置浏览器的本机包不应作为普通公开发行包上传。浏览器归档原有版权和许可证保持原样。

本仓库保存了 2026-10-20 获取的官方许可原文 [CloakBrowser-BINARY-LICENSE.md](licenses/CloakBrowser-BINARY-LICENSE.md)，SHA-256 为 `a959b6f9db58f7e273694368659140e9d82960d964ab48b5f6cf9c4545cc2981`。它作为第三方许可文件按原文保留。打包工具从锁定原始附件重新提取浏览器、校验每个文件并附带该许可，不打包已有浏览器资料目录。

## Firefox 身份生成

Camoufox 后端的身份生成器（`src/fpgen.cpp`、`src/firefox_identity.cpp`）是两个上游项目的 C++ 移植，许可文本保存在 `licenses/`，并随运行包安装到 `share/doc/ais2api/licenses/`：

- [fpgen](https://github.com/scrapfly/fingerprint-generator) 1.3.0（Apache License 2.0，[fpgen-LICENSE.txt](licenses/fpgen-LICENSE.txt)）：`src/fpgen.cpp` 按其 Python 实现移植了贝叶斯网络的读取、beam search 条件分布和采样。fpgen 的模型数据不纳入仓库和正式运行包，由 `ais2api setup` 从其 GitHub Release 下载，并按 `runtime-manifest.json` 中锁定的 SHA-256 校验；[开发版](docs/platforms.md#构建开发版)在构建时按同样的 SHA-256 校验后把这份模型嵌入程序。
- [Camoufox](https://github.com/daijro/camoufox) Python 启动器 0.5.9（MIT License，Copyright (c) 2024-2026 daijro and the Camoufox contributors，[camoufox-launcher-LICENSE.txt](licenses/camoufox-launcher-LICENSE.txt)）：`src/firefox_identity.cpp` 移植了启动器组装身份、修正几何与核数、抽取字体、语音、媒体设备和 WebGL、检查身份一致性的逻辑。`browser/camoufox/` 中的 JSON 目录数据是该启动器安装包中的原文件，未作修改，`catalog.json` 记录其版本和 SHA-256，构建时嵌入程序。

移植部分的许可不因 ais2api 的许可选择而改变。Camoufox 浏览器本身（MPL 2.0）不纳入仓库和正式运行包：`setup` 从官方 GitHub Release 下载锁定版本，跳过自带的字体（保留 emoji 字体）和 uBlock Origin 后安装在数据目录中，其余文件保持原样。开发版运行包在 `bin/camoufox-runtime/` 中附带同一锁定版本，裁剪方式相同，其余文件同样未作修改；其源码见 [Camoufox 仓库](https://github.com/daijro/camoufox) 中与 `runtime-manifest.json` 所锁定版本同名的标签，MPL 2.0 全文与本项目的 [LICENSE](LICENSE) 相同。

## 原理参考

[aistudio-api-termux](https://github.com/ReedSein/aistudio-api-termux/tree/84b50663fcae79058fe33e0e45b5c3452da7c461) 用于理解行为和私有协议事实。其本身采用 MIT 许可，版权归其贡献者；该许可不因 ais2api 的许可选择而改变。

[AIStudio2API](https://github.com/Mag1cFall/AIStudio2API/tree/80847a7665c442a238918c957963217773f36291)（MIT License，Copyright (c) 2026 Mag1cFall）是 Camoufox 后端的参考。按账号语言与时区改写 Camoufox 配置的键（现位于 `src/firefox_identity.cpp`），以及 `src/camoufox.cpp` 中 `CAMOU_CONFIG_N` 的分段大小、BiDi 端点的读取方式和 Firefox 首选项清单，参照了其 `internal/camoufoxnative`（Go）的做法，由本项目用 C++ 重新编写；该许可不因 ais2api 的许可选择而改变。

除上文“Firefox 身份生成”列出的移植代码和目录数据外，本仓库的 C++ 实现、网页桥接脚本及回归样例独立编写，不包含参考项目的 Python/JavaScript 实现文件或测试实现。本地 `.analysis/` 克隆、依赖 checkout 和生成文件不纳入 Git 提交。如果后续引入任何第三方源码，应逐文件保留其许可和版权声明，并更新本说明。
