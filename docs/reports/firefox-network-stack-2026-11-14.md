# Firefox 网络指纹实现（P4 第一步）

日期：2026-11-14。[Firefox 后端迁移计划](../firefox-backends.md) P4 的第一步：维护者选定 curl-impersonate 后，SpiderMonkey 后端的网络层改用它，TLS 与 HTTP/2 参数按实测的 Camoufox 158 设置。本文记录实现方式、离线测试、与 Camoufox 并排的实盘抓包，以及剩下的差异。调研与方案比较见 [Firefox 网络指纹调研](firefox-network-fingerprint-2026-11-12.md)。

## 结论

- **握手和连接层与 Firefox 一致**：实盘抓包中，SpiderMonkey 后端的 JA4、HTTP/2 SETTINGS、连接级 WINDOW_UPDATE、HEADERS 优先级（页面文档 42，其余 22）、伪首部顺序和 Cookie 分头，都与同一次运行里的 Camoufox 158 相同。
- **Google 正常接受**：同一测试账号先用 Camoufox、再用 SpiderMonkey 各生成 1 次，都返回 200。
- **抓包后修正了以下几处**（离线完成，有测试覆盖）：
  - 恢复 TLS 会话时，BoringSSL 发出的 ClientHello 和 Firefox 的不同，现在不再恢复会话；
  - RPC 请求头的顺序、`Priority` 和 `Content-Length` 的位置改成和网页自己的 XHR 一致；
  - 图片请求的权重改为 12。
- **修正后没有再发实盘请求**：这些改动不改变请求的先后顺序，按计划不追加实盘请求。线上的确认留到下一次抓包。
- **剩下的差异**：
  - 流级 WINDOW_UPDATE 和 ECH GREASE 长度，要自己编译 curl-impersonate 才能解决；
  - 页面行为方面的差异，例如 CORS 预检和页面自己的其他 RPC，见“剩下的差异”。

## 实现

- **构建**：
  - 构建选项 `AIS2API_CURL_IMPERSONATE`，开启 SpiderMonkey 时默认开启，也可以单独开启。
  - CMake 下载 curl-impersonate v2.3.2 的 Windows x64 包，并校验 SHA-256（`scripts/curl-impersonate.cmake`）。包里有 curl 8.23.0、BoringSSL 和 nghttp2 1.63.0。
  - 链接的是包里的 DLL，原因是静态库里的 BoringSSL 没有加符号前缀，会和程序静态链接的 OpenSSL 冲突。DLL 只导出 `curl_*` 和 `bssl_webcrypto_*`，只依赖系统 DLL。
  - 开启后整个程序只用这一份 libcurl，证书用系统证书库验证。
- **参数**：`src/firefox_http.cpp` 逐项设置以下参数，不使用 curl-impersonate 自带的 Firefox 预设（预设多了 FFDHE 组）：
  - 加密套件、密钥交换组、key share 数量、签名算法、delegated credentials；
  - 证书压缩、record_size_limit、扩展顺序、ECH GREASE；
  - HTTP/2 SETTINGS、连接级 WINDOW_UPDATE、伪首部顺序。
- **每个请求**：
  - HEADERS 帧的权重由请求给出：页面文档 42，图片 12，其余 22；
  - https 请求每个 Cookie 单独一个头；
  - 值为空的 `Content-Length` 头表示请求体长度，由会话填入，这样可以控制它的位置。
- **会话恢复**：关闭（`CURLOPT_SSL_SESSIONID_CACHE`），原因见“TLS 与 HTTP/2”。
- **诊断**：SpiderMonkey 网关的诊断增加 `network` 和 `networkStack` 两个字段。实盘时它们的值是 `curl-impersonate` 和 `libcurl/8.23.0-IMPERSONATE BoringSSL nghttp2/1.63.0`，安全报告里也带上这两个字段。

## 离线测试

`ais2api_tests "[firefox-http][fingerprint]"`：期望值在测试里单独写出，不引用实现中的参数表，这样参数写错时测试能发现。

