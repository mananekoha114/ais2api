# 发布流程

本文是维护者发布 ais2api 的操作清单。仓库为 [mananekoha114/ais2api](https://github.com/mananekoha114/ais2api)，默认分支 `main`。自有源码的版权署名为 Mana Nekoha (mananekoha114)，采用 MPL 2.0。

## 提交前检查

- 不要提交 Cookie、storage state、真实 API Key、浏览器资料目录、SQLite 数据库、原始请求/响应或个人日志。`.gitignore` 只是辅助措施，提交前用 `git diff --cached` 检查。
- `.analysis/`、`.tools/`、`build/`、`vcpkg_installed/`、本地账号文件与数据库不应被跟踪。
- 不要使用强制推送覆盖未知的远程历史；远程已有提交时先 fetch 并检查差异。
- Git 身份沿用本机配置。公开提交会公开作者信息；如需使用 GitHub noreply 地址，请在推送前调整本仓库的身份设置。

## 仓库设置建议

- 描述：`C++20 AI Studio gateway with Gemini and OpenAI-compatible APIs, Web console and CLI; Windows, Linux and Termux.`
- Topics：`cpp20`、`gemini`、`openai-compatible`、`aistudio`、`termux`、`mpl-2-0`。这些描述目标平台与接口，不代表所有能力都已通过真实验收。

## 持续集成

每次推送和 PR 都会运行 `.github/workflows/build.yml`（Windows、Ubuntu GCC/Clang sanitizer、Debian、bridge）与 `.github/workflows/termux.yml`。发布前确认对应提交的远端任务实际通过；未运行或失败的 CI 不能写成通过。各任务内容见 [构建与打包](platforms.md#持续集成)。

## 容器镜像

`.github/workflows/docker.yml` 在 amd64 与 arm64 原生 runner 上构建镜像并运行冒烟测试，推送到 `main`、标签或手动触发时发布 `linux/amd64` 与 `linux/arm64` 的 multi-arch manifest 到 `ghcr.io/mananekoha114/ais2api`。运行时浏览器（Camoufox）在第一次启动时下载到数据卷，不打入公共镜像，见 [Docker 部署](docker.md)。

## 发布二进制

1. 以 development / prerelease 标记当前版本，并在发行说明中列出尚未完成的账号、平台、性能和能力验证（见 [验证记录](validation.md)）。
2. 为发布提交打标签，上传与该提交一致的 CI 运行包（ZIP / TGZ）及其 SHA-256 旁文件。不要上传本机 `.tools/` 或 `data/runtime/` 目录。
3. 上传前运行 `python scripts/check-package.py PATH_TO_ARCHIVE` 检查清单和摘要，并确认附件中没有 Cookie 或个人数据。
4. MPL 覆盖的二进制须提供对应源码的获取方式：GitHub Release 应指向同一标签的源码并保留自动生成的源码归档，发行说明中写明对应的标签或提交，不能只发布二进制。
5. 运行包中保留 `LICENSE`、`NOTICE`、`THIRD_PARTY_NOTICES.md` 与依赖版权文件。依赖库的许可不因本项目而改变。

本清单只是本项目的操作流程，MPL 条款以仓库中的完整许可与 [Mozilla 官方文本](https://www.mozilla.org/MPL/2.0/) 为准。2026-10-20 首次上传前的范围与归属审查见 [报告](reports/publication-audit-2026-10-20.md)。
