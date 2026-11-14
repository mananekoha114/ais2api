# Firefox 网络指纹调研（P3）

日期：2026-11-12。[Firefox 后端迁移计划](../firefox-backends.md) P3 的调研项：SpiderMonkey 后端不启动浏览器，受保护请求由 ais2api 直接发出，这些请求的 TLS 握手和 HTTP/2 帧要像 Firefox。本文记录 Camoufox 的实测基准、原型网络层与它的差距，以及四种实现方案的比较。方案由维护者选定。

## 结论

- **原型的网络层与 Firefox 差别很大**：Windows 上 libcurl 用 Schannel，ClientHello 的加密套件、扩展和顺序都与 Firefox 不同，JA4 也不同；请求头声称 Firefox，握手却是 Windows 的。
- **curl-impersonate 的 Firefox 配置与实测的 Camoufox 158 大体一致**：加密套件、扩展顺序、签名算法、HTTP/2 SETTINGS、连接级 WINDOW_UPDATE 和伪首部顺序都相同。差异有三处：密钥交换组多了 FFDHE，HEADERS 的优先级权重固定，流级 WINDOW_UPDATE 的行为待核实。
- **HTTP/3**：Google 的相关域名都声明支持 `h3`。经 HTTP 代理时 Firefox 只用 HTTP/2，本次实测也是如此；不经代理时 Firefox 很可能改用 HTTP/3，这种情况没有实测。
- **建议**：P4 以 curl-impersonate（lexiforest 分支）为基础。Firefox 配置由我们按每次实测的值自己设置，再用同样的抓包方法验证。理由见“方案比较”。

## 方法

- 在本机 HTTP 代理前串一个记录用的 CONNECT 代理，原样记下每条隧道双向的字节，不解密、不改动。ClientHello 是明文，直接解析。
- Camoufox 支持 `SSLKEYLOGFILE`。用导出的 TLS 1.3 密钥解密客户端方向的记录，再把得到的 HTTP/2 字节交给 Node.js 自带的 HTTP/2 服务端解码，取得 SETTINGS、WINDOW_UPDATE、优先级、伪首部和请求头顺序。Schannel 不导出密钥，所以原型一侧只看 ClientHello。
- 请求头只记录名称，以及 `accept`、`sec-fetch-*`、`priority` 等固定白名单里的取值；Cookie 只记名称。原始抓包和密钥文件分析完即删除。
- 环境：Windows 11 x64，Camoufox `v158.0.1-beta.41`；SpiderMonkey 原型构建，libcurl 用 Schannel 并开启 HTTP/2。抓包与 [SpiderMonkey 原型报告](spidermonkey-prototype-2026-11-12.md) 中的真实账号请求是同一次运行，没有额外发生成请求。

## 基准：Camoufox 158（Windows）

### TLS ClientHello

| 项目 | 实测 |
|---|---|
| JA4 | `t13d1517h2_8daaf6152771_3cbfd9057e0d`；会话恢复时多一个 `pre_shared_key`，第三段变为 `e6dcd7ae0a9e` |
| JA3 | `9d42e90b0225e779f03141ddcd699df2` |
| 加密套件（15 个） | `1301 1303 1302 c02b c02f cca9 cca8 c02c c030 c013 c014 009c 009d 002f 0035` |
| 扩展顺序（17 个，各连接相同） | server_name、extended_master_secret、renegotiation_info、supported_groups、ec_point_formats、session_ticket、alpn、status_request、delegated_credentials、signed_certificate_timestamp、key_share、supported_versions、signature_algorithms、psk_key_exchange_modes、record_size_limit、compress_certificate、encrypted_client_hello |
| 密钥交换组 | X25519MLKEM768、x25519、secp256r1、secp384r1、secp521r1（没有 FFDHE） |
| key share | X25519MLKEM768（1216 字节）、x25519、secp256r1 |
| 签名算法 | `0403 0503 0603 0804 0805 0806 0401 0501 0601 0203 0201` |
| 其他 | ALPN `h2, http/1.1`；record_size_limit 16385；证书压缩 zlib、brotli、zstd；delegated_credentials `0403 0503 0603 0203`；ECH 外层 282 字节（没有 ECH 配置时的 GREASE）；没有 GREASE 值，扩展顺序不打乱 |

所有 Google 域名的连接都协商 TLS 1.3，绝大多数为 `TLS_AES_128_GCM_SHA256`。

### HTTP/2

| 项目 | 实测 |
|---|---|
| SETTINGS | `HEADER_TABLE_SIZE=65536`、`ENABLE_PUSH=0`、`INITIAL_WINDOW_SIZE=131072`、`MAX_FRAME_SIZE=16384` |
| 连接级 WINDOW_UPDATE | 12517377 |
| HEADERS | 带优先级字段（不独占、依赖 0），权重随请求类型变化：页面文档 42，脚本、XHR 等 22，另见 32、12、2 |
| 流级 WINDOW_UPDATE | 每个 HEADERS 之后紧跟一个 12451840 |
| 没有出现的帧 | 单独的 PRIORITY 帧、PRIORITY_UPDATE |
| 伪首部顺序 | `:method :path :authority :scheme` |
| Cookie | 每个 Cookie 单独一个 `cookie` 头 |