| 测试 | 内容 |
|---|---|
| ClientHello | 本地 TCP 端口读出 ClientHello，逐项比较加密套件、扩展顺序、密钥交换组、key share、签名算法、ALPN 等，以及 JA4 |
| HTTP/2 连接 | 本地 TLS + HTTP/2 服务端，检查 SETTINGS 和连接级 WINDOW_UPDATE |
| 优先级与请求头 | 用 nghttp2 解 HPACK，检查权重、不独占、依赖 0、伪首部顺序、Cookie 分头和 `Content-Length` 的位置 |
| 新连接的握手 | 第一条连接完成 TLS 1.3 握手、拿到会话票据后由服务端关闭，第二条连接的 ClientHello 仍须与完整握手相同 |
| 流级 WINDOW_UPDATE | 标为 `[!mayfail]`，预期失败，见“剩下的差异” |

结果：

- SpiderMonkey 构建完整测试共 510 个用例，509 个通过，另 1 个是上表里的预期失败；
- 普通构建 489 个用例全部通过；
- CI 新增作业 `windows-curl-impersonate`，只编测试并运行 `[firefox-http]` 和 `[setup][download]`，还没有在 CI 上跑过。

## 实盘对照

### 方法

- 沿用调研时的方法：一个记录用的 CONNECT 代理加 `SSLKEYLOGFILE`。Camoufox（NSS）和 curl-impersonate（BoringSSL）都会导出密钥。
- 解密后的 HTTP/2 字节交给 Node.js 的 HTTP/2 服务端解码；请求头只保留名称，值只保留白名单里的几项。
- 原始抓包和密钥文件分析完即删除。
- 抓包工具（`tools/netfp/`）修了一个问题：客户端回给真实服务端的 PING 确认，在回放时被 Node 当成连接错误，所以每条连接只解出前几个请求，之前漏掉了 GenerateContent。现在回放时去掉 PING 帧。
- 样本：一个测试账号，`gemini-3.1-flash-lite`，非流式，同一个服务进程里 Camoufox 和 SpiderMonkey 各生成 1 次（06:38–06:39 UTC）。导入 Cookie 时 Camoufox 打开的页面也经过同一个代理，一并分析。

| 后端 | 结果 | 耗时（含加载页面） |
|---|---|---|
| Camoufox 158 | 200 | 11.3 秒 |
| SpiderMonkey（curl-impersonate） | 200 | 5.8 秒；WAA 程序启动 1.67 秒 |

两次之后，ais2api 进程的工作集为 84 MiB，私有内存 66 MiB，都不含 Camoufox 的进程。

### TLS 与 HTTP/2

| 项目 | Camoufox 158 | SpiderMonkey | 处理 |
|---|---|---|---|
| JA4（完整握手） | `t13d1517h2_8daaf6152771_3cbfd9057e0d` | 相同 | — |
| 会话恢复时的扩展 | 去掉 session_ticket，加 pre_shared_key，仍是 17 个（以前的抓包：`…_e6dcd7ae0a9e`） | 保留 session_ticket，共 18 个（`t13d1518h2_…_5dff20295fdc`），Firefox 不会发出这样的握手 | 关闭会话恢复，每条连接都用完整握手 |
| ECH GREASE 扩展长度 | 完整握手 282 字节，恢复时 570 字节 | 本次 186 字节；离线多次运行在 186、218、250、282 里随机 | 记录，预编译库改不了 |
| SETTINGS、连接级 WINDOW_UPDATE | `1:65536;2:0;4:131072;5:16384`、12517377 | 相同 | — |
| HEADERS 权重 | 文档 42，XHR、iframe、OPTIONS 22，`u=5, i` 的图片 12 | 文档 42，其余 22 | 图片改为 12 |
| 流级 WINDOW_UPDATE | 每个 HEADERS 之后一个，12451840 | 没有 | 记录，预编译库改不了 |
| 伪首部顺序、Cookie 分头 | `mpas`；每个 Cookie 一个头 | 相同，Cookie 个数也相同 | — |

### 请求头

Camoufox 一侧的请求分两种来源，比较时要分开：

