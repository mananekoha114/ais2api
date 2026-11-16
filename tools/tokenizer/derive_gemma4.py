# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
"""Derives the compact Gemma 4 BPE table that ais2api embeds for local token counts.

Input: tokenizer.json of google/gemma-4-E4B-it (Apache-2.0) at the pinned revision.
Output: a zstd frame holding only what counting needs: the single-character
pieces, the byte-fallback base id and the merge table in rank order.

Requires Python 3.10+ and the `zstandard` package. Run with `python -I`.
Usage: derive_gemma4.py <tokenizer.json> <output.zst>
"""
import hashlib
import json
import sys

import zstandard

REVISION = "4ecc2ba63bcac23ba4a767fc87025b1450f84c9c"
TOKENIZER_SHA256 = "91d9cf86906f78e197021fda88214d448e5c120665d0243a8e776e046986c6bc"
MAGIC = b"A2TK"
VERSION = 1


def fail(message):
    sys.exit("derive_gemma4: " + message)


def varint(value, out):
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return


def main():
    if len(sys.argv) != 3:
        fail("usage: derive_gemma4.py <tokenizer.json> <output.zst>")
    data = open(sys.argv[1], "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != TOKENIZER_SHA256:
        fail("tokenizer.json sha256 %s is not the pinned %s (revision %s)" % (digest, TOKENIZER_SHA256, REVISION))
    tokenizer = json.loads(data)
    # The counter implements exactly this pipeline; refuse anything else.
    if tokenizer["normalizer"] != {"type": "Replace", "pattern": {"String": " "}, "content": "▁"}:
        fail("unexpected normalizer")
    if tokenizer["pre_tokenizer"] != {"type": "Split", "pattern": {"String": " "},
                                      "behavior": "MergedWithPrevious", "invert": False}:
        fail("unexpected pre_tokenizer")
    model = tokenizer["model"]
    expected = {"type": "BPE", "dropout": None, "continuing_subword_prefix": None, "end_of_word_suffix": None,
                "byte_fallback": True, "ignore_merges": False}
    for key, value in expected.items():
        if model.get(key) != value:
            fail("unexpected model.%s" % key)
    vocab = model["vocab"]
    if sorted(vocab.values()) != list(range(len(vocab))):
        fail("vocabulary ids are not contiguous")
    byte_base = vocab["<0x00>"]
    if any(vocab.get("<0x%02X>" % b) != byte_base + b for b in range(256)):
        fail("byte-fallback pieces are not contiguous")
    special = {token["content"] for token in tokenizer["added_tokens"]}
    chars = sorted((ord(piece), index) for piece, index in vocab.items() if len(piece) == 1 and piece not in special)
    merges = []
    for left, right in model["merges"]:
        merged = vocab.get(left + right)
        if merged is None or left not in vocab or right not in vocab:
            fail("merge %r + %r is outside the vocabulary" % (left, right))
        merges.append((vocab[left], vocab[right], merged))
    # HF tokenizers keeps the last rank of a repeated pair; the counter assumes none.
    if len({(left, right) for left, right, _ in merges}) != len(merges):
        fail("the merge table repeats a pair")

    out = bytearray(MAGIC)
    out.append(VERSION)
    varint(len(vocab), out)
    varint(byte_base, out)
    varint(len(chars), out)
    previous = 0
    for codepoint, _ in chars:
        varint(codepoint - previous, out)
        previous = codepoint
    for _, index in chars:
        varint(index, out)
    varint(len(merges), out)
    # Column by column, which compresses about a fifth better than rows.
    for column in (0, 1):
        for merge in merges:
            varint(merge[column], out)
    previous = 0
    for merge in merges:
        delta = merge[2] - previous
        varint((delta << 1) if delta >= 0 else ((-delta << 1) - 1), out)
        previous = merge[2]
    compressed = zstandard.ZstdCompressor(level=19, write_content_size=True, write_checksum=True).compress(bytes(out))
    with open(sys.argv[2], "wb") as handle:
        handle.write(compressed)
    print("vocabulary %d, single characters %d, merges %d, byte base %d" % (len(vocab), len(chars), len(merges), byte_base))
    print("payload %d bytes, compressed %d bytes, sha256 %s" % (len(out), len(compressed), hashlib.sha256(compressed).hexdigest()))


if __name__ == "__main__":
    main()