请求头顺序（名称，部分带值）：

- **页面文档**：user-agent、accept（`text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8`）、accept-language、accept-encoding（`gzip, deflate, br, zstd`）、cookie…、upgrade-insecure-requests、sec-fetch-dest（document）、sec-fetch-mode（navigate）、sec-fetch-site（none）、sec-fetch-user、priority（`u=0, i`）、te（trailers）。
- **页面的 RPC（POST）**：user-agent、accept（`*/*`）、accept-language、accept-encoding、referer、x-aistudio-visit-id、x-goog-api-key、x-goog-authuser、authorization、content-type（`application/json+protobuf`）、x-user-agent、content-length、origin、cookie…、sec-fetch-dest、sec-fetch-mode、sec-fetch-site（same-site）、te。
- **CORS 预检（OPTIONS）**：…accept-language、accept-encoding、access-control-request-method、access-control-request-headers、referer、origin、sec-fetch-*、priority（`u=4`）、te。

### 页面实际发出的请求

一次页面加载里，Camoufox 先后连接 aistudio、fonts.googleapis/gstatic、www.gstatic、apis.google、googletagmanager、console.cloud.google、alkalimakersuite-pa、waa-pa、people-pa、google-analytics、lh3.googleusercontent、www.google、play.google 和 accounts.google（会话 Cookie 刷新）。MakerSuite 一侧除 `GetLoggingContext`、`GetAiStudioBenefitTier`、`ListModels` 外，还有 `GetUserPreferences`、`GenerateAccessToken`、`ListPromos`、`ListIncidentsHistory`、`GetGdpProfileAndBenefits`。原型只做最小引导集合（页面、`GetLoggingContext`、`GetAiStudioBenefitTier`、`Waa/Create`、解释器），差距记在原型报告里。

### HTTP/3

`aistudio.google.com`、`alkalimakersuite-pa.clients6.google.com`、`waa-pa.clients6.google.com` 和 `www.gstatic.com` 的响应都带 `Alt-Svc: h3=":443"; ma=2592000,h3-29=":443"; ma=2592000`。经 HTTP 代理时，所有连接都是 CONNECT 隧道里的 HTTP/2。不经代理时 Firefox 默认会按 Alt-Svc 改用 HTTP/3；这需要另行抓 UDP，本次没有测。所以不用代理部署时，网络层还要模仿 QUIC 和 HTTP/3。

## 原型：libcurl + Schannel

| 项目 | 实测 |
|---|---|
| JA4 | `t13d2013h2_2b729b4bf6f3_e24568c0d440` |
| 加密套件（20 个） | `1302 1301 c02c c02b c030 c02f c024 c023 c028 c027 c00a c009 c014 c013 009d 009c 003d 003c 0035 002f`：没有 ChaCha20，多了 CBC-SHA256/384 |
| 扩展（13 个） | server_name、status_request、supported_versions、signature_algorithms、session_ticket、supported_groups、ec_point_formats、alpn、key_share、post_handshake_auth、extended_master_secret、renegotiation_info、psk_key_exchange_modes |
| 缺少的 Firefox 扩展 | delegated_credentials、signed_certificate_timestamp、record_size_limit、compress_certificate、encrypted_client_hello |
| 密钥交换组 | X25519MLKEM768、x25519、secp256r1、secp384r1 |
| 签名算法 | `0804 0805 0806 0401 0501 0201 0403 0503 0203 0202 0601 0603`（顺序与 Firefox 不同，多了 `0202`） |
| HTTP/2 | 没有实测。curl 的 HTTP/2 默认设置与 Firefox 不同，例如会发送 `MAX_CONCURRENT_STREAMS`；请求头名称和顺序由原型按 Firefox 显式给出，但 Cookie 合成一个头，HEADERS 也没有 Firefox 的权重 |

此外，原型的 User-Agent 声称 Firefox 152，而同一账号在 Camoufox 下是 158。

## 方案比较

