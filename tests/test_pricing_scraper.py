#!/usr/bin/env python3
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
"""Official pricing page parsing, including layouts that must not be published."""
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('scraper', ROOT / 'scripts/scrape-gemini-pricing.py')
scraper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scraper)
# The fixtures' long-listed models, whichever ones the live page needs.
scraper.REQUIRED_MODELS = ('gemini-2.5-pro', 'gemini-2.5-flash')


def section(name, ids, rows, tier='Standard'):
    links = ' and '.join(f'[`{value}`](https://example.invalid/{value})' for value in ids)
    table = '\n'.join(f'| {label} | Free of charge | {cell} |' for label, cell in rows)
    return (f'## {name}\n\n*{links}*\n\nDescription.\n\n### {tier}\n\n'
            f'|   | Free Tier | Paid Tier, per 1M tokens in USD |\n|---|---|---|\n{table}\n\n### Batch\n\n'
            '| Input price | Not available | $0.01 |\n')


def markdoc_cell(cell):
    """A cell of the earlier layout as the Markdoc page writes it: unescaped,
    footnotes as {% sup %} tags, each price on its own line."""
    cell = cell.replace('\\<', '<').replace('\\>', '>')
    cell = cell.replace('^\\*\\*^', '{% sup %}**{% /sup %}').replace('^\\*^', '{% sup %}*{% /sup %}')
    return '    \n  '.join(part.strip() for part in re.split(r'(?=\$[0-9])', cell) if part.strip())


def markdoc_table(rows):
    lines = ['{% table %}', '  * Feature', '  * Free Tier', '  * Paid Tier, per 1M tokens in USD']
    for label, cell in rows:
        lines += ['', '  ---', '', f'  * {label}', '  * Free of charge', f'  * {markdoc_cell(cell)}']
    return '\n'.join(lines + ['  {% /table %}'])


def markdoc_section(name, ids, rows, tier='Standard'):
    """The November 2026 layout: ids in a two-column header, the price tiers
    in tabs; tier=None gives the single table a Live model has."""
    links = ',\n    '.join(f'[`{value}`](/docs/models/{value})' for value in ids)
    header = (f'## {name}  {{% id="{ids[0]}" %}}\n\n{{% two-column-layout %}}\n'
              f'  {{% left-column class="center-vertically" %}}\n  *{links}*\n\n  {{% /left-column %}}\n'
              '\n{% /two-column-layout %}\n\n{% hr /%}\n\nDescription.\n\n')
    if tier is None:
        return header + markdoc_table(rows) + '\n\n'
    batch = markdoc_table([('Input price', '$0.01'), ('Output price', '$0.02')])
    return (header + f'{{% tabs %}}\n  {{% tab title="{tier}" %}}\n  {markdoc_table(rows)}\n\n  {{% /tab %}}\n\n'
            f'  {{% tab title="Batch" %}}\n  {batch}\n\n  {{% /tab %}}\n{{% /tabs %}}\n\n')


def filler(count, layout=section):
    return ''.join(layout(f'Filler {n}', [f'gemini-filler-{n}'],
                          [('Input price', '$1.00'), ('Output price', '$2.00')]) for n in range(count))


SECTIONS = [
    ('Gemini 2.5 Pro', ['gemini-2.5-pro'], [
        ('Input price', '$1.25, prompts \\<= 200k tokens $2.50, prompts \\> 200k tokens'),
        ('Output price (including thinking tokens)', '$10.00, prompts \\<= 200k tokens $15.00, prompts \\> 200k'),
        ('Context caching price', '$0.125, prompts \\<= 200k tokens $0.25, prompts \\> 200k '
                                  '$4.50 / 1,000,000 tokens per hour (storage price)'),
        ('Used to improve our products', 'No')]),
    ('Gemini 2.5 Flash', ['gemini-2.5-flash'], [
        ('Input price', '$0.30 (text / image / video) $1.00 (audio)'),
        ('Output price (including thinking tokens)', '$2.50'),
        ('Context caching price', '$0.03 (text / image / video) $0.1 (audio) $1.00 / 1,000,000 tokens per hour')]),
    ('Gemini 4 Argon', ['gemini-4-argon'], [
        ('Input price', '$2.00 through November 30, 2026. $4.00 starting December 1, 2026.'),
        ('Output price (including thinking tokens)', '$10.00 through November 30, 2026. $20.00 starting December 1, 2026.'),
        ('Context caching price', '$0.10 through November 30, 2026. $0.20 starting December 1, 2026. '
                                  'Implicit caching only (explicit caching is not supported).')]),
    ('Gemini Image 🍌', ['gemini-image', 'gemini-image-alias'], [
        ('Input price', '$2.00 (text/image), equivalent to $0.0011 per image^\\*^'),
        ('Output price', '$12.00 (text and thinking) $120.00 (images) Equivalent to $0.134 per 1K/2K image^\\*\\*^')]),
    ('Gemini Live', ['gemini-live'], [
        ('Input price', '$0.75 (text) $3.00 or $0.005/min (audio)'),
        ('Output price (including thinking tokens)', '$4.50 (text) $12.00 or $0.018/min (audio)')]),
    ('Gemini TTS', ['gemini-tts'], [('Input price', '$1.00 (text)'), ('Output price', '$20.00 (audio)')]),
    ('Gemini Dangling', ['gemini-dangling'], [
        ('Input price', '$1.00 through December 31, 2026.'), ('Output price', '$2.00')]),
]
TOOLS = '## Pricing for tools\n\nNo models here.\n'
PAGE = ''.join(['Intro\n\n### Free\n\n- Free tier\n\n', *(section(*item) for item in SECTIONS), filler(4), TOOLS])
# Live models have a single table, without tabs.
PAGE_MARKDOC = ''.join([
    '# Gemini Developer API pricing\n\n{% card-grid %}\n  {% card title="Free" %}\n  Free tier\n  {% /card %}\n'
    '{% /card-grid %}\n\n',
    *(markdoc_section(*item, tier=None if item[0] == 'Gemini Live' else 'Standard') for item in SECTIONS),
    filler(4, markdoc_section), TOOLS])


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

    def test_markdoc_layout_gives_the_same_catalog(self):
        self.assertIn('{% sup %}*{% /sup %}', PAGE_MARKDOC)
        self.assertIn('prompts <= 200k tokens    \n  $2.50', PAGE_MARKDOC)
        self.assertEqual(scraper.parse(PAGE_MARKDOC), scraper.parse(PAGE))

    def test_markdoc_layouts_that_must_not_be_published(self):
        # Batch prices alone are not the Standard tier's.
        with self.assertRaises(scraper.PricingError):
            scraper.parse(PAGE_MARKDOC.replace('tab title="Standard"', 'tab title="Paid"'))
        doubled = PAGE_MARKDOC.replace('{% /table %}\n\n  {% /tab %}', '{% /table %}\n  {% table %}\n  * Input price\n'
                                       '  * Free of charge\n  * $9.00\n  {% /table %}\n\n  {% /tab %}', 1)
        with self.assertRaises(scraper.PricingError):
            scraper.parse(doubled)
        _, skipped = scraper.parse(doubled.replace('`gemini-2.5-pro`', '`gemini-2.5-pro-twice`') + markdoc_section(
            'Gemini 2.5 Pro', ['gemini-2.5-pro'], [('Input price', '$1.00'), ('Output price', '$2.00')]))
        self.assertEqual(skipped['gemini-2.5-pro-twice'], 'more than one Standard price table')

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
            self.assertEqual(catalog['source'], 'https://aistudio.google.com/docs/pricing')
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
