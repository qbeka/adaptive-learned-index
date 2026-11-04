#!/usr/bin/env python3
"""Derives osm_cellids_200M_uint64 from the 800M file.

This keeps every 4th key, the method SOSD uses for its 200M slices, so the key
distribution is preserved. The file format is one uint64 count, then the keys.
"""
import struct
import sys
from pathlib import Path

import numpy as np


def main() -> int:
    data_dir = Path(__file__).resolve().parent.parent / "data" / "sosd"
    source = data_dir / "osm_cellids_800M_uint64"
    target = data_dir / "osm_cellids_200M_uint64"
    if target.exists():
        print(f"{target.name}: already present")
        return 0
    keys = np.fromfile(source, dtype=np.uint64)[1:][::4]
    with open(target, "wb") as out:
        out.write(struct.pack("<Q", len(keys)))
        keys.tofile(out)
    print(f"{target.name}: wrote {len(keys)} keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
