#!/usr/bin/env bash
#
# Documentation checks for camstream.
#
#   ./tools/check_docs.sh          check README.md and docs/
#   ./tools/check_docs.sh -q       only print a summary line
#
# Fails (exit 1) when any of the following is true:
#
#   1. an em dash or en dash is used as punctuation
#   2. a relative link points at a file that does not exist
#   3. an in-page anchor does not match any heading in the same file
#   4. a docs/*.md file is missing from the docs/README.md index
#   5. a docs/*.md file is missing its breadcrumb or its previous/next footer
#   6. a fenced block is unclosed, empty, or has no language tag
#
# Exits 0 when every check passes.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1

python3 - "$QUIET" <<'PY'
import pathlib
import re
import sys
import unicodedata

quiet = sys.argv[1] == "-q"

root = pathlib.Path(".")
targets = [root / "README.md"] + sorted((root / "docs").glob("*.md"))

problems = []


def slugify(heading):
    """GitHub's anchor rule: lowercase, drop punctuation, spaces to hyphens."""
    text = re.sub(r"`|\*\*|\*|_", "", heading.strip())
    text = text.lower()
    out = []
    for ch in text:
        if ch.isalnum() or ch == " " or ch == "-":
            out.append(ch)
    return "".join(out).replace(" ", "-")


def note(path, line, message):
    problems.append(f"{path}:{line}: {message}")


link_re = re.compile(r"\[([^\]]*)\]\(([^)\s]+)\)")
heading_re = re.compile(r"^(#{1,6})\s+(.*)$")

for path in targets:
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")

    # 1. em dash / en dash
    for number, line in enumerate(lines, 1):
        for bad in ("\u2014", "\u2013"):
            if bad in line:
                name = unicodedata.name(bad)
                note(path, number, f"{name} used as punctuation")

    # fenced blocks: balanced, non-empty, and tagged with a language
    depth = 0
    fence_start = 0
    fence_lang = ""
    fence_lines = 0
    for number, line in enumerate(lines, 1):
        stripped = line.lstrip()
        if stripped.startswith("```"):
            if depth == 0:
                depth = 1
                fence_start = number
                fence_lang = stripped[3:].strip()
                fence_lines = 0
                if not fence_lang:
                    note(path, number, "fenced block has no language tag")
            else:
                depth = 0
                if fence_lines == 0:
                    note(path, fence_start, "fenced block is empty")
            continue
        if depth:
            fence_lines += 1
    if depth:
        note(path, fence_start, "fenced block is never closed")

    # collect the anchors defined by this file
    in_fence = False
    anchors = set()
    for line in lines:
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = heading_re.match(line)
        if match:
            anchors.add(slugify(match.group(2)))

    # 2 and 3. links
    for number, line in enumerate(lines, 1):
        for _label, target in link_re.findall(line):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            file_part, _, fragment = target.partition("#")
            if file_part:
                resolved = (path.parent / file_part).resolve()
                if not resolved.exists():
                    note(path, number, f"link target does not exist: {file_part}")
                continue
            if fragment and fragment not in anchors:
                note(path, number, f"anchor does not match a heading: #{fragment}")

# 4 and 5. index coverage and per-document navigation
index = (root / "docs" / "README.md").read_text(encoding="utf-8")
for path in sorted((root / "docs").glob("*.md")):
    if path.name == "README.md":
        continue
    if path.name not in index:
        note(path, 1, "not listed in docs/README.md")
    body = path.read_text(encoding="utf-8")
    if "[camstream docs](README.md) /" not in body:
        note(path, 1, "missing the breadcrumb line under the title")
    if "| **Index**<br>[docs](README.md) |" not in body:
        note(path, 1, "missing the previous/index/next footer")

if problems:
    for problem in problems:
        print(f"FAIL {problem}")
    print(f"\n{len(problems)} documentation problem(s) found in "
          f"{len(targets)} file(s).")
    sys.exit(1)

if not quiet:
    print(f"OK   {len(targets)} files checked")
    print("     no em or en dashes, all links resolve, index in sync")
PY
