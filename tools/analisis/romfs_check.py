"""What a .nup's RomFS holds, for the decisions that depend on the game's files (docs/romfs-res-audit.md).

Why: the runtime has code that reads files of the game it was first written for (runtime/sdk/fs/romfs_res.c reads
System/GymlArchive/general_format.Product_*.sarc.zs). Whether Orbitals ships such files decides whether that code stays.
This reads only the package's table of contents: no file of the game is read, decoded or copied.

Built as romfs_check.exe by .github/workflows/romfs_check.yml, for players without Python: drop the .nup on the exe,
or run it and paste the path. The report (romfs_check.txt, beside the exe) holds file names and counts only.

Usage: uv run tools/analisis/romfs_check.py [GAME.nup]
"""

import collections
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[0] / "prepare"))
from nup import Nup, is_nup  # noqa: E402

# Files the runtime's MKW code looks for (runtime/sdk/fs/romfs_res.c), and other formats that tell the engine apart.
GYML = ("GymlArchive", "GamePerformanceGraphicsConfig")
MARKERS = {
    "Unreal .pak": (".pak",),
    "Unreal .utoc/.ucas": (".utoc", ".ucas"),
    "SARC .sarc.zs": (".sarc.zs",),
    "BYML .bgyml/.byml": (".bgyml", ".byml"),
    "zstd .zs": (".zs",),
}


def out_dir() -> Path:
    """Beside the exe when frozen (PyInstaller), else the current folder."""
    return Path(sys.executable).parent if getattr(sys, "frozen", False) else Path.cwd()


def ask_path() -> Path | None:
    print("Drop the .nup file on this window (or paste its path), then press Enter:")
    try:
        text = input("> ").strip().strip('"').strip("'").strip()
    except EOFError:
        return None
    return Path(text) if text else None


def report(nup: Path) -> list[str]:
    pkg = Nup(nup)
    files = list(pkg.files("romfs"))
    lines = [f"package: {nup.name}", f"title {pkg.title_id}, version {pkg.version}"]
    lines.append("records: " + ", ".join(f"{tid:016X} kind {kind} v{ver}" for tid, kind, _, _, ver in pkg.packages))
    lines.append(f"{len(files)} RomFS files")
    gyml = [p for p in files if any(g in p for g in GYML)]
    lines.append(f"Gyml: {gyml[:10]}" + (f" (+{len(gyml) - 10} more)" if len(gyml) > 10 else ""))
    lines.append(f"top folders: {sorted({p.split('/')[0] for p in files})}")
    for name, exts in MARKERS.items():
        n = sum(1 for p in files if p.lower().endswith(exts))
        lines.append(f"{name}: {n}")
    ext = collections.Counter(Path(p).suffix.lower() or "(none)" for p in files)
    lines.append("extensions: " + ", ".join(f"{e} {n}" for e, n in ext.most_common(15)))
    lines.append("")
    lines.append("VERDICT: " + ("the MKW resolution file is ABSENT (romfs_res.c can go)" if not gyml
                                else "MKW-style Gyml files are PRESENT (keep romfs_res.c)"))
    return lines


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else ask_path()
    code = 0
    if not path or not path.is_file():
        print(f"romfs_check: no such file: {path}")
        code = 2
    elif not is_nup(path):
        print(f"romfs_check: {path} is not a .nup package")
        code = 2
    else:
        try:
            lines = report(path)
        except (OSError, ValueError, UnicodeDecodeError) as e:
            lines, code = [f"romfs_check: cannot read {path}: {type(e).__name__}: {e}"], 1
        text = "\n".join(lines) + "\n"
        print(text)
        dest = out_dir() / "romfs_check.txt"
        try:
            dest.write_text(text, encoding="utf-8")
            print(f"Saved to {dest}: send that file (or the lines above).")
        except OSError as e:
            print(f"(could not save {dest}: {e})")
    if getattr(sys, "frozen", False):   # double-clicked: keep the window open
        try:
            input("\nPress Enter to close.")
        except EOFError:
            pass
    return code


if __name__ == "__main__":
    sys.exit(main())
