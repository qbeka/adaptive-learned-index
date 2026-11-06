#!/usr/bin/env python3
"""Writes the demo as one self-contained HTML file.

Usage: scripts/bundle_site.py [OUTPUT]

The fonts, stylesheets and scripts are inlined, so the file makes no network
requests for anything it needs and can be copied to any static host.
"""
import base64
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONTS = ("GeistSerif-Regular", "Geist-Regular", "Geist-Medium")
STYLES = ("foundation.css", "style.css")
SCRIPTS = ("recording.js", "replay.js")


def inline(page: str, tag: str, replacement: str) -> str:
    if tag not in page:
        raise SystemExit(f"{tag} is missing from site/index.html")
    return page.replace(tag, replacement)


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/site/index.html"
    page = (ROOT / "site/index.html").read_text()
    for sheet in STYLES:
        css = (ROOT / "site" / sheet).read_text()
        page = inline(page, f'<link rel="stylesheet" href="{sheet}">', f"<style>\n{css}</style>")
    for name in FONTS:
        font = base64.b64encode((ROOT / f"site/fonts/{name}.woff2").read_bytes()).decode()
        page = inline(page, f"url('fonts/{name}.woff2')", f"url(data:font/woff2;base64,{font})")
    for script in SCRIPTS:
        code = (ROOT / "site" / script).read_text()
        page = inline(page, f'<script src="{script}"></script>', f"<script>\n{code}</script>")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page)
    print(f"wrote {out}, {len(page.encode()) / 1024:.0f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
