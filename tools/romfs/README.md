# tools/romfs

RomFS overlays (game files replaced when read; `runtime/sdk/fs/romfs.c`, `ORBITALS_ROMFS_PATCHES`).

| Script | What it does | Usage |
|---|---|---|
| `byml.py` | Minimal BYML reader (v2-v7, little- or big-endian, with v7 hash dictionaries) that prints the tree, e.g. `GameData/GameDataList` | `python3 tools/romfs/byml.py FILE` |
| `findbytes.py` | Searches byte sequences across the whole RomFS, decompressing nested zstd | see its header |
