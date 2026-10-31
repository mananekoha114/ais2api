# 客户端接入

ais2api 同时提供 Gemini 原生接口和 OpenAI 兼容接口。本文说明接口地址、鉴权方式、常见客户端的填写方法，以及搜索模型等功能的用法。字段级的支持范围见 [协议合同](protocol.md)。

## 接口一览

默认地址为 `http://127.0.0.1:18451`。手机或其他设备访问时，请把 `127.0.0.1` 换成服务所在机器的局域网地址，并把配置项 `bind` 改为 `0.0.0.0`（见 [配置参考](configuration.md#让局域网或手机访问)）。

| 用途 | 方法与路径 |
|---|---|
| OpenAI 模型列表 | `GET /v1/models`、`GET /v1/models/{id}` |
| OpenAI Chat Completions | `POST /v1/chat/completions` |
| OpenAI Responses | `POST /v1/responses`（见 [Responses 说明](responses.md)） |
| Gemini 模型列表 | `GET /v1beta/models`、`GET /v1beta/models/{id}` |
| Gemini 生成 | `POST /v1beta/models/{model}:generateContent` |
| Gemini 流式生成 | `POST /v1beta/models/{model}:streamGenerateContent?alt=sse` |
| 进程存活 | `GET /health`（无需鉴权） |
| 本地前提就绪 | `GET /ready`（无需鉴权） |

另有管理接口 `/api/admin/v1/`（见 [管理 API](management-api.md)）、Cookie 导入接口 `/api/accounts/probe-import`（见 [Cookie 导入](userscript.md)）和根路径 `/` 的 WebUI。

## 鉴权

生成与模型接口始终需要 API Key，以下任意一种方式均可：

- `Authorization: Bearer YOUR_KEY`
- `x-goog-api-key: YOUR_KEY`
- `x-api-key: YOUR_KEY`
- 查询参数 `?key=YOUR_KEY`

Key 来自 `config.json` 的 `api_key`，或通过管理 API / WebUI 创建的托管密钥。管理密码（`admin_key`）和 Cookie 导入密钥都不能用于生成接口。HTTP 日志不记录 URL 查询、请求头、正文或返回内容。

## 常见客户端

### Cherry Studio

- 提供商类型：OpenAI 兼容
- Base URL：`http://127.0.0.1:18451/v1`
- API Key：你的 `api_key`
- 模型：从列表中选择

工具与结构化输出中常见的 `additionalProperties`、`title`、`default` 和本地 `$ref` 都可以使用；上游无法原生执行的 schema 限制会转成提示写入描述，客户端仍需自行校验输出。

### SillyTavern

- 聊天补全来源：Google AI Studio
- 反向代理地址：`http://127.0.0.1:18451`，或社区常用的 `http://127.0.0.1:18451/v1beta/`（末尾斜杠可省略）
- 代理密码：你的 `api_key`

SillyTavern 自动追加版本路径造成的重复 `/v1beta` 和多余斜杠会被自动归一化，模型列表、详情、普通与流式生成均可正常使用。

### RikkaHub

- 提供商类型：Google
- Base URL：`http://127.0.0.1:18451/v1beta`
- API Key：你的 `api_key`

### OpenAI SDK

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:18451/v1", api_key="YOUR_KEY")
reply = client.chat.completions.create(
    model="gemini-3.1-flash-lite",
    messages=[{"role": "user", "content": "你好"}],
)
print(reply.choices[0].message.content)
```

### Gemini 原生请求

```sh
curl "http://127.0.0.1:18451/v1beta/models/gemini-3.1-flash-lite:streamGenerateContent?alt=sse" \
  -H "x-goog-api-key: YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"contents":[{"role":"user","parts":[{"text":"你好"}]}]}'
```

## 模型列表

模型只来自两种来源：浏览器实时发现后保存在本地的清单（`saved`）和配置项 `configured_models`（`configured`）。读取列表不会访问上游；清单在导入账号后自动获取一次，之后通过管理页面或 `ais2api models list` 手动刷新。项目不内置“声称可用”的模型清单，列表中出现的模型也不保证当前账号有权限使用。

列表会为每个支持生成的 Gemini、Gemma 和图片模型追加一个 `<模型>-search` 条目（来源为 `search_variant`），见下一节。

## 搜索

### 搜索模型 `*-search`

使用以 `-search` 结尾的模型 id（例如 `gemini-3.1-flash-lite-search`）时：

- 客户端不传 tools 也会默认开启 Google 搜索；图片模型同时开启网页和图片搜索。
- 实际请求、账号调度、配额统计和响应中的 `model` 都使用去掉后缀的基础模型名。
- 上游不允许搜索与自定义函数同时使用。默认舍弃函数、保留搜索；可在 WebUI「配置 → API 兼容性」关闭“搜索模型舍弃自定义函数”（配置项 `search_model_drop_functions`），改为保留客户端工具、不加搜索。
- 不带后缀的模型在客户端不传 tools 时不搜索。

### 其他开启方式

- Gemini 原生：在 `tools` 中声明 `{"googleSearch": {}}`（也接受 `google_search`）。
- Chat Completions：传入 `web_search_options`。
- Responses：使用 `web_search` 或 `web_search_preview` 工具。

搜索是否实际执行由模型决定。搜索来源与引用元数据目前不解析，Responses 也不输出 `web_search_call` 项。`search_context_size`、`user_location`、`allowed_domains`、`timeRangeFilter`、`excludeDomains` 等选项会校验格式但不生效。

## 思考与签名

- Gemini 的 `thinkingLevel` 可用（不区分大小写）；未指定时使用官方文档中的模型默认等级（如 Gemini 4 为 `MEDIUM`），未列出的文本模型默认 `HIGH`，图片模型默认 `MINIMAL`。Gemini 4 不支持 `MINIMAL`。
- 原生 Gemini 接口与官方一致，只有 `includeThoughts: true` 时才返回思考文本；未设置或为 `false` 时在网关隐藏，但模型仍会思考，签名、函数调用和 usage 保留。
- `thinkingBudget` 与 `seed` 为兼容客户端而接受，但**不会生效**；`thinkingBudget: 0` 也不会关闭思考。例外：Gemini 4 上单独的 `thinkingBudget: 0` 映射为 `LOW`，与 `thinkingLevel` 同时提供时报错。
- Gemini 4 每次最多输出 32,768 token（含思考），不支持官方的续写（`continuationToken`）；更大的输出上限会被压到 32768，超长回答以 `MAX_TOKENS` / `length` 截断。
- OpenAI 响应额外提供 `reasoning_content` 与 `thought_signature` 字段。多轮对话中原样回传模型给出的签名和 `call_gemini_...` 工具调用 ID 即可，详见 [真 role 与思考签名](true-role.md)。

## 兼容开关：忽略不支持的参数

默认情况下，上游无法保证的参数会被明确拒绝（例如 strict schema、强制工具调用、`n > 1`、`parallel_tool_calls: false`）。如果希望舍弃这些参数后继续生成，可以：

- 在 WebUI「配置 → API 兼容性」打开“忽略不支持的参数”；或
- 在 `config.json` 设置 `"ignore_unsupported_parameters": true` 后执行 `ais2api config reload`；或
- 设置环境变量 `AIS2API_IGNORE_UNSUPPORTED_PARAMETERS=true`。

开启后被舍弃的字段名会写入日志，但被舍弃的能力不会被模拟。未知参数、无效 JSON、字段冲突、资源超限等仍会报错。完整清单见 [协议合同](protocol.md#optional-unsupported-parameter-dropping)。

## 不支持的功能

- Responses 为无状态模式：没有服务端响应存储、`previous_response_id` 续接或后台任务。
- 没有 Files、embeddings、独立的图片生成或音频接口；`fileData` 不支持。
- 强制工具调用（`ANY` / `required` / 指定函数）与 `VALIDATED` 模式会被拒绝。
- 同一时间只处理一个生成请求，其余请求排队（默认最多 8 个，最长等待 30 秒）。

## 健康检查

- `/health` 只表示 HTTP 进程存活。
- `/ready` 检查 API Key、本地账号与浏览器等前提，并明确返回 `google_verified: false`。真实可用性需要通过账号探测和实际生成确认。
