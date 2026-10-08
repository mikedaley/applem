#!/usr/bin/env python3
"""make_help.py - Build ApplEm's Help Book from the user guide

Turns docs/guide's Markdown into the HTML pages of a macOS Help Book
(ApplEm.help), copies its pictures and stylesheet, writes the book's
Info.plist and indexes it with hiutil, so Help > ApplEm Help and the Help
menu's search find the guide inside the app.

The converter covers the Markdown the guide is written in, and no more:
headings, paragraphs, lists (nested, with code blocks inside), fenced code,
tables, block quotes, pictures, links and inline emphasis and code. A link
to another page of the guide becomes a link to that page here; a link out
of the guide, to the examples or the README, goes to the repository on
GitHub, which a Help Book cannot hold.

Usage: make_help.py <guide directory> <help.css> <output ApplEm.help> <version>

Written by
 Mike Daley <michael_daley@icloud.com>
"""

import html
import os
import re
import shutil
import subprocess
import sys

BOOK_ID = "uk.co.retrotech71.applem.native.help"
BOOK_TITLE = "ApplEm Help"
REPOSITORY = "https://github.com/mikedaley/applem/blob/main/"


# ---------------------------------------------------------------------------
# Inline text
# ---------------------------------------------------------------------------

def slug(text):
    """The anchor GitHub gives a heading, so links written for GitHub work."""
    text = re.sub(r"<[^>]+>", "", text).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def rewrite_link(target, page_names):
    """A guide page's link, for the Help Book."""
    if re.match(r"^[a-z]+:", target):
        return target
    path, _, anchor = target.partition("#")
    if not path:
        return "#" + anchor
    name = os.path.basename(path)
    if name in page_names and not path.startswith(".."):
        out = page_names[name]
        return out + ("#" + anchor if anchor else "")
    # Outside the guide: the repository's copy.
    clean = os.path.normpath(os.path.join("docs/guide", path))
    return REPOSITORY + clean + ("#" + anchor if anchor else "")


def emphasis(text):
    """**strong** and *em*, nested either way round, from runs of asterisks."""
    out = []
    stack = []  # (kind, index in out)
    pieces = re.split(r"(\*+)", text)
    for i, piece in enumerate(pieces):
        if not piece.startswith("*") or piece == "":
            out.append(piece)
            continue
        before = pieces[i - 1][-1:] if i > 0 else ""
        after = pieces[i + 1][:1] if i + 1 < len(pieces) else ""
        can_open = after != "" and not after.isspace()
        can_close = before != "" and not before.isspace()
        n = len(piece)
        while n > 0:
            if can_close and stack:
                kind, index = stack[-1]
                need = 2 if kind == "strong" else 1
                if n >= need:
                    stack.pop()
                    out[index] = "<%s>" % kind
                    out.append("</%s>" % kind)
                    n -= need
                    continue
            if can_open:
                kind = "strong" if n >= 2 else "em"
                stack.append((kind, len(out)))
                out.append("*" * (2 if kind == "strong" else 1))
                n -= 2 if kind == "strong" else 1
                continue
            out.append("*" * n)
            n = 0
    return "".join(out)


def inline(text, page_names):
    """One line or paragraph of inline Markdown, as HTML."""
    saved = []

    def keep(fragment):
        saved.append(fragment)
        return "\x00%d\x00" % (len(saved) - 1)

    # Code spans first, so nothing inside them is touched.
    text = re.sub(r"`([^`]+)`", lambda m: keep("<code>%s</code>" % html.escape(m.group(1), quote=False)), text)
    # Backslash escapes.
    text = re.sub(r"\\([\\`*_{}\[\]()#+\-.!|<>])", lambda m: keep(html.escape(m.group(1))), text)
    text = html.escape(text, quote=False)

    def image(m):
        alt, src = m.group(1), m.group(2)
        return keep('<img src="%s" alt="%s">' % (html.escape(src), html.escape(alt)))

    text = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)", image, text)

    def link(m):
        label, target = m.group(1), html.unescape(m.group(2))
        href = rewrite_link(target, page_names)
        return keep('<a href="%s">' % html.escape(href)) + label + keep("</a>")

    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, text)
    text = emphasis(text)
    while "\x00" in text:
        text = re.sub(r"\x00(\d+)\x00", lambda m: saved[int(m.group(1))], text)
    return text


