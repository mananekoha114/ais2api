# Gemma 4 分词器的紧凑词表

`gemma4-bpe.zst` 由 Google 在 Hugging Face 发布的 [`google/gemma-4-E4B-it`](https://huggingface.co/google/gemma-4-E4B-it/tree/4ecc2ba63bcac23ba4a767fc87025b1450f84c9c) 中的 `tokenizer.json` 派生（Apache License 2.0，许可全文见 [`licenses/gemma4-tokenizer-LICENSE.txt`](../../licenses/gemma4-tokenizer-LICENSE.txt)）。Gemini 与 Gemma 4 使用同一个分词器，ais2api 用它在本地估算 token 数（`/v1/messages/count_tokens`），不访问上游。

| 项目 | 值 |
|---|---|
| 上游提交 | `4ecc2ba63bcac23ba4a767fc87025b1450f84c9c` |
| `tokenizer.json` 的 SHA-256 | `91d9cf86906f78e197021fda88214d448e5c120665d0243a8e776e046986c6bc` |
| `gemma4-bpe.zst` 的 SHA-256 | `9bff64d0ebcfd3ebf3543e319063e9a927d14f33c53373307cad87eb2c90d6ac` |

## 相对原文件的改动

这不是原文件，而是计数所需部分的另一种编码：

- 保留：单字符词条及其 id、字节回退词条（`<0x00>`–`<0xFF>`）的起始 id、全部 514,653 条合并规则（按原顺序，即优先级）；
- 舍弃：多字符词条的文本、24 个控制词条（`<eos>`、`<|turn>` 等）、规范化器和后处理器的配置；
- 格式：`A2TK` 版本 1，变长整数按列存放，用 zstd 压缩，详见 [`tools/tokenizer/derive_gemma4.py`](../../tools/tokenizer/derive_gemma4.py)。

计数器（`src/tokenizer.cpp`）按原文件的流程实现：空格替换为 `▁`，不预切分，按合并优先级做 BPE，词表外的字符回退为 UTF-8 字节词条。控制词条不识别，按普通文本计数。

## 重新生成与核对

两个脚本都需要 Python 3.10+，用 `python -I` 运行。原文件放进单独的空目录，只当数据读。

```sh
# 派生（需要 zstandard 包）。脚本先核对 tokenizer.json 的 SHA-256 和流程配置，结果应与上表一致
python -I tools/tokenizer/derive_gemma4.py <tokenizer.json> third_party/gemma4-tokenizer/gemma4-bpe.zst

# 用 Hugging Face tokenizers 生成参考计数（输入是字符串组成的 JSON 数组），再让测试逐条比对
python -I tools/tokenizer/reference_counts.py <tokenizer.json> <texts.json> <corpus.json>
AIS2API_TOKENIZER_CORPUS=<corpus.json> ais2api_tests "[.tokenizer-corpus]"
```

更换上游版本时，同步更新本表、两个脚本中的常量、`scripts/embed-tokenizer.cmake` 中的 SHA-256 和 `THIRD_PARTY_NOTICES.md`。
