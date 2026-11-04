#!/usr/bin/env python3
"""Prints one benchmark JSON line as a short row. Used by smoke.sh."""
import json
import sys

record = json.loads(sys.stdin.read())
if "ns_per_lookup" in record:
    print(f"{record['index']:8} {record['ns_per_lookup']:8.1f} ns  checksum_ok={record['checksum_ok']}")
else:
    print(f"{record['index']:9} {record['ops_per_s'] / 1e6:6.2f} Mops/s  checksum={record['checksum']}")
