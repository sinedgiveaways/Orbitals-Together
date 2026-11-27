#!/usr/bin/env python3
"""One-shot migration of a user's settings from the old MKW_* names to ORBITALS_*.

Why: the runtime reads only ORBITALS_* and orbitals.ini, with no alias and no fallback (docs/environment-variables.md).
An installation or a shell profile from before the rename would silently lose its settings, so this script, run once
by the user, rewrites them. It is never part of the runtime and nothing in the runtime reads its table.

The rule: MKW_X becomes ORBITALS_X. The exceptions are in tools/migrate/mkw-to-orbitals.tsv ("old<TAB>new", or "old<TAB>-"
for a setting that no longer exists: its line is deleted).

What it changes (each listed before it is touched):
  - orbitals.ini next to the executable (folders given with --dir, default the current one): made from mkw.ini or
    orbital.ini, the keys written with the old prefix rewritten; the old file is kept as <name>.bak.
  - Shell profiles: ~/.bashrc, ~/.bash_profile, ~/.bash_login, ~/.profile, ~/.zshrc, ~/.zshenv, ~/.zprofile,
    ~/.pam_environment, ~/.config/environment.d/*.conf, ~/.config/fish/config.fish.
  - PowerShell profiles ($PROFILE of Windows PowerShell and PowerShell 7: current user, all hosts and current host).
  - The user's Windows environment variables (HKCU\\Environment): each MKW_* value is stored under its new name with
    the same type, then the old one is deleted, and running programs are told. A copy of the old values is written to
    ~/orbitals-migrate-backup-<time>.json first. The machine-wide variables (HKLM) need an administrator: they are only
    reported.
  - --move-data: ~/.local/share/mkw-recomp becomes ~/.local/share/orbitals-recomp (when the new one does not exist).

Nothing is written without --apply: the default is a dry run that only prints. Every file it rewrites is copied to
<file>.bak first (a second run adds a time stamp rather than overwrite a .bak).

Usage: python tools/migrate_config.py [--dir DIR ...] [--profile FILE ...] [--move-data] [--apply]
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TABLE = HERE / "migrate" / "mkw-to-orbitals.tsv"
OLD, NEW = "MKW_", "ORBITALS_"
NAME = re.compile(r"\bMKW_[A-Z0-9_]+\b")
DROP = "-"


# ---- the rule and its exceptions
def load_exceptions(path: Path = TABLE) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        old, sep, new = line.partition("\t")
        old, new = old.strip(), new.strip()
        if not sep or not old.startswith(OLD) or not (new == DROP or new.startswith(NEW)):
            raise SystemExit(f"{path}:{n}: expected 'MKW_NAME<TAB>ORBITALS_NAME' or 'MKW_NAME<TAB>-'")
        out[old] = new
    return out


def new_name(old: str, exceptions: dict[str, str]) -> str:
    """The new name of old, or DROP when the setting no longer exists."""
    if old in exceptions:
        return exceptions[old]
    return NEW + old[len(OLD):]


# ---- text files (profiles, ini)
def decode(raw: bytes) -> tuple[str, str]:
    """(text, encoding): the encoding keeps a byte order mark, so the file is written back the way it was read."""
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw.decode("utf-8-sig"), "utf-8-sig"
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16"), "utf-16"
    return raw.decode("utf-8", "surrogateescape"), "utf-8"


def encode(text: str, enc: str) -> bytes:
    if enc == "utf-16":   # the codec writes a little-endian mark of its own
        return text.encode("utf-16")
    return text.encode(enc, "surrogateescape")


def rewrite(text: str, exceptions: dict[str, str]) -> tuple[str, list[str]]:
    """The text with every MKW_* name replaced (a line naming a dropped setting is deleted), and what changed."""
    out, notes = [], []
    for line in text.splitlines(keepends=True):
        names = NAME.findall(line)
        if not names:
            out.append(line)
            continue
        if any(new_name(n, exceptions) == DROP for n in names):
            notes.append(f"delete  {line.strip()[:90]}")
            continue
        new = NAME.sub(lambda m: new_name(m.group(0), exceptions), line)
        notes.append(f"rewrite {line.strip()[:60]}  ->  {new.strip()[:60]}")
        out.append(new)
    return "".join(out), notes


def backup_path(path: Path) -> Path:
    bak = path.with_name(path.name + ".bak")
    return bak if not bak.exists() else path.with_name(f"{path.name}.{time.strftime('%Y%m%d-%H%M%S')}.bak")


def write_atomic(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    if path.exists():
        shutil.copymode(path, tmp)
    os.replace(tmp, path)


class Plan:
    def __init__(self, apply: bool):
        self.apply = apply
        self.changes = 0
        self.files = 0

    def say(self, text: str) -> None:
        print(text)

    def file(self, path: Path, exceptions: dict[str, str], dest: Path | None = None) -> None:
        """Rewrite path (into dest when given; path then becomes path.bak)."""
        try:
            text, enc = decode(path.read_bytes())
        except OSError as e:
            self.say(f"  cannot read {path}: {e}")
            return
        new, notes = rewrite(text, exceptions)
        if not notes and dest is None:
            return
        target = dest or path
        self.say(f"{path}" + (f"  ->  {target}" if dest else ""))
        for n in notes:
            self.say(f"    {n}")
        self.changes += len(notes) + (dest is not None)
        self.files += 1
        if not self.apply:
            return
        if dest is None:
            shutil.copy2(path, backup_path(path))
            write_atomic(path, encode(new, enc))
        else:
            write_atomic(dest, encode(new, enc))
            os.replace(path, backup_path(path))   # the old file stays, as .bak


# ---- the files of an installation and the user's profiles
def migrate_ini(plan: Plan, folder: Path, exceptions: dict[str, str]) -> None:
    new = folder / "orbitals.ini"
    old = next((folder / n for n in ("orbital.ini", "mkw.ini") if (folder / n).is_file()), None)
    others = [folder / n for n in ("orbital.ini", "mkw.ini") if (folder / n).is_file() and (folder / n) != old]
    for extra in others:
        plan.say(f"{extra}: left as it is (only {old.name} is migrated)")
    if new.is_file():
        if old:
            plan.say(f"{folder}: orbitals.ini exists, {old.name} left as it is")
        plan.file(new, exceptions)   # a new file can still carry old key names
        return
    if old:
        plan.file(old, exceptions, dest=new)


def shell_profiles() -> list[Path]:
    home = Path.home()
    names = [".bashrc", ".bash_profile", ".bash_login", ".profile", ".zshrc", ".zshenv", ".zprofile", ".pam_environment"]
    found = [home / n for n in names] + [home / ".config/fish/config.fish"]
    found += sorted((home / ".config/environment.d").glob("*.conf"))
    return [p for p in found if p.is_file()]


def powershell_profiles() -> list[Path]:
    found: list[Path] = []
    for exe in ("powershell", "pwsh"):
        if not shutil.which(exe):
            continue
        cmd = [exe, "-NoProfile", "-NonInteractive", "-Command",
               "$PROFILE.CurrentUserAllHosts; $PROFILE.CurrentUserCurrentHost"]
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        found += [Path(l.strip()) for l in out.splitlines() if l.strip()]
    docs = Path.home() / "Documents"
    for base in (docs, Path.home() / "OneDrive" / "Documents"):
        for sub in ("PowerShell", "WindowsPowerShell"):
            found += [base / sub / "profile.ps1", base / sub / "Microsoft.PowerShell_profile.ps1"]
    found += [Path.home() / ".config/powershell" / n for n in ("profile.ps1", "Microsoft.PowerShell_profile.ps1")]
    seen, out = set(), []
    for p in found:
        key = os.path.normcase(str(p))
        if key not in seen and p.is_file():
            seen.add(key)
            out.append(p)
    return out


# ---- the user's Windows environment
class WinUserEnv:
    """HKCU\\Environment (and, to report only, HKLM's)."""

    def __init__(self):
        import winreg

        self.reg = winreg

    def _open(self, root, write=False):
        r = self.reg
        return r.OpenKey(root, r"Environment" if root == r.HKEY_CURRENT_USER else
                         r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment", 0,
                         r.KEY_READ | (r.KEY_SET_VALUE if write else 0))

    def items(self, machine=False) -> list[tuple[str, object, int]]:
        r = self.reg
        out, i = [], 0
        with self._open(r.HKEY_LOCAL_MACHINE if machine else r.HKEY_CURRENT_USER) as k:
            while True:
                try:
                    out.append(r.EnumValue(k, i))
                except OSError:
                    return out
                i += 1

    def set(self, name: str, value, kind: int) -> None:
        with self._open(self.reg.HKEY_CURRENT_USER, write=True) as k:
            self.reg.SetValueEx(k, name, 0, kind, value)

    def delete(self, name: str) -> None:
        with self._open(self.reg.HKEY_CURRENT_USER, write=True) as k:
            self.reg.DeleteValue(k, name)

    def broadcast(self) -> None:
        import ctypes

        result = ctypes.c_ulong()
        ctypes.windll.user32.SendMessageTimeoutW(0xFFFF, 0x1A, 0, "Environment", 2, 5000, ctypes.byref(result))


def migrate_registry(plan: Plan, env, exceptions: dict[str, str]) -> None:
    items = env.items()
    old = [(n, v, t) for n, v, t in items if NAME.fullmatch(n)]
    for n, _, _ in env.items(machine=True):
        if NAME.fullmatch(n):
            plan.say(f"machine-wide variable {n}: not touched (needs an administrator); delete it and set "
                     f"{new_name(n, exceptions)} yourself")
    if not old:
        return
    have = {n for n, _, _ in items}
    plan.say("Windows user environment (HKCU\\Environment):")
    todo = []
    for n, v, t in old:
        new = new_name(n, exceptions)
        if new == DROP:
            plan.say(f"    delete  {n}  (no longer exists)")
        elif new in have:
            plan.say(f"    keep    {new} already set; {n} is deleted without overwriting it")
        else:
            plan.say(f"    rename  {n}  ->  {new}")
        todo.append((n, v, t, new))
    plan.changes += len(todo)
    if not plan.apply:
        return
    backup = Path.home() / f"orbitals-migrate-backup-{time.strftime('%Y%m%d-%H%M%S')}.json"
    backup.write_text(json.dumps([{"name": n, "value": v, "type": t} for n, v, t in old], indent=2), encoding="utf-8")
    plan.say(f"    old values saved in {backup}")
    for n, v, t, new in todo:
        if new != DROP and new not in have:
            env.set(new, v, t)
        env.delete(n)
    env.broadcast()


def move_data(plan: Plan) -> None:
    old = Path.home() / ".local/share/mkw-recomp"
    new = Path.home() / ".local/share/orbitals-recomp"
    if not old.is_dir():
        return
    if new.exists():
        plan.say(f"{old}: not moved, {new} exists")
        return
    plan.say(f"move {old}  ->  {new}")
    plan.changes += 1
    if plan.apply:
        shutil.move(str(old), str(new))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--dir", action="append", type=Path, default=[], help="an installation folder (default: the current one)")
    ap.add_argument("--profile", action="append", type=Path, default=[], help="one more file to rewrite")
    ap.add_argument("--move-data", action="store_true", help="also move ~/.local/share/mkw-recomp")
    ap.add_argument("--apply", action="store_true", help="write the changes (the default only prints them)")
    args = ap.parse_args()

    exceptions = load_exceptions()
    plan = Plan(args.apply)
    print("DRY RUN: nothing is written. Add --apply to migrate.\n" if not args.apply else "APPLYING the migration.\n")

    for folder in args.dir or [Path.cwd()]:
        migrate_ini(plan, folder, exceptions)
    files = shell_profiles() + powershell_profiles() + [p for p in args.profile if p.is_file()]
    for f in dict.fromkeys(files):
        plan.file(f, exceptions)
    if sys.platform == "win32":
        migrate_registry(plan, WinUserEnv(), exceptions)
    if args.move_data:
        move_data(plan)
    left = sorted(n for n in os.environ if NAME.fullmatch(n))
    if left:
        print(f"\nSet in this shell, not changed (close it and open a new one): {', '.join(left)}")

    print(f"\n{plan.changes} change(s) in {plan.files} file(s).")
    if not plan.changes:
        print("Nothing to migrate.")
    elif not args.apply:
        print("Nothing was written. Run again with --apply to migrate.")
    else:
        print("One-shot migration done: the old MKW_* variables were removed after migration. The program reads only "
              "ORBITALS_* now; there is no alias. Open a new terminal for the new environment.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
