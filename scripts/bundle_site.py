#!/usr/bin/env python3
"""Writes the demo as one self-contained HTML file.

Usage: scripts/bundle_site.py [OUTPUT]

The font and the two data scripts are inlined, so the file makes no network
requests for anything it needs and can be copied to any static host.
"""
import base64
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/site/index.html"
    page = (ROOT / "site/index.html").read_text()
    font = base64.b64encode((ROOT / "site/fonts/GeistSerif-Regular.woff2").read_bytes()).decode()
    page = page.replace('url("fonts/GeistSerif-Regular.woff2")', f'url(data:font/woff2;base64,{font})')
    for script in ("recording.js", "results.js"):
        tag = f'<script src="{script}"></script>'
        if tag not in page:
            raise SystemExit(f"{tag} is missing from site/index.html")
        page = page.replace(tag, f"<script>\n{(ROOT / 'site' / script).read_text()}</script>")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page)
    print(f"wrote {out}, {len(page.encode()) / 1024:.0f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
