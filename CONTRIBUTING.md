# 贡献指南

ais2api 是独立编写的 C++20 网关，目前仍为开发版本。开始之前建议阅读 [验证记录](docs/validation.md)、[路线图](docs/roadmap.md) 和 [协议合同](docs/protocol.md)。

## 代码结构

| 目录 | 内容 |
|---|---|
| `src/`、`include/ais2api/` | C++ 服务：HTTP、协议适配、存储、配置、CLI、管理 API |
| `browser/` | 浏览器网关（CDP、代理转发、重试策略）与页面桥接脚本 `bridge.js`、Build App 桥接脚本 `app_bridge.js` |
| `webui/` | Vue 3 管理控制台，构建时嵌入可执行文件 |
| `userscript/` | Tampermonkey Cookie 导入脚本 |
| `launcher/` | 运行包中的一键启动脚本 |
| `schemas/` | 配置、管理 API 与协议扩展的机器可读定义 |
| `scripts/` | 构建、打包与校验脚本 |
| `tests/`、`tools/` | Catch2 测试、Python 冒烟测试与开发工具 |

## 开发约定

- 使用 [构建与打包](docs/platforms.md) 中的构建入口，保持 `vcpkg.json` 的 baseline 与 `runtime-manifest.json` 锁定，不要在构建中追随 latest。
- 上游协议细节集中在协议适配层（`src/protocol.cpp`）；业务层不直接访问 JSPB 数组索引或网页 DOM。
- 新增的网络或流式路径应保留取消、资源限制和错误分类；不要通过丢弃内容块来限制内存。
- 测试样例独立编写。引用第三方实现时必须保留其许可并更新 `THIRD_PARTY_NOTICES.md`；不要把分析用的克隆直接复制进仓库。
- 代码格式遵循 `.clang-format`。新建源码文件添加与现有文件相同的 SPDX 和 MPL 声明。
- 修改配置项、接口或用户可见行为时，同步更新 `docs/` 中的对应文档与 `schemas/`。

## 验证

运行与改动相关的测试：

```text
ctest --preset windows                      # 或 build/linux 下的 ctest
node --test browser/test_bridge.cjs         # 页面桥接
node userscript/test_cookie_helper.cjs      # Cookie 导入脚本
python tests/cli_smoke.py --exe PATH        # CLI / IPC / HTTP 冒烟
cd webui && npm run typecheck && npm test   # WebUI
```

- 协议与流式改动需覆盖任意分块、错误和取消；环境准备相关的改动需覆盖中断与恢复。
- 涉及真实 Google 账号或真实生成的验证，应在测试账号上进行，结果写入 PR 说明或 `docs/reports/`。

## 提交 PR

PR 说明应写清：

- 行为变化与触发条件。
- 运行过的检查及结果。
- 验证范围，分别标注离线合成、真实浏览器、真实账号与真实设备，它们不能互相替代。没有权限或设备的能力记为未验证。

**不要提交** Cookie、storage state、真实 API Key、浏览器资料目录、SQLite 数据库、原始请求/响应或个人日志。`.gitignore` 只是辅助措施，提交前请检查 `git diff --cached`。

## 许可

提交贡献即表示该贡献按本项目的 MPL 2.0 条款提供，并且你有权作出该贡献。第三方文件保留原许可，不能仅替换文件头。完整许可见 [LICENSE](LICENSE)。