# ---------------------------------------------------------------------------
# Blocks
# ---------------------------------------------------------------------------

LIST_ITEM = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")
FENCE = re.compile(r"^(\s*)```(\w*)\s*$")


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def dedent(lines, amount):
    return [l[amount:] if l[:amount].strip() == "" else l.lstrip(" ") for l in lines]


def split_row(line):
    cells, cell, i = [], "", 0
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    while i < len(line):
        if line[i] == "\\" and i + 1 < len(line) and line[i + 1] == "|":
            cell += "\\|"
            i += 2
            continue
        if line[i] == "|":
            cells.append(cell.strip())
            cell = ""
        else:
            cell += line[i]
        i += 1
    cells.append(cell.strip())
    return cells


def starts_block(line):
    s = line.strip()
    return (s.startswith("#") or s.startswith("```") or s.startswith("|") or s.startswith(">")
            or LIST_ITEM.match(line) is not None)


def blocks(lines, page_names, headings):
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        fence = FENCE.match(line)
        if fence:
            depth = len(fence.group(1))
            language = fence.group(2)
            body = []
            i += 1
            while i < len(lines) and not re.match(r"^\s*```\s*$", lines[i]):
                body.append(lines[i][depth:] if lines[i][:depth].strip() == "" else lines[i].lstrip())
                i += 1
            i += 1
            cls = ' class="language-%s"' % language if language else ""
            out.append("<pre><code%s>%s</code></pre>" % (cls, html.escape("\n".join(body), quote=False)))
            continue
        heading = re.match(r"^(#{1,6})\s+(.*)$", line)
        if heading:
            level = len(heading.group(1))
            content = inline(heading.group(2), page_names)
            anchor = slug(content)
            headings.append((level, re.sub(r"<[^>]+>", "", content), anchor))
            out.append('<h%d id="%s"><a name="%s"></a>%s</h%d>' % (level, anchor, anchor, content, level))
            i += 1
            continue
        if line.strip().startswith("|") and i + 1 < len(lines) and re.match(r"^\s*\|?[\s:|-]+\|?\s*$", lines[i + 1]):
            head = split_row(line)
            i += 2
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(split_row(lines[i]))
                i += 1
            cell = lambda c: inline(c.replace("\\|", "|"), page_names)
            table = ["<table>", "<thead><tr>%s</tr></thead>" % "".join("<th>%s</th>" % cell(c) for c in head),
                     "<tbody>"]
            for row in rows:
                table.append("<tr>%s</tr>" % "".join("<td>%s</td>" % cell(c) for c in row))
            table.append("</tbody></table>")
            out.append("".join(table))
            continue
        if line.strip().startswith(">"):
            quoted = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                quoted.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            out.append("<blockquote>%s</blockquote>" % blocks(quoted, page_names, headings))
            continue
        item = LIST_ITEM.match(line)
        if item:
            depth = len(item.group(1))
            ordered = item.group(2)[0].isdigit()
            items = []
            while i < len(lines):
                m = LIST_ITEM.match(lines[i])
                if not m or len(m.group(1)) != depth or m.group(2)[0].isdigit() != ordered:
                    break
                content_indent = len(lines[i]) - len(m.group(3))
                body = [m.group(3)]
                i += 1
                while i < len(lines):
                    nxt = lines[i]
                    if not nxt.strip():
                        # A blank line stays in the item if what follows is
                        # indented under it.
                        j = i
                        while j < len(lines) and not lines[j].strip():
                            j += 1
                        if j < len(lines) and indent_of(lines[j]) >= content_indent:
                            body.extend([""] * (j - i))
                            i = j
                            continue
                        break
                    if indent_of(nxt) >= content_indent:
                        body.append(nxt[content_indent:])
                        i += 1
                        continue
                    if LIST_ITEM.match(nxt) or starts_block(nxt):
                        break
                    body.append(nxt.strip())  # a lazy continuation line
                    i += 1
                rendered = blocks(body, page_names, headings)
                # A tight item: one paragraph, shown without its <p>.
                single = re.fullmatch(r"<p>(.*?)</p>(.*)", rendered, re.S)
                if single and "<p>" not in single.group(2):
                    rendered = single.group(1) + single.group(2)
                items.append("<li>%s</li>" % rendered)
            tag = "ol" if ordered else "ul"
            out.append("<%s>%s</%s>" % (tag, "".join(items), tag))
            continue
        paragraph = []
        while i < len(lines) and lines[i].strip() and not (paragraph and starts_block(lines[i])):
            paragraph.append(lines[i].strip())
            i += 1
        text = inline(" ".join(paragraph), page_names)
        if re.fullmatch(r'<img [^>]+>', text):
            alt = re.search(r'alt="([^"]*)"', text).group(1)
            out.append('<figure>%s<figcaption>%s</figcaption></figure>' % (text, alt))
        else:
            out.append("<p>%s</p>" % text)
    return "\n".join(out)


