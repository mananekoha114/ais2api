#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
"""Official pricing page parsing, including layouts that must not be published."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('scraper', ROOT / 'scripts/scrape-gemini-pricing.py')
scraper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scraper)


def section(name, ids, rows, tier='Standard'):
    links = ' and '.join(f'[`{value}`](https://example.invalid/{value})' for value in ids)
    table = '\n'.join(f'| {label} | Free of charge | {cell} |' for label, cell in rows)
    return (f'## {name}\n\n*{links}*\n\nDescription.\n\n### {tier}\n\n'
            f'|   | Free Tier | Paid Tier, per 1M tokens in USD |\n|---|---|---|\n{table}\n\n### Batch\n\n'
            '| Input price | Not available | $0.01 |\n')


def filler(count):
    return ''.join(section(f'Filler {n}', [f'gemini-filler-{n}'],
                           [('Input price', '$1.00'), ('Output price', '$2.00')]) for n in range(count))


PAGE = ''.join([
    'Intro\n\n### Free\n\n- Free tier\n\n',
    section('Gemini 2.5 Pro', ['gemini-2.5-pro'], [
        ('Input price', '$1.25, prompts \\<= 200k tokens $2.50, prompts \\> 200k tokens'),
        ('Output price (including thinking tokens)', '$10.00, prompts \\<= 200k tokens $15.00, prompts \\> 200k'),
        ('Context caching price', '$0.125, prompts \\<= 200k tokens $0.25, prompts \\> 200k '
                                  '$4.50 / 1,000,000 tokens per hour (storage price)'),
        ('Used to improve our products', 'No')]),
    section('Gemini 2.5 Flash', ['gemini-2.5-flash'], [
        ('Input price', '$0.30 (text / image / video) $1.00 (audio)'),
        ('Output price (including thinking tokens)', '$2.50'),
        ('Context caching price', '$0.03 (text / image / video) $0.1 (audio) $1.00 / 1,000,000 tokens per hour')]),
    section('Gemini 4 Argon', ['gemini-4-argon'], [
        ('Input price', '$2.00 through November 30, 2026. $4.00 starting December 1, 2026.'),
        ('Output price (including thinking tokens)', '$10.00 through November 30, 2026. $20.00 starting December 1, 2026.'),
        ('Context caching price', '$0.10 through November 30, 2026. $0.20 starting December 1, 2026. '
                                  'Implicit caching only (explicit caching is not supported).')]),
    section('Gemini Image 🍌', ['gemini-image', 'gemini-image-alias'], [
        ('Input price', '$2.00 (text/image), equivalent to $0.0011 per image^\\*^'),
        ('Output price', '$12.00 (text and thinking) $120.00 (images) Equivalent to $0.134 per 1K/2K image^\\*\\*^')]),
    section('Gemini Live', ['gemini-live'], [
        ('Input price', '$0.75 (text) $3.00 or $0.005/min (audio)'),
        ('Output price (including thinking tokens)', '$4.50 (text) $12.00 or $0.018/min (audio)')]),
    section('Gemini TTS', ['gemini-tts'], [('Input price', '$1.00 (text)'), ('Output price', '$20.00 (audio)')]),
    section('Gemini Dangling', ['gemini-dangling'], [
        ('Input price', '$1.00 through December 31, 2026.'), ('Output price', '$2.00')]),
    filler(4),
    '## Pricing for tools\n\nNo models here.\n',
])


class ScraperTest(unittest.TestCase):
    def test_text_token_prices_tiers_and_periods(self):
        models, skipped = scraper.parse(PAGE)
        tier = lambda above, i, o, c: {'above_tokens': above, 'input': i, 'output': o, 'cache_read': c}
        self.assertEqual(models['gemini-2.5-pro']['periods'], [
            {'from': None, 'tiers': [tier(0, 1.25, 10.0, 0.125), tier(200000, 2.5, 15.0, 0.25)]}])
        self.assertEqual(models['gemini-2.5-flash']['periods'], [{'from': None, 'tiers': [tier(0, 0.3, 2.5, 0.03)]}])
        self.assertEqual(models['gemini-4-argon']['periods'], [
            {'from': None, 'tiers': [tier(0, 2.0, 10.0, 0.1)]},
            {'from': '2026-12-01', 'tiers': [tier(0, 4.0, 20.0, 0.2)]}])
        self.assertEqual(models['gemini-image']['periods'], [{'from': None, 'tiers': [tier(0, 2.0, 12.0, None)]}])
        self.assertEqual(models['gemini-image-alias'], models['gemini-image'])
        self.assertEqual(models['gemini-image']['name'], 'Gemini Image')
        self.assertEqual(models['gemini-live']['periods'], [{'from': None, 'tiers': [tier(0, 0.75, 4.5, None)]}])
        self.assertIn('no text input and output', skipped['gemini-tts'])
        self.assertIn('no price announced after 2026-12-31', skipped['gemini-dangling'])
        self.assertNotIn('gemini-tts', models)

    def test_unrecognized_layouts_are_not_published(self):
        with self.assertRaises(scraper.PricingError):
            scraper.parse(PAGE.replace('### Standard', '### Paid'))
        with self.assertRaises(scraper.PricingError):
            scraper.parse(PAGE.replace('`gemini-2.5-pro`', '`gemini-2.5-pro-renamed`'))
        with self.assertRaises(scraper.PricingError):
            scraper.parse(PAGE.replace('$2.50, prompts', '$2.75, prompts', 1).replace(
                '$1.25, prompts \\<= 200k tokens', '$1.25, prompts \\<= 200k tokens $1.50, prompts \\<= 200k tokens'))

    def test_unchanged_prices_keep_the_published_file(self):
        with tempfile.TemporaryDirectory() as directory:
            page, output = Path(directory, 'page.md'), Path(directory, 'gemini.json')
            page.write_text(PAGE, encoding='utf-8')
            self.assertEqual(scraper.main(['--input', str(page), '--output', str(output)]), 0)
            first = output.read_text(encoding='utf-8')
            catalog = json.loads(first)
            self.assertEqual(catalog['schema_version'], 1)
            self.assertEqual(catalog['source'], 'https://ai.google.dev/gemini-api/docs/pricing')
            self.assertEqual(scraper.main(['--input', str(page), '--output', str(output), '--keep-unchanged']), 0)
            self.assertEqual(output.read_text(encoding='utf-8'), first)
            page.write_text(PAGE.replace('| $2.50 |', '| $2.60 |'), encoding='utf-8')
            self.assertEqual(scraper.main(['--input', str(page), '--output', str(output), '--keep-unchanged']), 0)
            self.assertEqual(json.loads(output.read_text(encoding='utf-8'))['models']['gemini-2.5-flash']
                             ['periods'][0]['tiers'][0]['output'], 2.6)
            page.write_text('# Moved\n', encoding='utf-8')
            self.assertEqual(scraper.main(['--input', str(page), '--output', str(output)]), 1)


if __name__ == '__main__':
    unittest.main()
