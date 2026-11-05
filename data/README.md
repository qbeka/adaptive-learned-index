# Data

Nothing in this folder is tracked by Git except this file.

| Path | Source | How to get it |
|---|---|---|
| `sosd/wiki_ts_200M_uint64` | Wikipedia edit timestamps, from SOSD | `scripts/fetch_sosd.sh wiki_ts_200M_uint64` |
| `sosd/fb_200M_uint64` | Facebook user IDs, from SOSD | `scripts/fetch_sosd.sh fb_200M_uint64` |
| `sosd/osm_cellids_800M_uint64` | OpenStreetMap cell IDs, from SOSD | `scripts/fetch_sosd.sh osm_cellids_800M_uint64` |
| `sosd/osm_cellids_200M_uint64` | Every 4th key of the file above | `scripts/prepare_sosd.py` |
| `smoke/` | Synthetic, from a seed | `scripts/smoke.sh` |

The fetch script checks each file against the MD5 sum published in SOSD's own
download script. These datasets are not redistributed here. Their terms are set
by SOSD and the original data owners.

File format: one little-endian `uint64` count, then the keys in sorted order.

`site/recording.js` holds about 210 keys from `wiki_ts` for the replay. No dataset file is stored in this repository.
