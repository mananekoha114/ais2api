# ais2api 文档

## 使用

| 文档 | 内容 |
|---|---|
| [快速开始](getting-started.md) | 获取程序、一键启动、设置密钥、导入账号、发出第一个请求 |
| [Docker 部署](docker.md) | 构建镜像、数据目录、环境变量、浏览器与升级 |
| [客户端接入](clients.md) | 接口与鉴权，Cherry Studio / SillyTavern / RikkaHub / OpenAI SDK 的填写方法，搜索模型 |
| [配置参考](configuration.md) | 全部配置项、环境变量与命令行参数，热更新规则，代理与低内存模式 |
| [命令行](cli.md) | 所有命令、输出格式、服务生命周期 |
| [Web 管理界面](webui.md) | 登录、各页面功能、构建与前端开发 |
| [Cookie 导入](userscript.md) | Tampermonkey 脚本的安装、读取方式与原版脚本兼容 |
| [故障排查](troubleshooting.md) | 按错误原因整理的常见问题 |

## 接口参考

| 文档 | 内容 |
|---|---|
| [协议合同](protocol.md) | 三种数据 API 的支持范围、schema 转换、工具与搜索、扩展字段、流式解析 |
| [Responses 接口](responses.md) | OpenAI Responses 的无状态子集与示例 |
| [真 role 与思考签名](true-role.md) | 消息边界保真与签名往返 |
| [管理 API](management-api.md) | 管理接口的鉴权、路由、配置保存规则与日志流 |
| [浏览器网关](browser.md) | 浏览器驱动原理、指纹、Cookie 写回、重试冷却与账号调度 |

机器可读的定义：[config.schema.json](../schemas/config.schema.json)、[management.openapi.json](../schemas/management.openapi.json)、[protocol-extensions.schema.json](../schemas/protocol-extensions.schema.json)。

## 开发与维护

| 文档 | 内容 |
|---|---|
| [构建与打包](platforms.md) | Windows / Linux / Termux 构建、运行包结构、Browser-Local 包、CI |
| [验证记录](validation.md) | 已执行的验证、能证明的范围与未满足的发布门槛 |
| [路线图](roadmap.md) | 阶段状态、待决定的方向与已知缺口 |
| [Firefox 后端迁移计划](firefox-backends.md) | 从 CloakBrowser 迁移到 Camoufox 与 SpiderMonkey 的决策、阶段与待实测事项 |
| [性能工具](performance.md) | 离线回放工具的用法与报告解读 |
| [安全回归审查](security-review.md) | 下载、连接上限与媒体预算等安全修复的回归 |
| [发布流程](github-publishing.md) | 提交检查、CI 与二进制发布清单 |
| [专项报告](reports/README.md) | 各次修复与验证的历史记录 |
| [贡献指南](../CONTRIBUTING.md) | 开发约定、验证要求与许可 |
