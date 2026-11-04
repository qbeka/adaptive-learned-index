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

# ALEX assumes x86 and C++14. third_party/ALEX-arm64 is a copy with portability
# fixes applied. They touch a CPU feature check, three bit intrinsics and the
# allocator calls C++20 removed. No index logic changes.
rm -rf third_party/ALEX-arm64
mkdir -p third_party/ALEX-arm64
cp -R third_party/ALEX/src third_party/ALEX-arm64/
patch --quiet -d third_party/ALEX-arm64 -p1 < third_party/patches/alex-arm64.patch
cp third_party/patches/x86intrin.h third_party/ALEX-arm64/x86intrin.h
sed -i '' -E \
  -e 's/typename Alloc::template rebind<(.*)>::other/typename std::allocator_traits<Alloc>::template rebind_alloc<\1>/' \
  third_party/ALEX-arm64/src/core/alex_nodes.h
sed -i '' -E \
  -e 's/(data|model)_node_allocator\(\)\.destroy\((.*)\);/std::destroy_at(\2);/' \
  third_party/ALEX-arm64/src/core/alex.h
