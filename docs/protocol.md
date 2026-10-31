# 协议合同

本文定义三种数据 API（Gemini、OpenAI Chat Completions、OpenAI Responses）的支持范围与转换规则。面向使用者的简要说明见 [客户端接入](clients.md)。

本项目的实现是独立编写的 C++ 代码。参考项目的 `docs/WIRE_SPECIFICATION.md` 仅作为上游协议的描述来源，没有复用其实现或测试样例。上游协议细节集中在 `src/protocol.cpp`，Responses 适配器位于 `src/responses.cpp`。

## 总体流程

所有 API 适配器先把请求归一化为标准 Gemini 请求对象，再由浏览器传输层基于页面中实时捕获的模板重新编码为 JSPB 请求：

- 每次请求都会替换或清空模板中已知的生成字段、contents、model、签名、系统提示、tools 与缓存内容字段；未知字段保留，以兼容上游变化。
- 模型别名（`model_aliases`）在编码前解析。
- 服务不保存聊天历史，客户端每次提交完整历史，可以任意编辑或删除单条消息。

## Gemini 路由

原生 Gemini 模型路由允许重复的 `/v1beta` 前缀，以及版本段之后紧跟的多余斜杠，例如 `/v1beta/v1beta/models` 和 `/v1beta//v1beta/models/MODEL:streamGenerateContent`。这是为了兼容末尾已经带 `/v1beta/` 的 SillyTavern 代理地址。列表、详情、普通生成和 SSE 请求都使用标准的 `/v1beta/models` 处理逻辑，鉴权不变。模型资源名、查询字符串、其他 API 版本和非 Gemini 路由不做归一化。

## 模型发现

模型发现与生成引导相互独立：等待所选账号身份确认后，捕获页面真实的 ListModels 请求（fetch 或 XHR），重放前检查账号槽位。它不需要提示词编辑器、Run 按钮、生成模板或快照服务。

- 手动刷新每次重新请求上游；校验并完整保存后返回 `saved`，记录按账号持久保留，不随浏览器退出或服务重启消失。
- `/v1/models`、`/v1beta/models` 及模型详情读取本地清单，不启动浏览器或排队发现。当前账号无保存记录时返回显式配置的 `configured_models`（`configured`），二者都没有时返回空列表。
- 读取和刷新使用当前账号，其次是首选账号，均不存在时使用第一个账号。刷新失败或保存失败明确报告错误并保留旧结果，备用配置不作为发现结果保存。
- 页面身份缺失或不明确，与“确认是另一个账号”是两种情况，都不允许跨账号复用结果。
- 不会虚构模型目录或 API Key。

## 内容与消息

支持的 Gemini 内容部件：`text`、`inlineData`、`functionCall`、`functionResponse`、`executableCode`、`codeExecutionResult`。

