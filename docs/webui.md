# Web 管理界面

WebUI 是可选的 Vue 3 管理控制台。启用后，前端资源在构建时生成并嵌入 ais2api 可执行文件，运行时不需要 Node.js，也不需要在程序旁放置 `webui/` 或其他资源目录。未启用 WebUI 的构建仍然可以通过命令行和 [管理 API](management-api.md) 管理服务。

GitHub CI 的 Windows、Ubuntu、Debian 与 Termux 运行包均启用 WebUI。

## 登录

1. 按 [快速开始](getting-started.md) 初始化并启动服务。
2. 在浏览器打开服务地址，例如 `http://127.0.0.1:18451/`。登录页的“后端地址”留空表示当前服务。
3. 新创建的配置默认管理密码为 `admin`。首次登录后控制台会强制进入“设置新的管理密码”页面，修改前服务拒绝其他所有管理请求。

注意事项：

- 默认密码只接受来自本机或局域网（回环、私有地址、链路本地、IPv6 ULA）的连接。如果服务只能从公网访问，请先在 `config.json` 中设置足够强的 `admin_key`，再重启服务或执行 `ais2api config reload`。
- 管理密码与生成接口的 `api_key` 相互独立。
- 之后可以在“设置 → 管理密码”中再次修改。新密码需为 8–4096 个可见 ASCII 字符且不能是 `admin`，立即生效，即使存在待重启的配置也是如此。
- 管理密钥默认只保存在当前标签页的 sessionStorage；勾选“在此设备上记住”才会写入 localStorage。
- 如需撤销访问，清空 `admin_key` 并重新加载配置或重启服务（这会关闭 HTTP 管理）。

## 页面

| 路径 | 页面 | 主要功能 |
|---|---|---|
| `/` | 概览 | 运行状态、账号调度与健康、用量概况、快捷操作 |
| `/accounts` | 账号 | 导入、改名、启停、设为首选、探测、清除冷却、查看与编辑指纹 |
| `/models` | 模型 | 模型发现缓存与刷新 |
| `/keys` | API 密钥 | 设置/轮换/清空配置密钥，创建与撤销托管密钥，查看接入地址 |
| `/stats` | 用量统计 | 按账号与模型的调用计数和 usage |
| `/logs` | 日志 | 全部日志来源的历史与实时流 |
| `/config` | 服务配置 | 生成上游（含实验性 Build App 上游）、网络、浏览器与存储、API 兼容性、限额等 |
| `/diagnostics` | 诊断 | 浏览器适配阶段、当前指纹、模型发现、Cookie 写回状态，导出脱敏快照 |
| `/settings` | 设置 | 外观与连接偏好（仅保存在当前浏览器）、修改管理密码 |

控制台通过同源的 `/api/admin/v1` 调用管理 API，无需配置跨域来源。

修改需要重启的配置（如端口）后，页面会显示“已保存，重启后生效”。此时轮换或清空配置密钥也只会写入待重启配置，页面会明确说明旧密钥在重启前仍然有效。

## 构建

WebUI 构建需要 Node.js `^20.19.0` 或 `>=22.12.0`（推荐 22.12 或更新），以及对应平台的 C++ 构建环境（见 [构建与打包](platforms.md)）。

```powershell
# Windows（开发者 PowerShell）
./scripts/build.ps1 -WebUI
```

```sh
# Linux
sh scripts/build.sh linux --webui
# Termux
pkg install nodejs npm
sh scripts/build.sh termux --webui
```

直接调用 CMake 时设置 `-DAIS2API_BUILD_WEBUI=ON`。构建会执行前端依赖安装（`npm ci`）、类型检查和生产构建，并把产物编码为 C++ 内嵌资源。默认值为 `OFF`，因此纯后端构建不需要 Node.js。构建脚本的其他选项（如 `--jobs`、`--skip-tests`、`--package`）照常可用。

## 前端开发

```sh
cd webui
npm ci
npm run dev        # http://localhost:5173，/api/admin/ 代理到 http://127.0.0.1:18451
npm run typecheck
npm test           # Vitest + happy-dom，模拟管理 API，不连接真实服务
npm run build
```

- 用环境变量 `AIS2API_URL` 指定开发服务器代理的后端地址。
- `npm run preview` 只预览构建产物。独立静态部署时，在登录页填写后端地址，并在后端 `admin_origins` 中加入页面的完整来源。
- 目录结构：`src/api` 为管理 API 与日志 SSE 客户端，`src/views` 为各页面，`src/components` 为 Fluent 风格组件，`src/styles` 为主题和控件样式；`embed.mjs` 在 CMake 构建时把 `dist/` 编码为 C++ 内嵌资源。

WebUI 源码采用 MPL-2.0；Vue、Vue Router 与 Fluent System Icons 的 MIT 许可见 [THIRD_PARTY_LICENSES.txt](../webui/public/THIRD_PARTY_LICENSES.txt)。
