#!/usr/bin/env python3
"""Extract plain training text from a ZIM archive.

The text a model is trained on should be the text the device would show,
so the markup this drops is the markup `zim/zim_html.c` drops:
`class_word_skipped` there lists the classes whose content is navigation,
editing chrome or hidden, and the same list is used here. Training on
navboxes and category footers would spend a very small model's capacity on
furniture it will never be asked to produce.

Tables, references and the metadata boxes go too. They are dense with
markup and numbers, they read badly as running prose, and a model at this
scale has nothing to spare.
"""

import argparse
import re
import sys
import unicodedata
from html.parser import HTMLParser
from pathlib import Path

# Straight from zim/zim_html.c, class_word_skipped().
SKIPPED_CLASSES = {
    "navbox", "navbox-styles", "noprint", "vertical-navbox",
    "mw-editsection", "mw-jump-link", "mw-hidden-catlinks", "mw-indicators",
    "mw-empty-elt", "printfooter", "catlinks", "sistersitebox",
}

# Not in the device's list, but not prose either: infoboxes and reference
# lists are mostly markup and bare numbers, and tables do not linearize.
SKIPPED_CLASSES |= {
    "infobox", "reflist", "reference", "references", "thumb", "thumbcaption",
    "gallery", "hatnote", "ambox", "metadata", "mbox-small", "toc",
    "sidebar", "succession-box", "authority-control", "portal",
}

SKIPPED_TAGS = {"script", "style", "table", "sup", "head", "figure", "math"}

# HTML void elements never get an end tag. Counting them into the skip
# depth means the depth only ever climbs: <head> opens it, the <meta> and
# <link> inside push it higher, </head> takes off one, and everything after
# that is silently discarded -- which is an empty article for every input
# and no error anywhere.
VOID_TAGS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr",
}

# Date and year pages ("June 22", "1932", "1930s") are birth-and-death
# lists, not prose: 5% of Simple English by volume and almost entirely the
# same sentence with the numbers changed. A model of a few million
# parameters will learn that form very well and it is not worth the
# capacity.
DATE_TITLE = re.compile(
    r"^(January|February|March|April|May|June|July|August|September|"
    r"October|November|December)\s+\d{1,2}$|^\d{1,4}(s|\s+BC)?$"
)
LIST_LINE = re.compile(r"^\d{3,4}\s*[-\u2013\u2014]")


def mostly_list(text):
    lines = [l for l in text.split("\n") if l.strip()]
    if not lines:
        return True
    hits = sum(1 for l in lines if LIST_LINE.match(l.strip()))
    return hits > len(lines) * 0.5


# Sections that are lists of pointers rather than writing.
STOP_SECTIONS = {
    "references", "related pages", "other websites", "sources", "notes",
    "further reading", "external links", "bibliography", "see also",
    "footnotes", "citations",
}


class ArticleText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []
        self.depth_skip = 0      # inside a skipped element
        self.stack = []
        self.in_heading = None
        self.heading = []
        self.stopped = False

    def _skipped(self, tag, attrs):
        if tag in SKIPPED_TAGS:
            return True
        for name, value in attrs:
            if name == "class" and value:
                if SKIPPED_CLASSES & set(value.lower().split()):
                    return True
            if name == "role" and value in ("navigation", "note"):
                return True
        return False

    def handle_starttag(self, tag, attrs):
        if tag in VOID_TAGS:
            if not self.depth_skip and tag == "br":
                self.out.append("\n")
            return
        if self.depth_skip:
            self.depth_skip += 1
            return
        if self._skipped(tag, attrs):
            self.depth_skip = 1
            return
        self.stack.append(tag)
        # h1 is the article title, which is written separately; emitting it
        # here too taught the model that every article opens by saying its
        # own name twice.
        if tag in ("h1", "h2", "h3", "h4"):
            self.in_heading = tag
            self.heading = []
        elif tag in ("p", "li", "dd", "div"):
            self.out.append("\n")

    def handle_endtag(self, tag):
        if tag in VOID_TAGS:
            return
        if self.depth_skip:
            self.depth_skip -= 1
            return
        if self.in_heading == tag:
            title = "".join(self.heading).strip()
            self.in_heading = None
            # A pointer section ends the article: everything after it is
            # links and citations.
            if title.lower().rstrip(" :") in STOP_SECTIONS:
                self.stopped = True
            elif tag != "h1":
                self.out.append("\n\n" + title + "\n\n")
        if self.stack and tag in self.stack:
            while self.stack and self.stack.pop() != tag:
                pass

    def handle_data(self, data):
        if self.depth_skip or self.stopped:
            return
        if self.in_heading:
            self.heading.append(data)
        else:
            self.out.append(data)

    def text(self):
        return "".join(self.out)


