# 价格快照

`gemini.json` 由 `scripts/scrape-gemini-pricing.py` 从 [Google 官方价格页](https://ai.google.dev/gemini-api/docs/pricing)
（`pricing.md.txt` 版本）抓取，构建时嵌入程序，作为从未联网成功时的价格。

运行中的服务从 `pricing-data` 分支下载最新版本，该分支由 `.github/workflows/pricing.yml` 每 6 小时更新一次，只在价格变化时提交。
发布新版本前可以刷新这份快照：

```sh
python3 -I scripts/scrape-gemini-pricing.py --output pricing/gemini.json --keep-unchanged
```

目录只收录 Standard 付费档的文本 token 单价（美元 / 百万 token）：`input`、`output`（含思考）、`cache_read`，
按提示长度分档（`above_tokens`），并按公布的生效日期分段（`from`，首段为 `null`）。
无法用文本 token 计价的模型（TTS、嵌入、Lyria 等）会被跳过；页面结构无法识别时脚本失败，不写出文件。