- **网页自己的 grpc-web XHR**：不带 `Priority`，请求头顺序固定。SpiderMonkey 模仿的是这一种。
- **Camoufox 后端自己补发的 fetch**：`browser/bridge.js` 在生成前补发 GetAiStudioBenefitTier，并发送 GenerateContent。它们把 `authorization` 和 `x-goog-authuser` 挪到了后面，Firefox 还会给 fetch 加上 `priority: u=4`。
- 所以网页自己发的 GenerateContent 这次没有抓到。它的顺序按网页自己发的 CountTokens 推断：两者都带会员等级头，而且放在最前面。

网页 XHR 的请求头顺序：

- **MakerSuite RPC**：`user-agent`、`accept`、`accept-language`、`accept-encoding`、`referer`、[`x-aistudio-g1-tier`]、`x-aistudio-visit-id`、[`x-goog-ext-519733851-bin`]、`x-goog-api-key`、`x-goog-authuser`、`authorization`、`content-type`、`x-user-agent`、`content-length`、`origin`、`cookie`…、`sec-fetch-dest`、`sec-fetch-mode`、`sec-fetch-site`、`te`。
- **Waa/Create**：`referer`、`x-goog-api-key`、`authorization`、`x-goog-authuser`、`content-type`、`x-user-agent`、`content-length`、`origin`、`cookie`…，之后与上面相同。

修正前，SpiderMonkey 和网页 XHR 的不同之处：

- 头的顺序不同：`content-type` 在最前面，`authorization` 和 `x-aistudio-visit-id` 排在后面；
- 每个 RPC 都带 `priority: u=4`，GenerateContent 带 `u=0`；
- `content-length` 由 curl 加在最后；
- CountTokens 没有会员等级头。

现在都改成和网页一致，由 `[spidermonkey]` 中的请求头测试覆盖。

## 剩下的差异

**网络库（要自己编译 curl-impersonate）**：

- 流级 WINDOW_UPDATE：预编译库把流窗口上限改成了 1 MiB，也没有对应选项；
- 会话恢复：要打补丁，让 BoringSSL 恢复会话时不带 session_ticket，之后才能重新开启恢复；
- ECH GREASE 的长度：要按 NSS 的方式计算；
- HTTP/3：不经代理时 Firefox 会改用 HTTP/3，这个后端始终用 HTTP/2。

**页面行为（只记录，本步不实现）**：

- Firefox 第一次发某个跨站 RPC 前会先发 CORS 预检 `OPTIONS`（`priority: u=4`，权重 22），SpiderMonkey 不发。
- 网页还会发出很多 SpiderMonkey 不发的请求：
  - RPC：ListRecentApplets、ListPrompts、ListPromos、GetGdpProfileAndBenefits、GetAppletGalleryConfig、ListBillingAccounts、LogGdpInteractionEvents、GenerateTitle、GenerateAccessToken、GetUserPreferences、ListIncidentsHistory；
  - 一次 CSP 报告 POST。
- `x-goog-ext-519733851-bin`：
  - 网页的第一个 GetAiStudioBenefitTier 和 GetLoggingContext 同时发出，所以不带这个头；
  - SpiderMonkey 等 GetLoggingContext 返回之后才发 GetAiStudioBenefitTier，所以带上了。
- ListModels：网页自己发的不带会员等级头，SpiderMonkey 带。没有改，因为去掉后返回的模型列表可能不同。
- 图片的 `Accept`：Firefox 158 多了 `image/jxl`。SpiderMonkey 目前声称 152，等它和 Camoufox 同版本时一起对齐。

## 后续建议

1. 经同意后，用 SpiderMonkey 后端带抓包生成 1 次，确认修正后的请求头在线上的样子和 Google 的反应。
2. 自己编译 curl-impersonate，解决上面“网络库”一项的所有问题；顺便加 `BORINGSSL_PREFIX` 静态链接，支持 Linux 和 Android。和 SpiderMonkey 静态库仓库一起做。
3. 对照实验（P4）之后，按结果决定是否模仿 CORS 预检，以及 ListModels 和第一个 GetAiStudioBenefitTier 的细节。

## 资料

- 抓包工具：[`tools/netfp/`](../../tools/netfp/README.md)。
- 网络参数：`src/firefox_http.cpp`。
- 离线测试：`tests/test_firefox_fingerprint.cpp`。
- 请求头：`src/spidermonkey_page.cpp`。
