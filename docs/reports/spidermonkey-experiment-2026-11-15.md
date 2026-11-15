# SpiderMonkey 主线收尾与对照实验报告（P4）

2026-11-15，在整合 `p4/integration` 后完成 P4 其余工程项，并使用统一测试账号与工具完成受控三格对照实验、`ais2api serve` 默认路由实测和诊断导出复核。

## 结论

- **三格 10 项操作全通过**：`tools/waa_experiment` 在 Cell 4（`spidermonkey` + `libcurl`）、Cell 1（`camoufox` + `page`）、Cell 2（`camoufox` + `libcurl`）共执行 10 次操作，全部返回 200、内容非空，零拒绝、零失败。
- **内存占用下降一个数量级**：稳态下 SpiderMonkey 进程工作集 **68.23 MB**（私有字节 **55.18 MB**，0 子进程）；同环境下 Camoufox 宿主加 5 个子进程共 **771.76 MB**（私有字节 **635.91 MB**），工作集约为 SpiderMonkey 的 11.3 倍。
- **默认路由已确定**：
  - **桌面与服务器**（Windows、Linux x64 / ARM64、Docker）：`auto` 默认路由到 **`camoufox`**，优先保证真实浏览器语义与稳定性；
  - **Termux 原生 NDK 构建**（开启 `AIS2API_SPIDERMONKEY`）：`auto` 默认路由到 **`spidermonkey`**，免除 `proot-distro` 容器与 1.3 GB 浏览器下载；
  - **6 小时自动切换兜底**：设为 `auto` 的账号在就地重建会话后仍遇到 `403`（`permission`）时，同一请求内自动切换到另一种已就绪的 Firefox 后端重试，并在该后端停留 6 小时（`21600` 秒）。
- **端到端与诊断检查通过**：`ais2api serve` 在 `auto` 默认路由下非流式生成返回 `200 OK`；`POST /api/admin/v1/diagnostics/export`（`6,959` 字节）零敏感字段泄漏；停机后零残留浏览器子进程。

## P4 后续工程实现（对照实验前集成）

在 P4 第一步（网络指纹）和第二步（升级到 SpiderMonkey 158）之后，对照实验前完成以下关键工程项：

1. **统一四平台静态预编译库（`mananekoha114/ais2api-spidermonkey`）**：
   - **静态 SpiderMonkey 158.0.1**（`spidermonkey-158.0.1-r1`）：统一产出 `windows-x64`、`linux-x64`、`linux-arm64`、`android-arm64` 四个目标的静态归档（Android ARM64 使用 NDK r27c），通过 [`scripts/spidermonkey.cmake`](../../scripts/spidermonkey.cmake) 接入；
   - **静态 curl-impersonate 2.3.2**（`curl-impersonate-2.3.2-r2`）：接入四个平台，BoringSSL 编译时加 `BORINGSSL_PREFIX=bssl_` 符号前缀，可直接与程序静态链接的 OpenSSL 3 共存于同一可执行文件，运行时零 `.dll`/`.so` 依赖；
   - **完整补齐 Firefox 158 网络指纹**：恢复 TLS 1.3 PSK 会话复用并省略与 `pre_shared_key` 并存的 `session_ticket` 扩展（首次握手 `JA4 = t13d1517h2_8daaf6152771_3cbfd9057e0d`，恢复握手 `JA4 = t13d1517h2_8daaf6152771_e6dcd7ae0a9e`），按 NSS 尺寸对齐外层 ECH GREASE（首次 `282` 字节、恢复 `570` 字节），并在每个 HTTP/2 `HEADERS` 后发送流级 `WINDOW_UPDATE 12451840`。
2. **SpiderMonkey 无浏览器身份合成**：
   - 此前从未用 Camoufox 启动过身份的账号，首次经过 SpiderMonkey 后端时可以直接读取 `fpgen_model`，按同一确定性算法与宿主 OS 生成存储 Firefox `158.0` 身份，不再需要先启动一次浏览器（`claimsHostGpu: false`、`canvasCovered: false`、`hostGpu: "not_checked"`）。
3. **Camoufox 隐藏页面桥接全局量**：
   - 将浏览器页面桥接从原先挂载在 `window.__ais2bridge` 的可枚举全局属性，改为通过 WebDriver BiDi `script.addPreloadScript` 传入预加载通道 `ais2api-bridge`（`ownership: "root"` 的 `RemoteReference` 句柄）；页面 `window`/`globalThis` 上不再暴露任何桥接全局属性。
