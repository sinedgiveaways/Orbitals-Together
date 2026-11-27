# Audit: runtime/sdk/fs/romfs_res.c

16/11/2026, branch `cleanup/mkw-removal`. Read from the code; not run against Orbitals (no game files in the sandbox).

## What it does

`romfs_res.c` backs two settings of the Graphics tab: `ORBITALS_RESOLUTION` (`original` by default, or 540 to 2160
lines) and `ORBITALS_DETAIL_DISTANCE` (2 by default). It does nothing unless one of them differs from its default. The
Steam Deck profile sets `RESOLUTION=720` (`runtime/core/base/cfg.c` `DECK`), so on a Deck it always runs.

When it runs, at start (`romfs.c` `romfs_resolution_overlay`, after the mods and the language overlays, and only when
`ORBITALS_ROMFS_PATCHES` is unset):

1. It walks the RomFS index for a file whose path starts with `System/GymlArchive/general_format.Product_` and ends
   with `.sarc.zs` (`find_archive`).
2. It reads that file (a mod's copy first), decompresses it with zstd and looks in the SARC for the member
   `Gyml/GamePerformanceGraphicsConfigGParam/30_Extendted.game__GamePerformanceGraphicsConfigGParam.bgyml`.
3. In that BYML it rewrites the integer fields whose names contain `For3D` (to lines / 0.65 docked, lines handheld)
   or `Screen` (to lines), and the float `LODDistanceRatio` (to the detail distance).
4. It compresses the archive again and writes it with a `manifest.tsv` to `<game dir>/cache/resolution-<lines>/` (or
   `graphics-r<lines>-lod<x>/`), which `romfs.c` serves as an overlay. A directory written before from the same source
   is reused.

`romfs_resolution_lines()` tells the menu which lines were applied (`menu.cpp`, the resolution row's note).

Everything it reads is MKW's data layout: the Gyml archive, the SARC member name, the BYML field names and the 0.65
scene scale measured in MKW races. `tools/romfs/resolution.py` is the same tool for the command line.

## When the file is absent

No crash and no change to the game. `find_archive` returns NULL, `write_overlay` logs
`resolution: no System/GymlArchive/general_format.Product_*.sarc.zs in <romfs>: the game's own resolution` and returns
NULL, and no overlay is loaded. Its other failure paths (archive does not decompress, member or fields not found,
cannot write the directory) end the same way, with their own log line. The cost is one pass over the RomFS index at
start. The menu then shows the resolution the player chose, but nothing applies it.

## Does Orbitals have the file?

Not verified: the sandbox has no Orbitals files. Orbitals is an Unreal Engine game (`recomp/gen.py`), and Unreal games
ship their data in `.pak`/`.utoc` containers, not in Nintendo EPD's Gyml/BYML archives, so the file is very unlikely
to be there. To check on the `.nup` (PowerShell, from the repository):

```powershell
$nup = (Get-ChildItem "C:\Users\mco2v\Downloads\Orbitals Deluxe Edition\*.nup" | Select-Object -First 1).FullName
uv run python -c "import sys; sys.path.insert(0, 'tools/prepare'); from nup import Nup; f = Nup(sys.argv[1]).files('romfs'); print(len(f), 'RomFS files'); print('Gyml:', [p for p in f if 'GymlArchive' in p or 'GamePerformanceGraphicsConfig' in p][:10]); print('top folders:', sorted({p.split('/')[0] for p in f}))" "$nup"
```

`Gyml: []` confirms the absence. After a run with `ORBITALS_RESOLUTION=720`, `logs/orbitals.log` also says it
(`resolution: no System/GymlArchive/...`).

## Decision

Proposed: **remove**, once the check above shows `Gyml: []`. The code is MKW-only, never changes anything for
Orbitals, and leaves two menu rows (Resolution, Detail distance) that do nothing. What goes in that commit:
`romfs_res.c`, its calls in `romfs.c` and `menu.cpp`, `tools/romfs/resolution.py` and `res_fields.py`, the
`RESOLUTION` and `DETAIL_DISTANCE` settings and menu rows, the Deck profile's `RESOLUTION=720`, and the run scripts'
`build/romfs-res` default. Two details to settle then:

- The Deck profile's 720 lines (#317) had a real effect on MKW (36 to 24 ms a frame). Orbitals needs its own way to
  lower the render resolution on weak GPUs, found in its own engine settings. That is new work, not a rename.
- `tools/romfs/byml.py` and `findbytes.py` are generic (a BYML reader, a byte search) and can stay.

If the check finds the file, the decision changes to "keep and condition" and the code stays as it is.

## Result (16/11/2026)

`tools/analisis/romfs_check.py` (as `romfs_check.exe`) on `Orbitals Deluxe Edition [040032B023CA4000][v327680].nup`:
54 RomFS files across the base (v65536), the update (v327680) and the add-on; top folders `Engine`, `Orbitals`,
`__rom` and Unreal's manifests; 1 `.pak` and 4 `.utoc`/`.ucas`; no `.sarc.zs`, no `.bgyml`/`.byml`; `Gyml: []`.
The file is absent, so `romfs_res.c` and what depends on it were removed, as decided above.