| | curl-impersonate（lexiforest） | libcurl + 自维护 BoringSSL 补丁 | NSS + nghttp2 | 参考项目的 Go 网络栈 |
|---|---|---|---|---|
| 是什么 | 打过补丁的 curl、BoringSSL、nghttp2、ngtcp2/nghttp3，提供 libcurl 接口 | 自己维护和 curl-impersonate 同类的补丁 | Firefox 自己的 TLS 库，HTTP/2 用 nghttp2，自写 HTTP 客户端 | `Mag1cFall/utls v1.8.2-firefox.3`、`fhttp v0.6.8-firefox.2`、`uquic v0.0.6-firefox.6`，经 cgo 编成库 |
| 与 Firefox 的一致程度 | 静态特征基本一致（上文实测对照）；BoringSSL 与 NSS 在细节上可能不同，待抓包核实 | 同左，取决于自己的投入 | TLS 可以做到逐字节一致（同一实现，按 Firefox 的选项配置）；HTTP/2 的行为要照 Firefox 的 Necko 手工复现；HTTP/3 要再引入 neqo（Rust） | 静态特征按 Firefox 152 调好，含 HTTP/3 |
| 跟进新版本 | 有 Firefox 目标（最新 `firefox155`，Firefox 正式版已是 159）；各项参数也能通过选项自己设置，不必等它更新 | 全靠自己 | 跟着 NSS 版本走，TLS 部分省力，HTTP 部分全靠自己 | 取决于分支作者，或自己改 Go 代码 |
| 四个平台 | 预编译库覆盖 Windows x64、Linux x64/ARM64（glibc 与 musl）、Android ARM64 | 自己搭四平台构建 | NSS/NSPR 能编（Firefox 自己在这些平台上用），构建复杂 | Go 都支持；Windows 上 cgo 需要 MinGW 工具链 |
| 静态链接 | Windows 包里有 MSVC 格式的静态库，`/MT`（libcmt）与本项目一致；Linux/Android 有 `.a` | 可以 | 可以，较繁琐 | c-archive 与 MSVC 链接麻烦，实际多用 DLL；进程里多一个 Go 运行时 |
| HTTP/3 | 支持，含 QUIC 传输参数的设置 | 要自己接 ngtcp2 | 要引入 neqo | 支持 |
| 许可证 | MIT（BoringSSL 另附其许可） | 同左 | MPL 2.0 与 MIT | BSD-3-Clause 与 MIT |
| 接入成本 | 低：`HttpSession` 本来就基于 libcurl | 高 | 很高：替换整个 HTTP 层 | 中高：跨语言边界、进程内两套运行时 |

### curl-impersonate 与实测的差异

curl-impersonate 的 `firefox155` 配置（`patches/curl.patch`）与上文的 Camoufox 158 对照：

- **一致**：15 个加密套件及顺序、扩展顺序 `0-23-65281-10-11-35-16-5-34-18-51-43-13-45-28-27-65037`、签名算法、delegated credentials、证书压缩、record_size_limit 16385、3 个 key share、ECH、SETTINGS `1:65536;2:0;4:131072;5:16384`、WINDOW_UPDATE 12517377、伪首部 `mpas`、Cookie 分头。
- **不同**：
  - 密钥交换组多了 `ffdhe2048`、`ffdhe3072`，实测的 Camoufox 没有；
  - HEADERS 的权重固定为 22，Firefox 按请求类型变化。libcurl 的 `CURLOPT_STREAM_WEIGHT` 可以逐个请求设置；
  - Firefox 在每个 HEADERS 之后发流级 WINDOW_UPDATE，curl-impersonate 是否一样还没有核实；
  - ECH GREASE 的长度、记录切分等由 BoringSSL 实现，可能与 NSS 不同。公开文章指出，HTTP/2 的动态节奏（何时发 WINDOW_UPDATE、连接复用）比静态字符串更难模仿。

## 建议

P4 采用 curl-impersonate（lexiforest 分支）：

1. 用它的库替换 vcpkg 的 libcurl，只在 SpiderMonkey 后端的 `HttpSession` 里用。不直接用它内置的 Firefox 目标，而是按我们实测的 Firefox 值逐项设置：去掉 FFDHE，按请求类型设权重，User-Agent 与 Camoufox 的版本一致。
2. BoringSSL 同样支持 `SSLKEYLOGFILE`，所以可以用本次的抓包方法把我们自己的流量与 Camoufox 并排比较，HTTP/2 部分也能比。这一步写进版本升级的检查清单。
3. 不经代理的部署要开 HTTP/3，参数同样按实测设置。
4. 如果之后发现 BoringSSL 与 NSS 的细节差异确实导致拒绝，再考虑 NSS + nghttp2。Go 网络栈不建议：进程里多一个运行时，依赖第三方分支，Windows 构建要换工具链。

## 资料

- [lexiforest/curl-impersonate](https://github.com/lexiforest/curl-impersonate)（v2.3.2，2026-10-30）与[文档](https://curl-impersonate.readthedocs.io/en/latest/)
- [lwthiker/curl-impersonate](https://github.com/lwthiker/curl-impersonate)（原项目，Firefox 版基于 NSS，最后的目标是 ff117）
- [Detecting curl-impersonate and uTLS: the second-order tells](https://blog.crawlex.net/blog/detecting-curl-impersonate-utls/)（2026-03-05）
- [The ClientHello of every major browser, version by version](https://blog.crawlex.net/blog/browser-clienthello-catalog/)
- [refraction-networking/utls](https://pkg.go.dev/github.com/refraction-networking/utls)
- [AIStudio2API](https://github.com/Mag1cFall/AIStudio2API/tree/e38957a084c8928977e74a1d4faba80e5e852950)（`go.mod` 中的网络栈版本、`docs/protocol.md`）
- [draft-ietf-tls-esni](https://datatracker.ietf.org/doc/draft-ietf-tls-esni/25/)（没有 ECH 配置时发送 GREASE）