4. **Termux 原生 NDK 路由与轻量初始化**：
   - 抽离 `setup_fpgen_model()`：开启 SpiderMonkey 的 Termux 原生环境下，`setup` 与 `doctor` 针对 `spidermonkey` / 默认 `auto` 仅检查与安装 `fpgen-model`，不触发 `sudo apt`、`proot-distro` 容器或 Camoufox 浏览器下载；
   - `default_firefox_backend(platform)` 统一控制平台默认路由，在显式指定 `camoufox` 时仍保留 proot + Camoufox 路线。
5. **6 小时（`21600` 秒）`auto` 自动切换后端**：
   - SQLite 新增 `account_backend_switches` 持久化临时切换目标与截止时间（`21600` 秒）；手动切换后端、重置账号或清除冷却时一并清理；
   - `Service::generate()` 在账号设置为 `auto`、就地恢复会话后仍然遇 `403`（`permission`）且本请求尚未切换过后端时，在另一侧 Firefox 后端已编译并配置就绪的前提下持久化切换 6 小时、记入 `backend_switch` 事件统计并在同一次请求内放宽一次切换重试。

## 受控对照实验与 `ais2api serve` 实测

实验在同一受控主机环境（Windows x64，`p4/integration` 静态构建，`libcurl/8.23.0-IMPERSONATE BoringSSL nghttp2/1.63.0` + SpiderMonkey `158.0.1`）下使用同一测试账号与廉价模型 `gemini-3.1-flash-lite` 执行，测试间隔 15 秒，执行后无残余进程，原始明细不入库。

### 三格对照（`tools/waa_experiment`）

命令维度：`--cells 412 --rounds 1 --interval 15 --modes same --ops probe,models,generate,rerun`；整轮耗时 `161.20` 秒，退出码 `0`，10/10 全部成功（0 拒绝、0 失败）：

| 实验格 | 操作 | 总耗时 | 细分指标 | 内存占用（工作集 / 私有，子进程） | 结果 |
|---|---|---|---|---|---|
| **Cell 4** (`spidermonkey` + `libcurl`) | `probe` | 2,207 ms | `identity_ready: true`（VM 未启动前） | 18.39 MB / 5.36 MB，0 子进程 | 通过 |
| **Cell 4** (`spidermonkey` + `libcurl`) | `models` | 367 ms | `models_count: 41` | — | 通过 |
| **Cell 4** (`spidermonkey` + `libcurl`) | `generate` | 3,707 ms | `proof_ms: 2,056`，`proof_bytes: 1,565`，`first_chunk_ms: 1,641`，`send_ms: 1,650`，`chunks: 2`，`text_chars: 2`，`vm_started: true` | **68.23 MB / 55.18 MB，0 子进程** | 通过 |
| **Cell 4** (`spidermonkey` + `libcurl`) | `rerun` | 3,139 ms | `proof_ms: 1,163`，`proof_bytes: 1,667`，`first_chunk_ms: 1,956`，`send_ms: 1,975`，`chunks: 2`，`text_chars: 2`，`vm_started: false` | **67.85 MB / 52.68 MB，0 子进程** | 通过 |
| **Cell 1** (`camoufox` + `page`) | `probe` | 7,940 ms | `identity_ready: true` | — | 通过 |
| **Cell 1** (`camoufox` + `page`) | `models` | 1,049 ms | `models_count: 41` | — | 通过 |
| **Cell 1** (`camoufox` + `page`) | `generate` | 2,154 ms | `proof_ms: 921`，`proof_bytes: 1,563`，`first_chunk_ms: 1,230`，`send_ms: 1,233`，`chunks: 2`，`text_chars: 2` | **771.76 MB / 635.91 MB**（宿主 70.38 MB + 5 子进程 701.38 MB） | 通过 |
| **Cell 1** (`camoufox` + `page`) | `rerun` | 1,369 ms | `proof_ms: 29`，`proof_bytes: 1,520`，`first_chunk_ms: 1,303`，`send_ms: 1,339`，`chunks: 2`，`text_chars: 2` | **775.62 MB / 641.65 MB**（宿主 70.41 MB + 5 子进程 705.21 MB） | 通过 |
| **Cell 2** (`camoufox` + `libcurl`) | `generate` | 2,196 ms | `ua_matched: true`，`proof_ms: 61`，`proof_bytes: 1,520`，`first_chunk_ms: 1,854`，`send_ms: 2,135`，`chunks: 2`，`text_chars: 2` | **769.35 MB / 612.25 MB**（宿主 70.82 MB + 5 子进程 698.53 MB） | 通过 |
| **Cell 2** (`camoufox` + `libcurl`) | `rerun` | 1,133 ms | `ua_matched: true`，`proof_ms: 107`，`proof_bytes: 1,520`，`first_chunk_ms: 1,005`，`send_ms: 1,025`，`chunks: 2`，`text_chars: 2` | — | 通过 |

