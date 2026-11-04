#!/usr/bin/env bash
# Downloads SOSD datasets into data/sosd and checks the upstream MD5 sums.
# Usage: scripts/fetch_sosd.sh [dataset ...]
# Data stays out of Git. See data/README.md for sources and licence notes.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p data/sosd

dataverse='https://dataverse.harvard.edu/api/access/datafile/:persistentId?persistentId=doi:10.7910/DVN/JGVF9A'

url_of() {
  case "$1" in
    wiki_ts_200M_uint64) echo "$dataverse/SVN8PI" ;;
    fb_200M_uint64) echo "$dataverse/EATHF7" ;;
    books_800M_uint64) echo 'https://www.dropbox.com/s/y2u3nbanbnbmg7n/books_800M_uint64.zst?dl=1' ;;
    osm_cellids_800M_uint64) echo 'https://www.dropbox.com/s/j1d4ufn4fyb4po2/osm_cellids_800M_uint64.zst?dl=1' ;;
    *) echo "unknown dataset: $1" >&2; exit 1 ;;
  esac
}

md5_of() {
  case "$1" in
    wiki_ts_200M_uint64) echo 4f1402b1c476d67f77d2da4955432f7d ;;
    fb_200M_uint64) echo 3b0f820caa0d62150e87ce94ec989978 ;;
    books_800M_uint64) echo 8708eb3e1757640ba18dcd3a0dbb53bc ;;
    osm_cellids_800M_uint64) echo 70670bf41196b9591e07d0128a281b9a ;;
  esac
}

if [ "$#" -eq 0 ]; then
  set -- wiki_ts_200M_uint64
fi

for name in "$@"; do
  file="data/sosd/$name"
  want="$(md5_of "$name")"
  if [ -f "$file" ] && [ "$(md5 -q "$file")" = "$want" ]; then
    echo "$name: already present"
    continue
  fi
  echo "$name: downloading"
  curl -sSL --retry 3 "$(url_of "$name")" | zstd -d -q -f -o "$file.part"
  got="$(md5 -q "$file.part")"
  if [ "$got" != "$want" ]; then
    echo "$name: checksum mismatch ($got), expected $want" >&2
    exit 1
  fi
  mv "$file.part" "$file"
  echo "$name: ok $got"
done
