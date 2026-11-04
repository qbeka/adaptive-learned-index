#!/usr/bin/env bash
# Clones each baseline at the commit pinned in third_party/PINS.
# Clones stay out of Git; only the pins are tracked.
set -euo pipefail
cd "$(dirname "$0")/.."
grep -v '^#' third_party/PINS | while read -r name url commit; do
  dir="third_party/$name"
  if [ ! -d "$dir/.git" ]; then
    git clone --quiet "$url" "$dir"
  fi
  git -C "$dir" fetch --quiet origin "$commit" 2>/dev/null || true
  git -C "$dir" -c advice.detachedHead=false checkout --quiet "$commit"
  echo "$name $(git -C "$dir" rev-parse HEAD)"
done
