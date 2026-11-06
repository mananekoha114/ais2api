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

## 原理参考

[aistudio-api-termux](https://github.com/ReedSein/aistudio-api-termux/tree/84b50663fcae79058fe33e0e45b5c3452da7c461) 用于理解行为和私有协议事实。其本身采用 MIT 许可，版权归其贡献者；该许可不因 ais2api 的许可选择而改变。

[AIStudio2API](https://github.com/Mag1cFall/AIStudio2API/tree/80847a7665c442a238918c957963217773f36291)（MIT License，Copyright (c) 2026 Mag1cFall）是 Camoufox 后端的参考。`src/camoufox.cpp` 中按账号语言与时区改写 Camoufox 配置的键、`CAMOU_CONFIG_N` 的分段大小、BiDi 端点的读取方式和 Firefox 首选项清单参照了其 `internal/camoufoxnative`（Go）的做法，由本项目用 C++ 重新编写；该许可不因 ais2api 的许可选择而改变。

本仓库的 C++ 实现、网页桥接脚本及回归样例独立编写，不包含参考项目的 Python/JavaScript 实现文件或测试实现。本地 `.analysis/` 克隆、依赖 checkout 和生成文件不纳入 Git 提交。如果后续引入任何第三方源码，应逐文件保留其许可和版权声明，并更新本说明。
