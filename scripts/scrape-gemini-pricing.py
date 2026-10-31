#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Scrape Gemini API token prices from Google's official pricing page.

The result is the catalog ais2api downloads from this repository's
pricing-data branch (and embeds from pricing/gemini.json at build time).
Only the Standard paid tier's text token prices are kept: input, output
(thinking included) and context cache reads, with prompt-size tiers and
announced price changes. Storage, per-image, per-minute and audio/image-only
prices are not token prices for text and are left out.
"""
import argparse
import datetime
import json
import re
import sys
import urllib.request

SOURCE = "https://ai.google.dev/gemini-api/docs/pricing"
MARKDOWN = SOURCE + ".md.txt"
SCHEMA_VERSION = 1
# Long-listed models; a scrape without them means the page layout changed and
# must not replace a published catalog.
REQUIRED_MODELS = ("gemini-2.5-pro", "gemini-2.5-flash")
MINIMUM_MODELS = 8
MAXIMUM_BYTES = 4 * 1024 * 1024

MODEL_ID = re.compile(r"`([a-z0-9][a-z0-9.\-]*)`")
PRICE = re.compile(r"\$\s*([0-9]+(?:\.[0-9]+)?)")
# "$3.00 or $0.005/min (audio)": the per-minute alternative is not a token price.
PER_MINUTE = re.compile(r"\s+or\s+\$[0-9.]+\s*/\s*min\S*")
MODALITY = re.compile(r"\(((?:\s*(?:text|images?|video|audio|thinking|and|/|,)\s*)+)\)")
TIER = re.compile(r"prompts\s*\\?(<=|>)\s*([0-9]+)\s*k", re.I)
DATE = re.compile(r"\b(through|starting)\s+([A-Z][a-z]+\s+[0-9]{1,2},\s+[0-9]{4})")
FIELDS = (("input price", "input"), ("output price", "output"), ("context caching price", "cache_read"))


class PricingError(Exception):
    pass


def fetch(url=MARKDOWN):
    request = urllib.request.Request(url, headers={"User-Agent": "ais2api-pricing-scraper"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(MAXIMUM_BYTES + 1)
    if len(data) > MAXIMUM_BYTES:
        raise PricingError("pricing page exceeds 4 MiB")
    return data.decode("utf-8")


def parse_date(text):
    return datetime.datetime.strptime(" ".join(text.replace(",", " ").split()), "%B %d %Y").date()


def clauses(cell):
    """Yield (price, text) for each token price in a paid-tier cell."""
    cell = PER_MINUTE.sub("", cell)
    matches = list(PRICE.finditer(cell))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(cell)
        text = cell[match.end():end]
        # Storage ("/ 1,000,000 tokens per hour") and per-unit equivalents
        # ("per 1K image", "per 10s audio") are other dimensions.
        if re.match(r"\s*(/|per\b)", text) or "per hour" in text:
            continue
        yield match.group(1), text


def parse_cell(cell):
    """Return entries (start, until, above_tokens, price) for text tokens."""
    entries = []
    for price, text in clauses(cell):
        modality = MODALITY.search(text)
        if modality and "text" not in modality.group(1):
            continue
        tier = TIER.search(text)
        above = 0
        if tier and tier.group(1) == ">":
            above = int(tier.group(2)) * 1000
        start = until = None
        date = DATE.search(text)
        if date:
            if date.group(1) == "starting":
                start = parse_date(date.group(2))
            else:
                until = parse_date(date.group(2))
        value = float(price)
        if not 0 <= value <= 1000:
            raise PricingError(f"implausible price ${price}")
        entries.append((start, until, above, value))
    return entries


def standard_table(section):
    match = re.search(r"^### Standard\s*\n(.*?)(?=^#{2,3} |\Z)", section, re.M | re.S)
    if not match:
        return None
    rows = {}
    for line in match.group(1).splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 2:
            continue
        label = cells[0].lower()
        for prefix, field in FIELDS:
            if label.startswith(prefix):
                if field in rows:
                    raise PricingError(f"duplicate {field} row")
                rows[field] = cells[-1]
    return rows


def resolve(entries, period, above):
    """The price in effect for a period start and prompt size, if any."""
    best = None
    for start, until, threshold, value in entries:
        if threshold > above:
            continue
        if start is not None and (period is None or start > period):
            continue
        if until is not None and period is not None and period > until:
            continue
        key = (start or datetime.date.min, threshold)
        if best is not None and key == best[0] and value != best[1]:
            raise PricingError("conflicting prices for the same period and tier")
        if best is None or key > best[0]:
            best = (key, value)
    return None if best is None else best[1]


def model_prices(rows):
    fields = {field: parse_cell(rows[field]) for field in ("input", "output", "cache_read") if field in rows}
    if not fields.get("input") or not fields.get("output"):
        raise PricingError("no text input and output token prices")
    entries = [entry for values in fields.values() for entry in values]
    starts = sorted({start for start, _, _, _ in entries if start is not None})
    for _, until, _, _ in entries:
        if until is not None and until + datetime.timedelta(days=1) not in starts:
            raise PricingError(f"no price announced after {until.isoformat()}")
    thresholds = sorted({above for _, _, above, _ in entries})
    periods = []
    for period in [None, *starts]:
        tiers = []
        for above in thresholds:
            tier = {"above_tokens": above}
            for field in ("input", "output", "cache_read"):
                value = resolve(fields.get(field, []), period, above)
                if value is None and field != "cache_read":
                    raise PricingError(f"no {field} price for every period and tier")
                tier[field] = value
            tiers.append(tier)
        periods.append({"from": period.isoformat() if period else None, "tiers": tiers})
    return periods


def parse(markdown):
    models, skipped = {}, {}
    for section in re.split(r"^## ", markdown, flags=re.M)[1:]:
        heading, _, body = section.partition("\n")
        # The model ids are on the line(s) right below the heading.
        ids = [value for value in MODEL_ID.findall("\n".join(body.splitlines()[:4]))
               if value.startswith(("gemini-", "gemma-"))]
        if not ids:
            continue
        name = re.sub(r"[^\x20-\x7e]", "", re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)).strip()
        try:
            rows = standard_table(body)
            if rows is None:
                raise PricingError("no Standard price table")
            prices = model_prices(rows)
        except PricingError as error:
            for value in ids:
                skipped[value] = str(error)
            continue
        for value in ids:
            if value in models:
                raise PricingError(f"{value} is listed twice")
            models[value] = {"name": name, "periods": prices}
    missing = [value for value in REQUIRED_MODELS if value not in models]
    if missing or len(models) < MINIMUM_MODELS:
        raise PricingError(f"pricing page layout not recognized (models: {len(models)}, missing: {missing})")
    return models, skipped


def catalog(models, published_at):
    return {
        "schema_version": SCHEMA_VERSION,
        "source": SOURCE,
        "published_at": published_at,
        "currency": "USD",
        "unit": "USD per 1M tokens",
        "service_tier": "standard",
        "models": dict(sorted(models.items())),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", help="parse this saved Markdown page instead of downloading it")
    parser.add_argument("--output", required=True, help="catalog JSON to write")
    parser.add_argument("--keep-unchanged", action="store_true",
                        help="leave the output untouched when its prices are unchanged")
    args = parser.parse_args(argv)
    try:
        if args.input:
            with open(args.input, encoding="utf-8") as stream:
                markdown = stream.read()
        else:
            markdown = fetch()
        models, skipped = parse(markdown)
    except (OSError, PricingError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for value, reason in sorted(skipped.items()):
        print(f"skipped {value}: {reason}", file=sys.stderr)
    if args.keep_unchanged:
        try:
            with open(args.output, encoding="utf-8") as stream:
                previous = json.load(stream)
            if previous.get("schema_version") == SCHEMA_VERSION and previous.get("models") == json.loads(
                    json.dumps(catalog(models, "")["models"])):
                print(f"unchanged: {len(models)} models")
                return 0
        except (OSError, ValueError):
            pass
    published_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with open(args.output, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(catalog(models, published_at), stream, indent=2, ensure_ascii=False)
        stream.write("\n")
    print(f"wrote {len(models)} models to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
