#!/usr/bin/env python3
"""Derive a geography sample from the local Wikipedia, independently of QA datasets."""
import argparse
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]


class States(HTMLParser):
    def __init__(self):
        super().__init__()
        self.depth = 0
        self.cell = 0
        self.in_cell = False
        self.links = []
        self.found = False

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'table':
            if self.depth or {'sortable', 'wikitable'} <= set(attrs.get('class', '').split()):
                self.depth += 1
        if self.depth != 1:
            return
        if tag == 'tr':
            self.cell = 0
            self.found = False
        elif tag == 'td':
            self.cell += 1
            self.in_cell = True
        elif tag == 'a' and self.in_cell and self.cell == 1 and not self.found:
            href = attrs.get('href', '')
            if href and not href.startswith(('#', 'http', './_')):
                self.links.append(unquote(href.removeprefix('./')))
                self.found = True

    def handle_endtag(self, tag):
        if tag == 'table' and self.depth:
            self.depth -= 1
        if tag == 'td':
            self.in_cell = False


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('zim', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    def article(title):
        return subprocess.check_output([str(ROOT/'host-tools/zim-reader/zimdump'),
                                        str(args.zim), 'blob', 'C', title])
    listing = article('List_of_sovereign_states')
    parser = States()
    parser.feed(listing.decode())
    titles = sorted(set(parser.links))
    if len(titles) < 190:
        raise RuntimeError(f'Unexpected state-list structure: only {len(titles)} articles')
    entities, missing = {}, []
    for i, title in enumerate(titles, 1):
        page = article(title)
        ids = set(re.findall(rb'www\.wikidata\.org/wiki/(Q[1-9][0-9]*)#identifiers', page))
        if len(ids) == 1:
            entities[title] = ids.pop().decode()
        else:
            missing.append(title)
        if i % 25 == 0:
            print(f'Read {i}/{len(titles)} local country articles', flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'local_zim': str(args.zim),
        'list_article': 'List_of_sovereign_states',
        'list_html_sha256': hashlib.sha256(listing).hexdigest(),
        'selection': 'first country link in each state-list table row; local authority-control Wikidata ID',
        'seeds': sorted(set(entities.values())), 'articles': entities,
        'missing_authority_id': missing}, indent=2)+'\n')
    print(f'{len(entities)} country articles resolved; {len(missing)} lack a unique authority ID')


if __name__ == '__main__':
    main()
