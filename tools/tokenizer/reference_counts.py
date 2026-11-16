# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
"""Counts tokens with Hugging Face `tokenizers` as the reference for src/tokenizer.cpp.

Input: the pinned tokenizer.json (see third_party/gemma4-tokenizer/README.md) and
a JSON array of strings. Output: a JSON array of {"text": ..., "count": ...},
which tests/test_tokenizer.cpp reads when AIS2API_TOKENIZER_CORPUS names it.
Texts that contain a control token such as <eos> are refused, because the
local counter treats them as ordinary text.

Requires Python 3.10+ and the `tokenizers` package. Run with `python -I`.
Usage: reference_counts.py <tokenizer.json> <texts.json> <output.json>
"""
import json
import sys

from tokenizers import Tokenizer


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: reference_counts.py <tokenizer.json> <texts.json> <output.json>")
    tokenizer = Tokenizer.from_file(sys.argv[1])
    special = [token.content for token in tokenizer.get_added_tokens_decoder().values()]
    with open(sys.argv[2], encoding="utf-8") as handle:
        texts = json.load(handle)
    if not isinstance(texts, list) or not all(isinstance(text, str) for text in texts):
        sys.exit("reference_counts: the input must be a JSON array of strings")
    for text in texts:
        for token in special:
            if token in text:
                sys.exit("reference_counts: a text contains the control token %r" % token)
    counts = [len(encoding.ids) for encoding in tokenizer.encode_batch(texts, add_special_tokens=False)]
    with open(sys.argv[3], "w", encoding="utf-8", newline="\n") as handle:
        json.dump([{"text": text, "count": count} for text, count in zip(texts, counts)], handle, ensure_ascii=False)
    print("%d texts, %d tokens" % (len(texts), sum(counts)))


if __name__ == "__main__":
    main()