# The panel's font is 8x13 with 256 entries, so the device cannot draw a
# foreign script whatever the model does with it -- and the corpus has 7695
# distinct characters, almost all of them from one-off glosses like
# "(Arabic: ...)" beside a name. Folding to ASCII costs 0.33% of the text
# and takes the vocabulary's required character set from 7946 to under a
# hundred, which is what makes a 4096-entry vocabulary possible at all.
PUNCTUATION = {
    "\u2013": "-", "\u2014": "-", "\u2212": "-", "\u2018": "'",
    "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u2026": "...",
    "\u00a0": " ", "\u00ab": '"', "\u00bb": '"', "\u2032": "'",
    "\u00d7": "x", "\u2044": "/",
}
EMPTY_GLOSS = re.compile(r"\s*\([^()]*:\s*\)")


def to_ascii(text):
    text = "".join(PUNCTUATION.get(c, c) for c in text)
    # Decompose, then drop the combining marks: this turns e-acute into e
    # rather than deleting the letter.
    text = unicodedata.normalize("NFKD", text)
    text = "".join(c for c in text if not unicodedata.combining(c))
    text = "".join(c if ord(c) < 128 else "" for c in text)
    # "(Arabic: )" once its contents are gone.
    return EMPTY_GLOSS.sub("", text)


# Editing leftovers and bracketed citation marks that survive the tags.
CITATION = re.compile(r"\[\s*(?:\d+|citation needed|needs? source|\?)\s*\]",
                      re.I)
SPACES = re.compile(r"[ \t ]+")
BLANKS = re.compile(r"\n\s*\n\s*")


def clean(text):
    text = to_ascii(text)
    text = CITATION.sub("", text)
    text = SPACES.sub(" ", text)
    text = BLANKS.sub("\n\n", text)
    lines = []
    for line in text.split("\n"):
        line = line.strip()
        # A line that is mostly punctuation or digits is a table remnant.
        if line and sum(c.isalpha() for c in line) < len(line) * 0.5:
            if len(line) > 3:
                continue
        lines.append(line)
    return "\n".join(lines).strip()


def article_text(html):
    parser = ArticleText()
    try:
        parser.feed(html)
    except Exception:
        return ""
    return clean(parser.text())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("zim", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after this many articles (0 = all)")
    ap.add_argument("--min-chars", type=int, default=200,
                    help="skip stubs shorter than this")
    args = ap.parse_args()

    from libzim.reader import Archive

    archive = Archive(str(args.zim))
    total = archive.entry_count
    kept = chars = 0

    with args.out.open("w", encoding="utf-8") as f:
        for i in range(total):
            entry = archive._get_entry_by_id(i)
            if entry.is_redirect:
                continue
            item = entry.get_item()
            if not str(item.mimetype).startswith("text/html"):
                continue
            # Namespaced pages -- templates, categories, the main page --
            # are not article prose.
            if ":" in entry.path and not entry.path.startswith("A/"):
                continue

            if DATE_TITLE.match(entry.title.strip()):
                continue

            body = article_text(bytes(item.content).decode("utf-8", "replace"))
            if len(body) < args.min_chars:
                continue
            if mostly_list(body):
                continue

            # One document per record, titled, blank-line separated: the
            # title is part of what an encyclopedia article looks like.
            f.write(entry.title + "\n\n" + body + "\n\n\n")
            kept += 1
            chars += len(body)
            if kept % 5000 == 0:
                print(f"  {kept} articles, {chars/1e6:.1f}M chars",
                      file=sys.stderr, flush=True)
            if args.limit and kept >= args.limit:
                break

    print(f"{args.out}: {kept} articles, {chars/1e6:.1f}M characters")


if __name__ == "__main__":
    main()