# ---------------------------------------------------------------------------
# The book
# ---------------------------------------------------------------------------

PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
{apple_title}<meta name="description" content="{description}">
<title>{title}</title>
<link rel="stylesheet" href="help.css">
</head>
<body>
<nav class="top"><a href="index.html">ApplEm Help</a>{nav}</nav>
<main>
{body}
</main>
<nav class="bottom">{nav}</nav>
</body>
</html>
"""


def main():
    guide, css, book, version = sys.argv[1:5]
    names = sorted(n for n in os.listdir(guide) if n.endswith(".md"))
    pages = ["README.md"] + [n for n in names if n != "README.md"]
    page_names = {n: ("index.html" if n == "README.md" else n[:-3] + ".html") for n in pages}

    if os.path.exists(book):
        shutil.rmtree(book)
    resources = os.path.join(book, "Contents", "Resources")
    lproj = os.path.join(resources, "en.lproj")
    os.makedirs(lproj)
    shutil.copy(css, os.path.join(lproj, "help.css"))
    images = os.path.join(guide, "images")
    if os.path.isdir(images):
        shutil.copytree(images, os.path.join(lproj, "images"))

    for index, name in enumerate(pages):
        source = open(os.path.join(guide, name), encoding="utf-8").read().splitlines()
        headings = []
        body = blocks(source, page_names, headings)
        title = headings[0][1] if headings else name
        first = re.search(r"<p>(.*?)</p>", body, re.S)
        description = html.escape(re.sub(r"<[^>]+>", "", first.group(1)) if first else title)[:300]
        links = []
        if index > 0:
            links.append('<a class="prev" href="%s">&larr; Previous</a>' % page_names[pages[index - 1]])
        if index + 1 < len(pages):
            links.append('<a class="next" href="%s">Next &rarr;</a>' % page_names[pages[index + 1]])
        apple_title = '<meta name="AppleTitle" content="%s">\n' % BOOK_TITLE if name == "README.md" else ""
        page = PAGE.format(apple_title=apple_title, description=description, title=html.escape(title),
                           nav="".join(links), body=body)
        open(os.path.join(lproj, page_names[name]), "w", encoding="utf-8").write(page)

    plist = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleIdentifier</key>
	<string>{id}</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>ApplEm</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>{version}</string>
	<key>CFBundleSignature</key>
	<string>hbwr</string>
	<key>CFBundleVersion</key>
	<string>{version}</string>
	<key>HPDBookAccessPath</key>
	<string>index.html</string>
	<key>HPDBookCSIndexPath</key>
	<string>ApplEm.cshelpindex</string>
	<key>HPDBookIndexPath</key>
	<string>ApplEm.helpindex</string>
	<key>HPDBookTitle</key>
	<string>{title}</string>
	<key>HPDBookType</key>
	<string>3</string>
</dict>
</plist>
""".format(id=BOOK_ID, version=html.escape(version), title=BOOK_TITLE)
    open(os.path.join(book, "Contents", "Info.plist"), "w").write(plist)

    # The search indexes: Spotlight's, which the Help menu's search uses, and
    # the older one for Help Viewer's own.
    hiutil = shutil.which("hiutil") or "/usr/bin/hiutil"
    if os.path.exists(hiutil):
        for kind, out in (("corespotlight", "ApplEm.cshelpindex"), ("lsm", "ApplEm.helpindex")):
            subprocess.run([hiutil, "-I", kind, "-Caf", os.path.join(lproj, out), "-s", "en", lproj],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        print("make_help.py: hiutil not found; the Help Book has no search index", file=sys.stderr)


if __name__ == "__main__":
    main()