对照观察：

- **功能与通过率**：三种组合（纯 SpiderMonkey、纯 Camoufox、Camoufox 算 proof 配原生 Firefox 指纹 libcurl 发送）在同一轮实验内全部通过；混合组合 Cell 2 的 `ua_matched` 为 `true`，说明两者身份 UA 与指纹口径对齐一致。
- **冷启动与单次耗时**：SpiderMonkey `probe`（2.21 秒）和 `models`（0.37 秒）明显快于 Camoufox（7.94 秒与 1.05 秒）；计算 proof 方面，Camoufox 在浏览器页内生成更快（`rerun` `proof_ms: 29 ms` 对 SpiderMonkey `1,163 ms`），后者受进程内解释执行 WAA 的耗时影响，总体单次生成仍在 3.1–3.7 秒内完成。
- **资源代价**：SpiderMonkey 省去了整个 Firefox 浏览器及 5 个内容/GPU 子进程（总工作集减少 ~703 MB、私有字节减少 ~580 MB），在无浏览器环境与 Termux 上具有决定性优势。

### `ais2api serve` 默认路由与诊断导出实测

使用同一静态发行构建启动完整的 `ais2api serve` 服务，测试账号后端配置为 `auto`（桌面默认解析为 `camoufox`）：

| 验证项目 | 实测结果 |
|---|---|
| 启动前服务内存 | 23.81 MB 工作集 / 8.82 MB 私有字节，0 浏览器子进程 |
| 非流式 `/v1/chat/completions`（`gemini-3.1-flash-lite`） | **200 OK**，耗时 `12.571` 秒（含首次拉起 Camoufox 与身份就绪）；`finish_reason: "stop"`，`prompt_tokens: 8`、`completion_tokens: 1`、`total_tokens: 9` |
| 请求后稳态内存 | **757.69 MB** 工作集 / **639.67 MB** 私有字节（宿主 25.54 MB / 11.03 MB + 5 个 Camoufox 子进程 732.15 MB / 628.64 MB） |
| 诊断导出（`POST /api/admin/v1/diagnostics/export`） | 返回 **6,959 字节**结构化 JSON；核查确认零 Cookie 值、零邮箱、零真实账号 ID、零请求文本、零本机路径、零代理地址泄漏 |
| 停机清理（`POST /api/admin/v1/service/stop`） | 正常关闭服务进程，状态库保持 `backend: "auto"` 且 `expired: 0`，系统中 0 个残留浏览器子进程 |

## 默认路由决策依据

综合三格对照与服务实测：

- **桌面与服务器继续以 `camoufox` 作为 `auto` 默认后端**：本轮仅完成单账号 10 次短序列实验，桌面资源裕量充足时由真实浏览器承担默认流量风险最低；若现场遇到页面端 WAA 403 且在位重建无法恢复，`auto` 账号会在当次请求自动故障切换到 `spidermonkey` 并维持 6 小时，兼顾稳妥与韧性。
- **Termux 原生 NDK 构建（开启 `AIS2API_SPIDERMONKEY`）以 `spidermonkey` 作为 `auto` 默认后端**：原生环境无法运行桌面浏览器，使用 SpiderMonkey 可以省去 Ubuntu proot 容器维护与 GB 级浏览器体积，68 MB 工作集完全契合移动端常驻。

## 资料

- 方案与阶段规划：[Firefox 后端迁移计划](../firefox-backends.md)
- 构建接入：[`scripts/spidermonkey.cmake`](../../scripts/spidermonkey.cmake)、[`scripts/curl-impersonate.cmake`](../../scripts/curl-impersonate.cmake)
- 路由与故障切换：`src/backend_router.cpp`、`src/store.cpp`、`src/server.cpp`、`src/setup.cpp`
- 对照工具：`tools/waa_experiment.cpp`
- 前序报告：[Firefox 网络指纹实现（P4 第一步）](firefox-network-stack-2026-11-14.md)、[SpiderMonkey 升级到 158.0.1（P4 第二项）](spidermonkey-158-2026-11-14.md)
