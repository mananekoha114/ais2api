# 网络指纹抓包对照

用于检查 SpiderMonkey 后端发出的 TLS 握手和 HTTP/2 帧是否与 Firefox（以 Camoufox 为准）一致。方法与结果见 [Firefox 网络指纹调研](../../docs/reports/firefox-network-fingerprint-2026-11-12.md)。离线部分由 `ais2api_tests "[firefox-http][fingerprint]"` 覆盖，本工具用于和真实的 Firefox 并排比较。

## 原理

- `capture_proxy.py` 是一个本地 HTTP CONNECT 代理，串在原有代理前面，原样记录每条隧道两个方向的字节，不解密、不改动。ClientHello 是明文，可以直接解析。
- Camoufox（NSS）和 curl-impersonate（BoringSSL）都支持环境变量 `SSLKEYLOGFILE`。`analyze.cjs` 用导出的 TLS 1.3 密钥解密客户端方向的记录，把得到的 HTTP/2 字节交给 Node.js 自带的 HTTP/2 服务端解码。
- 每个请求输出：
  - HEADERS 帧上的优先级（权重、依赖、是否独占）；
  - 该流的第一个 WINDOW_UPDATE；
  - 请求头的名称和顺序。
- 只有固定白名单里的请求头会保留取值（`accept`、`sec-fetch-*`、`priority` 等）；Cookie 只保留名称，路径去掉查询串。

## 用法

需要 Python 3 和 Node.js 18 或更新版本。

1. 启动抓包代理，把记录写到仓库外或 `.tools/` 下：

   ```sh
   python -I tools/netfp/capture_proxy.py 18470 127.0.0.1 <原代理端口> <记录目录>
   ```

2. 用一份单独的配置启动 ais2api：`proxy` 指向 `http://127.0.0.1:18470`，并设置环境变量 `SSLKEYLOGFILE=<密钥文件>`。Camoufox 继承这个环境变量。
3. 分别用 Camoufox 后端和 SpiderMonkey 后端各做一次生成。真实账号的生成请求尽量少，遇到拒绝就停止。
4. 分析：

   ```sh
   node tools/netfp/analyze.cjs <记录目录> <密钥文件> --hosts aistudio.google.com,alkalimakersuite-pa.clients6.google.com,waa-pa.clients6.google.com --json <输出.json>
   ```

5. 比较两个后端的 JA4、HTTP/2 前几帧，以及同类请求的权重、流级 WINDOW_UPDATE 和请求头顺序。
6. **分析完立即删除记录目录和密钥文件**：解密后的内容含有账号的 Cookie。

## 升级时复核

Firefox、Camoufox 或 curl-impersonate 升级后，按上面的步骤重新抓一次 Camoufox 的基准：

- 有变化时，同步更新 `src/firefox_http.cpp` 的参数表和 `tests/test_firefox_fingerprint.cpp` 的期望值；
- 请求头的变化同步到 `src/spidermonkey_page.cpp`。