- 每个部件保留各自的 `thought` 标志和 `thoughtSignature`。
- 函数值使用带类型的 protobuf Struct/Value 编码，正确处理 null、嵌套对象、布尔值、空列表与单元素列表。
- `fileData` 与 Files API 明确不支持。
- 每条 content 作为独立的 user/model 消息保留在原位置，包括连续的同角色消息。
- `systemInstruction` 单独编码（上游以 user 角色表示）。
- 未提供 tools 或 tools 为空时不启用任何工具，也不会插入搜索或分隔消息；[搜索模型](#search-models) 除外。
- 函数结果中的文本/内联数据附件会展开为相邻的内容部件。

示例与客户端限制见 [真 role 与思考签名](true-role.md)。

## 生成参数

### 思考

- `thinkingLevel` 不区分大小写。未指定时，文本模型默认 `HIGH`，id 中含 `image` 的模型默认 `MINIMAL`（与参考项目一致）。
- `includeThoughts` 接受布尔值。`false` 会在聚合或 SSE 输出前移除思考文本，保留签名、函数调用和 usage；它不会关闭模型推理。
- `thinkingBudget`（≥ -1 的整数）与 `seed`（整数）为兼容原生 Gemini 客户端而接受，但**被忽略**：私有上游没有经过验证的对应字段。字段名（不含值）记录在 `generation_parameters_ignored` 日志中。`thinkingBudget: 0` 不会关闭推理，也不会取消默认等级。

### 字段别名与取值

- 请求容器、内容部件、生成设置和工具声明接受对应的 snake_case 写法（包括 SillyTavern 的 `function_declarations` 与 `google_search`）。同一字段出现两种拼写时报错，而不是任选其一。调用方自己的函数参数、返回值和 schema 属性名保持原样。
- `candidateCount: 1` 按单候选处理，其他值报错。
- `responseModalities` 不区分大小写。文字与图片同时输出时按 `IMAGE,TEXT` 的规范顺序编码。
- 已废弃的 `HARM_CATEGORY_CIVIC_INTEGRITY` 仅在 `OFF` 或 `BLOCK_NONE` 时作为无操作接受；启用该类别或其他不支持的类别仍报错。四个受支持的安全类别保留请求的阈值。
- 原生模型列表与详情始终提供非空的 `displayName`，缺失时回退为模型 id。

### 支持的设置与默认值

生成设置包括：采样参数、停止序列、token 上限、响应 MIME/schema、惩罚项、logprobs 请求标志、文字/图片模态、思考配置、媒体分辨率以及图片宽高比与尺寸。

- `model_defaults` 中与模型 id 精确匹配的条目逐项填充缺失的生成与思考字段，请求中显式提供的字段优先。
- 省略 `safetySettings` 时，Gemini/Gemma 文本模型的四个安全类别默认为 `OFF`。
- 图片模型默认只输出 `IMAGE`，并按参考项目的传输规则省略安全设置、响应 MIME/schema 和媒体分辨率。
- 请求 logprobs 不保证返回 logprob 详情，该上游响应字段没有文档。

### 响应解析

响应部件采用宽松解析（与参考客户端一致）：

- 未知槽位，以及格式异常的非函数载荷（文本、内联数据、代码、执行结果、签名）会被跳过，而不是让整个生成失败。
- 格式异常的函数调用或函数结果仍以 `invalid_upstream` 失败。
- 被跳过的位置（如 `part[9]`、`candidate[9]`，不含内容）每个请求记录一次 `upstream_fields_ignored` 日志。
- 代码部件按 `[languageCode, source]`、执行结果按 `[outcomeCode, output]` 解析，枚举值原样保留。

## JSON Schema

三种 API 的工具参数与响应格式共用同一个 schema 适配器。

**直接支持**：具体的 string/number/integer/boolean/array/object 类型、可空类型数组、嵌套 properties/items、enum/required、长度与数值范围、pattern、example、oneOf/anyOf/allOf/not 以及 propertyOrdering。Gemini 还接受 `responseJsonSchema` / `response_json_schema` 作为 `responseSchema` 的替代，两者同时出现时报错。

**本地引用**：本地 JSON Pointer 形式的 `$ref`、`$defs` 与 `definitions` 会被展开（支持 `~0` / `~1` 转义）。远程、递归、无法解析或与同级字段冲突的引用明确报错。展开上限为深度 64、4096 个节点、累计 8 MiB。

**降级为描述提示**（仅限非 strict schema）：`additionalProperties`（布尔或 schema）、`title`、`default` 与非字符串的标量 enum 会被接受。私有上游没有对应字段，因此这些约束会转为描述中的提示文字；`$schema`、`$id`、`$comment` 直接省略。被降级的关键字去重后记录在 `generation_parameters_ignored` 中（不记录属性名或值）。这只是对模型的提示，不是强制约束，客户端必须自行校验模型输出和工具参数。

**其他规则**：字符串 `const` 转为单元素 enum；同类标量 enum/const 可推断类型。任意未知关键字、布尔 schema、元组 schema，以及无法推断具体类型的无约束 schema 仍不支持；联合类型仍需要具体或可推断的类型。OpenAI 的 strict JSON schema / strict 函数模式默认被拒绝，因为上游无法保证其语义；可通过下文的开关舍弃该要求。

## 工具

- 支持函数声明、代码执行、Google 搜索、Google 地图与 URL context。`parametersJsonSchema` 可替代 `parameters`，两者同时出现时报错。
- `NONE` 移除所有工具；`allowedFunctionNames` 过滤自定义函数。
- `ANY`、OpenAI 的 `required` 或指定工具、`VALIDATED` 在本地拒绝：私有上游没有经过验证的强制调用或验证解码字段。参考实现在这些模式下只是过滤工具，本项目不把过滤伪装成强制执行。
- Chat Completions 与 Responses 默认拒绝 `parallel_tool_calls: false`，因为上游无法保证只调用一次。
- 内置工具的模型限制与参考项目一致：图片模型只允许搜索；其他 Gemini 模型允许搜索、代码执行、地图与 URL context；其他模型系列允许搜索与代码执行。不兼容的内置工具在本地报错，而不是被静默移除。这些限制描述的是请求兼容性，不保证 Google 向某个账号开放对应能力。

### Google 搜索

- 同时接受 `googleSearch` 与 `google_search`；`searchTypes` 及其嵌套的 `webSearch` / `imageSearch` 也接受 snake_case。
- 重复的内置工具会去重；函数声明合并为一组，排在内置工具之后（与参考客户端一致）。
- 公开的 `timeRangeFilter`（`startTime` / `endTime` 字符串）与 `excludeDomains`（字符串列表）会校验格式，然后舍弃并记为忽略参数，搜索保持开启。其他未知的 googleSearch 字段仍报错。
- id 含 `image` 的模型把请求的网页/图片搜索合并为一个搜索工具；其他模型使用普通网页搜索。
- 带有工具的文本请求会附带浏览器当前的时区信息（独立调用编解码器时为 UTC）。捕获模板中的工具和位置信息不会泄漏到后续请求。
- RikkaHub 的 `toolConfig.includeServerSideToolInvocations` 布尔值会被接受，使自定义函数与内置工具混用时不在本地报错；但它要求的额外调用记录没有可验证的映射，因此记为忽略参数，不保证返回这些记录。
- 旧版 `googleSearchRetrieval.dynamicRetrievalConfig` 接受 `MODE_DYNAMIC` / `MODE_UNSPECIFIED` 与 [0,1] 范围内的阈值，然后映射为新版 Google 搜索；动态检索控制同样记为忽略参数。
- 这些转换只负责开启工具，是否执行由模型决定。私有响应中的引用与搜索来源元数据目前不解码。
- 请在 RikkaHub 与 SillyTavern 中使用 Google 提供商 / Google AI Studio 路线；Google Interactions 不支持。

<a id="search-models"></a>

## 搜索模型

以 `-search` 结尾的模型 id（不区分大小写，在解析 `model_aliases` 之后判断）表示“基础模型 + 默认开启 Google 搜索”，对应参考项目的 `default_tools`。后缀会在应用模型默认值、账号调度、配额统计和上游请求之前去掉，响应中的 `model` 也是基础模型 id（与别名的处理方式相同）。因此，本身 id 就以 `-search` 结尾的真实上游模型无法直接调用。

- **没有 tools**：文本模型加入 `{"googleSearch":{}}`；id 含 `image` 的模型加入网页加图片搜索。
- **有 tools 但没有函数声明**：追加 Google 搜索，已存在 googleSearch / googleSearchRetrieval 时不重复添加。
- **有函数声明**：上游不允许 Google 搜索与自定义函数同时使用。`search_model_drop_functions` 为 `true`（默认）时，舍弃函数声明、加入搜索，并把 `tools.functionDeclarations` 与非 NONE 的 `toolConfig.functionCallingConfig` 记为忽略参数；为 `false` 时保留调用方的工具不变，不加入搜索。
- `functionCallingConfig.mode: NONE` 仍会禁用包括搜索在内的所有工具。

不带后缀的 id 不会默认搜索，除非请求自己声明。模型列表（`/v1/models`、`/v1beta/models` 及其详情路由）会在每个支持 generateContent 的 Gemini、Gemma 和图片模型（不含 embedding 与 TTS 模型）之后追加 `<id>-search` 条目，`source` 为 `search_variant`；Gemini 列表的 displayName 追加 ` (Search)`。真实模型 id 不会被覆盖。管理控制台的模型页只显示保存的发现结果或备用配置。

Chat Completions 的 `web_search_options` 对所请求的模型应用同样的规则。`search_context_size`（low/medium/high）、`user_location`（`{"type":"approximate","approximate":{city,country,region,timezone}}`）与 `filters.allowed_domains` 会校验格式并记为忽略参数，没有经过验证的私有字段可映射。Responses 的 `web_search` / `web_search_preview` 工具（每个请求一个）会转换为这些选项，扁平的位置字段转换为 Chat 的嵌套格式；用 `tool_choice` 指定网页搜索仍不支持。由于不返回搜索来源，Responses 不输出 `web_search_call` 项。

<a id="optional-unsupported-parameter-dropping"></a>

## 可选：舍弃不支持的参数

`ignore_unsupported_parameters` 默认为 `false`。可通过以下任一方式开启：管理控制台「配置 → API 兼容性」、配置 PATCH、在 `config.json` 设置 `"ignore_unsupported_parameters": true` 后执行 `ais2api config reload`，或设置环境变量 `AIS2API_IGNORE_UNSUPPORTED_PARAMETERS=true`。它支持热更新，对之后三种 API 的生成请求生效。关闭后恢复默认的拒绝策略；它不影响上文所述的非 strict schema 降级转换。

开启后，已知不支持的可选参数会在编码前移除，字段名（不含值）记录在 `generation_parameters_ignored` 中：

- **Chat**：strict 函数/JSON 标志、强制或指定的 `tool_choice`、`parallel_tool_calls: false`、`n > 1`、`store: true`、`seed`、`logit_bias`、`service_tier`，以及无法映射的 `none` / `xhigh` 推理强度。其余工具与受支持的 schema 约束保留。
- **Gemini**：`candidateCount > 1`、`ANY` / `VALIDATED` 函数调用模式、`cachedContent`、不支持的音频输出/语音/路由/模型选择选项、不支持的内置工具选项、与模型不兼容的代码/地图/URL 工具，以及启用的已废弃 civic-integrity 过滤。受支持的工具、内容与历史保留。不支持的选项对象整体舍弃；会检查对象形状，但不会校验其内部字段。
- **Responses**：存储/后台/续接控制、额外的 include 数据、自动截断、service tier、摘要请求、并行限制、强制工具选择、strict 标志，以及已识别的不支持内置工具。请求仍只使用本次提交的完整历史同步执行，不保存响应。响应中的选项字段反映被移除的顶层控制和 strict 标志。
- **Schema**：在结构校验之后，逐个移除已知不支持的 JSON Schema 约束（如 `multipleOf`、exclusive 边界、`patternProperties`、元组前缀/contains/uniqueItems、条件/依赖/unevaluated 约束、content 注解、读写标志）。如果一个合法的可选 schema 完全无法表示（远程/递归引用、布尔/元组/无约束/多类型形式），则移除整个根 schema：被移除的输出 schema 保留 application/json MIME 模式，被移除的函数 schema 保留函数名与描述。

被舍弃的能力不会被强制执行或模拟。未知的请求/schema 关键字、保留字段格式错误、缺少必填输入、定义冲突、无效引用、不支持的内容或文件附件、鉴权失败与资源超限仍会明确报错。这不是对任意上游 400 的捕获重试策略，不会新增不支持的接口，也不会静默丢弃对话内容。

## OpenAI Chat Completions

### 消息映射

- 标准的 user / assistant / system / developer / tool 消息映射为 Gemini contents 与系统提示。
- 支持文本、`image_url`（data URL 或公开 HTTPS 地址）以及 `input_audio`（wav/mp3）。音频与图片最终是否可用取决于所选模型。
- 开头连续的 system / developer 消息成为 `systemInstruction`；对话开始后出现的 system / developer 消息保留在原位置，转为不带标签的 user 消息。
- 连续的普通 user / assistant 消息不会合并；连续的 tool 结果合并为一条 user 消息，内含按顺序排列的 functionResponse 部件。
- 函数工具、`tool_choice`、JSON 响应格式、token/采样控制与 `reasoning_effort` 均会转换。只支持 `n = 1`。不支持的字段以 `invalid_argument` 失败，不会被当作隐含支持。
- 函数参数 JSON 必须是对象；非对象或字符串形式的工具结果包装为 `{"result": ...}`。

### 远程图片

远程图片只允许端口 443 上的公开 HTTPS 地址。加载器在每次重定向时校验所有解析出的地址，并把每个连接（包括代理隧道）固定到已校验的 IP，同时保留原主机名的 TLS 校验。它使用网关的 `proxy` 设置（包括 `AIS2API_PROXY`）；`HTTPS_PROXY`、`NO_PROXY` 等通用 curl 环境变量不影响这条路径。支持公网 IPv6；带作用域的、私有的和特殊用途的地址会被拒绝。协议适配器本身从不抓取 URL。

### 工具调用 ID 与签名

带签名的工具调用使用由客户端携带的 `call_gemini_` ID，内容为 base64url 编码的 JSON `[原始ID或null, 签名, nonce]`。没有上游 ID 的无签名调用使用同样的格式，签名为 null。归一化时会在原始 functionCall 上恢复原 ID（包括“没有 ID”）和签名；functionResponse 只获得原 ID。

- 客户端应把整个 ID 当作不透明文本保存；签名从不依赖服务端存储重建。
- 流式输出时，每个鉴权主体的响应注册表跨数据块分配递增的序号与索引；不同数据块中相同的无 ID 调用视为不同调用；重复的显式上游 ID 保持原索引。注册表只保存索引与函数元数据，不保存签名。单个响应最多登记 4096 个调用。

### 输出

- 输出包含 content、tool_calls、finish_reason 与 token usage。
- 为兼容客户端，消息和 delta 额外提供 `reasoning_content` 与 `thought_signature`。输入的 assistant 消息接受 `reasoning_content`、`thought_signature` 或其别名 `signature`。文本签名成为独立的空文本部件；函数签名保留在对应的 functionCall 上。这些扁平字段无法表示交错的原生部件。
- 富内容的 assistant 输出同时附带原生 Gemini 内容，以便精确往返；流式 delta 附带归一化后的原生数据块。原生内容与兼容字段同时提供时必须一致。
- 思考文本不会混入可见 content；图片和代码部件保留在原生扩展中。
- 完整的工具调用以完整的参数字符串输出，不会伪造部分参数片段。
- 请求 `stream_options.include_usage` 时，普通数据块的标准 usage 为 null，`[DONE]` 之前另有一个 choices 为空、带实际 usage 的数据块；未请求时省略标准 usage。流 ID 与 created 时间戳保持不变；中途失败会输出错误且不发送 `[DONE]`。以上遵循 [OpenAI 流式合同](https://developers.openai.com/api/reference/resources/chat/subresources/completions/streaming-events)。

## OpenAI Responses

`POST /v1/responses` 与 Chat Completions 共用鉴权、生成调度、上游编解码、媒体加载和取消路径。适配器先把 Responses 输入转换为 Chat 消息，再归一化为 Gemini 请求。使用示例与支持子集见 [Responses 说明](responses.md)。

- 接口是无状态的：客户端提交完整输入历史并保留函数 `call_id`。字符串输入、instructions、消息内容、函数调用与结果、扁平函数声明、兼容的采样/token/推理控制以及非 strict 文本格式都映射到现有编解码器。data URL 图片直接归一化；HTTPS 图片使用与 Chat 相同的受限公网媒体加载器。存储、后台任务、引用和其他内置工具明确报错。
- reasoning 与 message 输出项可以携带 `ais2api.gemini_content` 和兼容的文本签名。保留这些字段即可重放原生部件；由于 Responses 把推理、可见/原生内容和函数调用分到不同输出项，这种表示不保证原始的跨项部件交错顺序。函数调用 ID 与 Chat 使用同样的签名格式。参数完全相同的重复显式 ID 视为一次调用，参数冲突时报错；无 ID 的调用保留各自的序号。
- JSON 结果为带类型输出项和 Responses usage 的 Response 对象。命名 SSE 记录的 `event` 与 JSON 的 `type` 一致，带递增的 `sequence_number`；依次输出响应创建/进行中、输出项与内容部件生命周期、文本/推理/函数参数增量事件，最后是 completed 或 incomplete。token 用尽映射为 `incomplete_details.reason=max_output_tokens`，过滤映射为 `content_filter`。中途失败输出 `response.failed`，已发送的内容不会重试。不追加 Chat 的 `[DONE]`。流状态受对象限额约束，超出则响应失败。以上遵循 [OpenAI Responses 事件参考](https://developers.openai.com/api/reference/resources/responses/streaming-events)。

## ais2api 扩展（版本 1）

扩展的 schema 为 [protocol-extensions.schema.json](../schemas/protocol-extensions.schema.json)。合同以外的任何扩展名都会被拒绝。

### 请求扩展

OpenAI 标准字段无法表达的能力在请求根部提供：

```json
{
  "ais2api": {
    "gemini": {
      "generationConfig": {"responseModalities": ["TEXT", "IMAGE"]},
      "tools": [{"googleSearch": {}}]
    }
  }
}
```

允许的 gemini 键为 `generationConfig`、`tools`、`toolConfig`、`safetySettings` 与 `systemInstruction`。已经通过标准字段提供的字段再次出现会报错，即使值相同；`generationConfig` 只有在子键互不重叠时才允许合并。

### 原生内容往返

assistant 消息可以通过 `message.ais2api.gemini_content` 往返原生内容：

```json
{
  "role": "assistant",
  "content": "answer",
  "ais2api": {
    "gemini_content": {"role": "model", "parts": [{"text": "answer", "thoughtSignature": "opaque-signature"}]}
  }
}
```

- 与该信封同时提供的标准可见文本和 tool_calls 必须与之一致；原生图片、思考和签名以信封为准。
- 对于编码过的调用，输出信封携带与标准 `tool_calls` 相同的客户端 `functionCall.id`；归一化时恢复原 ID 或“没有 ID”。该替换只作用于扩展的副本，其他原生字段、部件顺序和各部件的原始签名保持不变。因此把生成的标准字段与原生信封一起回传，可以通过严格的 ID/名称/参数一致性检查。
- 仅支持 assistant 消息。客户端应把原生扩展数据当作不透明数据保存，不要伪造或编辑签名。

### 流式扩展

OpenAI 流式数据块在 `choices[].delta.ais2api.gemini_delta` 中携带完整的归一化 Gemini 数据块。编码过的函数调用在该扩展副本中使用与标准 delta 相同的客户端 ID，原 ID 与签名在请求归一化时恢复。没有 choice 的纯元数据块把 `ais2api.gemini_delta` 放在根部。兼容的 `reasoning_content` / `thought_signature` 位于各个 delta 上，精确的原生部件顺序保留在扩展中。

## 流式解析与聚合

- 上游帧格式为可选的 `)]}'` XSSI 前缀，后接外层数组，内含若干批响应数据块数组。有界解析器每解析出一个深度为 3 的数据块就输出并丢弃其字节，然后才读取下一个。
- 网络分块位置、UTF-8 码元、JSON 转义以及包含括号的字符串可以出现在任何位置。无效前缀、无效的数据块 JSON/UTF-8、嵌套过深、数据块过大、外层分隔符错误以及末尾截断都会明确失败。解析器只缓存当前数据块和两个字节的外层状态，不缓存完整响应；只有外层数组完整闭合，`finish()` 才算成功。
- 聚合按序列化后的字节数设限。思考标志相同的相邻普通文本部件可以拼接；带签名的部件和其他原生载荷保持独立。usage 与 finish 元数据以最新快照替换，而不是累加。大小检查失败时保留之前的聚合结果不变。内容判定忽略响应 ID、usage 和仅含 finish 的元数据，从而区分元数据与第一个可见内容或模型动作。
- 结束原因覆盖全部已知上游代码。OpenAI 把 token 用尽映射为 `length`，安全/复述拦截映射为 `content_filter`，函数调用完成映射为 `tool_calls`，其他完成映射为 `stop`。`candidatesTokenCount` 包含可见输出与思考 token，`thoughtsTokenCount` 单独提供；OpenAI 输出 `reasoning_tokens` 与 `cached_tokens` 明细。HTTP/gRPC 错误分类与重试由运行时和传输层负责，不在编解码器内。

## HTTP 细节

鉴权方案与 JSON 媒体类型不区分大小写，并按精确的词边界比较；支持带 charset 参数的 `application/json`，拒绝 `application/jsonp`。

### 请求用量与参考费用

通过生成鉴权的 Chat Completions、Responses、Gemini 调用各有一个持久请求记录。参数拒绝、排队失败、取消与流式中断同样记录结果，内部重试归入同一请求的尝试列表。生成完成与 HTTP 传输完成分别记录，两者结束后在同一事务中写终态与累计；服务启动时把遗留未完成记录标为中断。鉴权前拒绝、管理和健康检查不写入请求历史。

每次尝试只采用最新用量快照，跨尝试的可确认用量只归集一次。此处消费本项目解码器的归一结果：`promptTokenCount` 包含缓存输入，`candidatesTokenCount` 已包含 `thoughtsTokenCount`。计价把非缓存输入、缓存读取、可见输出和思考分列，思考按输出单价计费且不会再次增加总输出。未知消耗保留未完整状态；失败不自动等于零消耗。尚未进入任何上游尝试的拒绝请求可确认费用为零。

价格来源按 LiteLLM 的 Google Gemini 直连目录、models.dev 的 Google 提供方目录排序，使用解析别名和 search 后缀后的真实模型精确匹配。单价以皮美元/token、金额以纳美元保存，每个分项四舍五入到纳美元再相加；上下文阶梯按单次尝试的完整输入量判断。执行时选定的来源、获取时间、单价和金额随请求保存，后续更新不改写历史费用。需要独立计费的搜索、非文本模态或其他无法可靠还原的维度保持未完整计价；估算不是实际账单。

主请求数归属最终执行账号，已确认用量按各尝试的执行账号归集。完整费用字段只计全部尝试均完整计价的请求；已知小计另行纳入所有已确认尝试的金额，并按实际尝试账号归集，所以账号用量行可能有消耗而没有最终归属请求。`PROHIBITED_CONTENT` 等内容拦截属于生成失败，传输完成和收到上游终态不改变失败分类；已返回的正文与可确认用量仍保留。Chat Completions 使用 `content_filter`，Gemini 保留原始 `finishReason`，Responses 使用 `incomplete` / `response.incomplete`。明细到期后保留累计，旧统计单列原口径，详见[管理 API](management-api.md#请求记录与估算费用)。

## 验证范围

Catch2 协议测试使用独立编写的合成样例，覆盖任意字节边界、格式错误或截断的流、各类上限、带类型的函数值、逐部件签名、schema 递归、请求模板替换、扩展冲突、多消息工具以及 OpenAI 原生往返。回归用例还覆盖跨数据块的相同无 ID 调用、显式 ID 的索引稳定、有/无签名调用不会错绑签名，以及生成的标准/原生工具消息一起回传时的严格冲突检查。

这些是协议与适配器测试，不能证明某个已登录的真实上游模型支持每项能力。真实浏览器与平台的验收结果另见 [验证记录](validation.md)。
