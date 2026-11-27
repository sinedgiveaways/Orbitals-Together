"""Feasibility report for porting another Switch 2 game onto this runtime: what in its exefs the runtime already covers.

Why: the recompiler (recomp/), the SDK replaced by name and NVN2 over Vulkan were written for MKW. Another game runs on
them only as far as it uses the same things; what it uses beyond them is the work of a port. This reads the game's own
exefs (from a dump folder or a .nup package, never anything in the repository) and says, without running the game:
  - the modules, their build ids and sizes, and the SDK version string;
  - which engine and middleware it is built with (Unity, Unreal, Nintendo's sead/agl, Havok, Wwise, FMOD, CRIWARE...);
  - its graphics API: the NVN2 driver's proc table in its sdk (tools/gpu/nvn2_procs.py) against the runtime's
    (runtime/gfx/nvn/procs/*.inc), and whether it uses Vulkan or NVN1 names instead;
  - its imports from the SDK by namespace: which the runtime replaces (HOST_OVERRIDE / HOST_OBSERVER in runtime/) and
    which end in a console service the runtime does not serve (tools/analisis/sdk_gaps.py's map);
  - the console services its main.npdm may open;
  - whether recomp/versions.py and the runtime know its build (they do not, for a new game: a row to add).
The report names functions of the game: keep it in build/ (the default) and out of git.

Usage: uv run tools/analisis/port_check.py GAME.nup|DUMP_DIR [--out build/port_check.md]
       DUMP_DIR holds exefs/ (or is the exefs folder itself). Takes ~10-60 s, mostly decompressing the modules.
       --dump-data FILE writes what it reads from runtime/ (replaced SDK names, NVN tables) as JSON, for the Windows
       executable (.github/workflows/port_check.yml): port_check.exe carries that file instead of the sources.
       --disasm main+0x181bf8[:0x180] sdk:clock_gettime[:0x200] ...: instead of the report, those ranges (an offset or
       an exported symbol; a trailing * also follows its direct calls two levels) disassembled (capstone), with calls through the PLT named by their import, direct calls by
       the module's exported symbol, and the module + offset of every branch target, to port_disasm.txt (a
       crash in orbitals.log names the addresses). It is the game's own code: send it, never commit it.
Windows executable: drop the .nup on port_check.exe (or run it and paste the path); the report goes next to the exe.
Run it with no argument and it also asks for the --disasm ranges (Enter for the report).
"""

import argparse
import json
import re
import struct
import sys
import tempfile
import time
from collections import Counter, defaultdict
from pathlib import Path

FROZEN = getattr(sys, "frozen", False)  # port_check.exe (PyInstaller): no runtime/ sources, their data baked in
ROOT = Path(__file__).resolve().parents[2]
DATA = Path(getattr(sys, "_MEIPASS", ROOT)) / "port_check_data.json"
sys.path.insert(0, str(ROOT / "recomp"))
sys.path.insert(0, str(ROOT / "tools/prepare"))
sys.path.insert(0, str(ROOT / "tools/gpu"))
sys.path.insert(0, str(ROOT / "tools/analisis"))

import nup  # noqa: E402
import nvn2_procs  # noqa: E402
import versions  # noqa: E402
from nso import Nso  # noqa: E402
from sdk_gaps import SERVICE_NS, overrides  # noqa: E402

# Engine and middleware markers, found as strings in the modules' images. A hit is a hint, not proof: the count says how
# strong it is.
MARKERS = {
    "Unity (IL2CPP)": [b"il2cpp", b"UnityPlayer", b"Unity Technologies", b"UnityEngine."],
    "Unreal Engine": [b"FEngineLoop", b"UnrealEngine", b"Unreal Engine", b"/Script/Engine", b"UE4Game", b"UE5"],
    "Godot": [b"Godot Engine", b"godot::"],
    "GameMaker": [b"YoYo Games", b"YYGameMaker"],
    "Nintendo sead/agl (MKW's family)": [b"sead::", b"agl::", b"N4sead", b"N3agl"],
    "Havok": [b"hkBase", b"Havok", b"hkpWorld", b"hknp"],
    "Wwise": [b"AK::SoundEngine", b"AkSoundEngine", b"Wwise"],
    "FMOD": [b"FMOD Studio", b"FMOD::", b"fmod_"],
    "CRIWARE": [b"CRI Middleware", b"criAtom", b"CRIWARE"],
    "Lua": [b"lua_pushvalue", b"LuaJIT", b"Lua 5."],
    "Mono": [b"mono_runtime_invoke", b"Mono.Runtime"],
    "Vulkan": [b"vkCreateInstance", b"vkQueueSubmit", b"VK_KHR_"],
}
SDK_VERSION = re.compile(rb"(?:NintendoSdk_|SDK MW\+Nintendo\+|NintendoSDK[ _]?)[\w.+\-]{2,80}")
NVN_NAME = re.compile(rb"\bnvn[A-Z][A-Za-z0-9]{3,80}\b")


def load_exefs(src: Path) -> tuple[dict[str, bytes], str]:
    """{file name: bytes} of the exefs, and where it came from."""
    if src.is_file() and nup.is_nup(src):
        pkg = nup.Nup(src)
        files = {name: pkg.read(e) for name, e in pkg.files("exefs").items() if "/" not in name}
        kinds = {0: "base", 1: "update", 2: "add-on"}
        recs = ", ".join(f"{t:016X} {kinds.get(k, f'kind {k}')} v{v}" for t, k, _, _, v in pkg.packages)
        # each record's own RomFS: Orbitals' setup copied 54 files, the update's alone (port-step1l), so what the
        # base record holds and how its fields read is printed with the raw record
        for i, (t, k, ex, ro, v) in enumerate(pkg.packages):
            own = pkg._walk(ro) if ro != nup.NONE else {}
            tops = sorted({p.split("/", 1)[0] for p in own})
            raw = pkg._t[0x30 + i * 0x40 : 0x70 + i * 0x40].hex(" ", 4)
            recs += (f"\n#   record {i}: {t:016X} kind {k} exefs dir {ex:#x} romfs dir {ro:#x}: {len(own)} RomFS files"
                     f" (top: {', '.join(tops[:8]) or '-'}); raw {raw}")  # fmt: skip
        recs += f"\n#   RomFS as the setup reads it (base and updates overlaid): {len(pkg.files('romfs'))} files"
        return files, f"{src.name}: .nup, game {pkg.title_id} version {pkg.version}; packages: {recs}"
    d = src / "exefs" if (src / "exefs").is_dir() else src
    if not d.is_dir():
        sys.exit(f"{src}: neither a .nup package nor a folder with exefs/")
    return {p.name: p.read_bytes() for p in sorted(d.iterdir()) if p.is_file()}, f"{d}: folder"


def nso_of(name: str, data: bytes, tmp: Path) -> Nso | None:
    if data[:4] != b"NSO0":
        return None
    p = tmp / name
    p.write_bytes(data)
    return Nso(str(p))


def namespace(mangled: str) -> str:
    """nn::hid for _ZN2nn3hid..., sead for _ZN4sead...; (C) for an unmangled name."""
    if not mangled.startswith("_Z"):
        return "(C)"
    s = mangled[2:]
    if s.startswith("N"):
        s = s[1:].lstrip("rVKRO")
    parts = []
    while s and s[0].isdigit() and len(parts) < 2:
        m = re.match(r"\d+", s)
        n = int(m.group())
        s = s[m.end() :]
        parts.append(s[:n])
        s = s[n:]
    if not parts:
        return "(other)"
    return "::".join(parts) if parts[0] == "nn" else parts[0]


def runtime_data() -> tuple[set[str], set[str], set[str]]:
    """(SDK names the runtime replaces, NVN names in its proc tables, NVN names implemented): from the sources, or
    from the file baked into the executable."""
    if FROZEN:
        d = json.loads(DATA.read_text())
        return set(d["replaced"]), set(d["nvn_listed"]), set(d["nvn_done"])
    return overrides(), *runtime_nvn()


def runtime_nvn() -> tuple[set[str], set[str]]:
    """(names in the runtime's proc tables, names with an implementation)."""
    listed = set()
    for inc in (ROOT / "runtime/gfx/nvn/procs").glob("*_procs.inc"):
        listed.update(re.findall(r"NVN_PROC\((\w+)\)", inc.read_text()))
    done = set()
    for src in (ROOT / "runtime").rglob("*.c"):
        if src.name == "nvn_procs.c":
            continue
        t = src.read_text(errors="replace")
        done.update(re.findall(r"NVN_API\((\w+)\)", t))
        done.update(re.findall(r"NVN2_IGNORED\((\w+)\)", t))
        done.update(re.findall(r"\bvoid nvnp_(\w+)\(Cpu", t))
    return listed, done


def npdm_services(data: bytes) -> list[str]:
    """Service names in main.npdm's ACI0 service access control (what the program may open)."""
    if data[:4] != b"META":
        return []
    aci, aci_size = struct.unpack_from("<II", data, 0x70)
    if data[aci : aci + 4] != b"ACI0":
        return []
    sac, sac_size = struct.unpack_from("<II", data, aci + 0x28)
    at, end, out = aci + sac, aci + sac + sac_size, []
    while at < end and at < len(data):
        ctrl = data[at]
        n = (ctrl & 7) + 1
        name = data[at + 1 : at + 1 + n].decode("ascii", "replace")
        out.append(name + (" (server)" if ctrl & 0x80 else ""))
        at += 1 + n
    return out


def plt_names(n: Nso) -> dict[int, str]:
    """GOT slot -> import name, from the JUMP_SLOT and GLOB_DAT relocations."""
    out = {}
    for off, typ, sym, _ in n.relocs:
        if typ in (1025, 1026) and 0 < sym < len(n.syms):  # R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT
            out[off] = n.syms[sym][0]
    return out


def plt_target(n: Nso, got: dict[int, str], t: int) -> str | None:
    """The import a PLT stub at t jumps to: adrp x16, page; ldr x17, [x16, #lo]; ... br x17."""
    if t + 8 > len(n.image):
        return None
    a, b = struct.unpack_from("<II", n.image, t)
    if a & 0x9F00001F != 0x90000010 or b & 0xFFC003FF != 0xF9400211:
        return None
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7FFFF) << 2)
    imm -= (1 << 21) if imm & (1 << 20) else 0
    slot = (t & ~0xFFF) + (imm << 12) + ((b >> 10) & 0xFFF) * 8
    return got.get(slot)


def disasm(mods: dict[str, Nso], specs: list[str]) -> list[str]:
    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    out = []

    def cstring(n: Nso, a: int) -> str | None:
        if not 0 <= a < len(n.image):
            return None
        end = n.image.find(b"\0", a, a + 200)
        raw = bytes(n.image[a : end if end >= 0 else a + 200])
        try:
            t = raw.decode("utf-8")
            if len(t) >= 3 and t.isprintable():
                return f'"{t}"'
        except UnicodeDecodeError:
            pass
        # UTF-16LE, Unreal's TEXT("...") (TCHAR is char16_t on the Switch): up to the first 16-bit 0
        w = bytes(n.image[a : a + 400])
        stop = next((i for i in range(0, len(w) - 1, 2) if w[i] == 0 and w[i + 1] == 0), -1)
        if stop < 6:
            return None
        try:
            t = w[:stop].decode("utf-16-le")
        except UnicodeDecodeError:
            return None
        return f'u"{t}"' if t.isprintable() else None

    # module -> address -> exported name, for MODULE:symbol specs and for direct calls inside a module (the SDK's
    # musl calls its own clock_gettime with a bl, not through the PLT)
    defined = {mn: {s[3]: s[0] for s in n.exports() if s[3]} for mn, n in mods.items()}
    # A spec ending in * also disassembles the functions its direct calls and tail branches reach in the same module
    # (not PLT imports), two levels deep and at most 24 of them, 0x180 bytes each: one run answers "what does this
    # call" without a second round trip.
    queue = [(s.strip().rstrip("*"), 2 if s.strip().endswith("*") else 0) for s in specs]
    seen: set[tuple[str, int]] = set()
    extra = 0
    while queue:
        spec, depth = queue.pop(0)
        m = re.fullmatch(r"(\w+)\+(0x[0-9a-fA-F]+)(?::(0x[0-9a-fA-F]+|\d+))?", spec)
        ms = None if m else re.fullmatch(r"(\w+):([\w.$@]+)(?::(0x[0-9a-fA-F]+|\d+))?", spec)
        if ms and ms.group(1) in mods:
            at = next((a for a, nm in defined[ms.group(1)].items() if nm == ms.group(2)), None)
            if at is None:
                out.append(f"## {spec}: {ms.group(2)} is not an exported symbol of {ms.group(1)}")
                continue
            mname, start, ln = ms.group(1), at & ~3, ms.group(3)
        elif m and m.group(1) in mods:
            mname, start, ln = m.group(1), int(m.group(2), 16) & ~3, m.group(3)
        else:
            out.append(f"## {spec}: not MODULE+0xOFFSET[:LENGTH] or MODULE:symbol[:LENGTH] of {', '.join(mods)}")
            continue
        if (mname, start) in seen:
            continue
        seen.add((mname, start))
        n = mods[mname]
        length = int(ln, 0) if ln else 0x100
        got = plt_names(n)
        callees: list[int] = []
        pages: dict[str, int] = {}  # register -> adrp page, for the string an adrp + add pair names
        here = defined[mname].get(start)
        out.append(f"## {mname}+{start:#x}{f' ({here})' if here else ''}, {length:#x} bytes")
        for ins in md.disasm(bytes(n.image[start : start + length]), start):
            line = f"{mname}+{ins.address:#08x}  {ins.bytes.hex()}  {ins.mnemonic:<8} {ins.op_str}"
            ops = [o.strip() for o in ins.op_str.split(",")]
            if ins.mnemonic == "adrp" and len(ops) == 2:
                pages[ops[0]] = int(ops[1].lstrip("#"), 16)
            elif ins.mnemonic == "add" and len(ops) == 3 and ops[1] in pages and ops[2].startswith("#0x"):
                a = pages[ops[1]] + int(ops[2][1:], 16)
                txt = cstring(n, a)
                if txt:
                    line += f"   ; {txt[:160]}"
            elif ins.mnemonic == "ldr" and len(ops) == 3 and ops[1].lstrip("[") in pages and ops[2].startswith("#0x"):
                a = pages[ops[1].lstrip("[")] + int(ops[2].rstrip("]!")[1:], 16)
                if a in got:
                    line += f"   ; GOT of {got[a]}"
            if ins.mnemonic in ("bl", "b") or ins.mnemonic.startswith(("b.", "cb", "tb")):
                t = int(ins.op_str.split("#")[-1], 16) if "#" in ins.op_str else None
                name = plt_target(n, got, t) if t is not None else None
                imported = name is not None
                name = name or (defined[mname].get(t) if t is not None else None)
                if name:
                    line += f"   ; -> {name}"
                far = t is not None and not start <= t < start + length
                if depth and not imported and far and ins.mnemonic in ("bl", "b") and t not in callees:
                    callees.append(t)
            out.append(line)
        out.append("")
        for t in callees:
            if extra < 24 and (mname, t) not in seen:
                extra += 1
                queue.append((f"{mname}+{t:#x}:0x180", depth - 1))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("game", type=Path, nargs="?", help="the dump: a .nup package or a folder with exefs/")
    default_out = Path(sys.executable).parent / "port_check.md" if FROZEN else ROOT / "build/port_check.md"
    ap.add_argument("--out", type=Path, default=default_out)
    ap.add_argument("--dump-data", type=Path, help="write the runtime's tables as JSON (for the executable) and exit")
    ap.add_argument("--disasm", nargs="+", metavar="MOD+0xOFF[:LEN] | MOD:symbol[:LEN]", help="disassemble these ranges instead")
    args = ap.parse_args()
    if args.dump_data:
        replaced, listed, done = runtime_data()
        d = {"replaced": sorted(replaced), "nvn_listed": sorted(listed), "nvn_done": sorted(done)}
        args.dump_data.parent.mkdir(parents=True, exist_ok=True)
        args.dump_data.write_text(json.dumps(d))
        print(f"{args.dump_data}: {len(replaced)} replaced SDK names, {len(listed)} NVN procs, {len(done)} implemented")
        return
    if args.game is None:  # started with a double click: ask
        typed = input("Drag the .nup file (or the dump folder) here and press Enter: ").strip().strip('"')
        args.game = Path(typed)
        ranges = input("Ranges to disassemble, e.g. main+0x181bf8:0x180 (Enter for the report): ").split()
        args.disasm = ranges or None
    t0 = time.time()
    files, origin = load_exefs(args.game)
    tmp_dir = tempfile.TemporaryDirectory(prefix="port_check_")
    tmp = Path(tmp_dir.name)
    mods = {}
    for name, data in files.items():
        n = nso_of(name, data, tmp)
        if n:
            mods[name] = n
    if not mods:
        sys.exit(f"{origin}: no NSO module in the exefs")
    if args.disasm:
        text = "\n".join(
            [f"# {origin}: the game's own code, for the port's work; do not commit", ""] + disasm(mods, args.disasm)
        )
        out = args.out.with_name("port_disasm.txt")
        out.write_text(text + "\n", encoding="utf-8")
        tmp_dir.cleanup()
        print(text)
        print(f"\nwritten to {out}", file=sys.stderr)
        return
    L: list[str] = []
    w = L.append

    w("# Port feasibility report\n")
    w(
        f"Source: {origin}. Generated by `tools/analisis/port_check.py`; names functions of the game: keep it out of git.\n"
    )

    # modules
    w("## Modules\n")
    w("| Module | Build id | Image | Imports | Exports | Known to this port |")
    w("|---|---|---|---|---|---|")
    # first 8 bytes of main's build id
    known_builds = {v.main_build_id: f"{versions.GAME} {v.name}" for v in versions.VERSIONS}
    sdk_known = set(nvn2_procs.TABLES)
    for name, n in mods.items():
        known = (known_builds.get(n.build_id[:16]) if name == "main" else None) or (
            "a listed sdk" if n.build_id in sdk_known else "no"
        )
        w(
            f"| `{name}` | `{n.build_id[:16]}` | {n.size / 2**20:.1f} MiB | {len(n.imports())} | {len(n.exports())} | {known} |"
        )
    other = sorted(set(files) - set(mods))
    if other:
        w(f"\nOther exefs files: {', '.join(f'`{o}`' for o in other)}")

    # SDK version and engine
    images = {name: bytes(n.image) for name, n in mods.items()}
    vers = Counter()
    for img in images.values():
        vers.update(m.decode("ascii", "replace") for m in SDK_VERSION.findall(img))
    w("\n## SDK version strings\n")
    w("\n".join(f"- `{v}` ({c}x)" for v, c in vers.most_common(5)) or "- none found")
    w("\n## Engine and middleware (string markers)\n")
    w("| Marker set | Hits | In modules |")
    w("|---|---|---|")
    engine_hits = {}
    for label, pats in MARKERS.items():
        where, hits = [], 0
        for name, img in images.items():
            h = sum(img.count(p) for p in pats)
            if h:
                where.append(name)
                hits += h
        engine_hits[label] = hits
        if hits:
            w(f"| {label} | {hits} | {', '.join(where)} |")
    if not any(engine_hits.values()):
        w("| none | 0 | |")

    # graphics
    w("\n## Graphics API\n")
    replaced, listed, done = runtime_data()
    names_in_code = set()
    for img in images.values():
        names_in_code.update(m.decode() for m in NVN_NAME.findall(img))
    driver = {}
    if "sdk" in mods:
        try:
            driver = nvn2_procs.procs(mods["sdk"], search=mods["sdk"].build_id not in sdk_known)
        except Exception as e:  # noqa: BLE001 - another sdk build may not match the search; the report says so
            w(f"- NVN2 driver proc table: not found in `sdk` ({type(e).__name__}: {e}).")
    if driver:
        new = sorted(set(driver) - listed)
        todo = sorted((set(driver) & listed) - done)
        w(
            f"- NVN2 driver in `sdk`: {len(driver)} procs. In the runtime's tables: {len(set(driver) & listed)}; "
            f"implemented: {len(set(driver) & done)}."
        )
        w(
            f"- Procs the runtime has no row for (new in this driver): {len(new)}"
            + (": " + ", ".join(f"`{x}`" for x in new[:60]) + (" ..." if len(new) > 60 else "") if new else "")
        )
        w(f"- Procs listed but not implemented (stop the run if the game calls them): {len(todo)}")
    used = sorted(names_in_code & (listed | set(driver)))
    w(
        f"- NVN names found as strings in the modules (what the game may ask nvnDeviceGetProcAddress for): {len(used)}; "
        f"of them without an implementation here: {len(set(used) - done)}"
        + (": " + ", ".join(f"`{x}`" for x in sorted(set(used) - done)[:60]) if set(used) - done else "")
    )
    unknown = sorted(names_in_code - listed - set(driver) - {"nvnBootstrapLoader"})
    if unknown:
        w(
            f"- NVN-looking names in no table (new procs, or strings that only look like one): {len(unknown)}: "
            + ", ".join(f"`{x}`" for x in unknown[:60])
            + (" ..." if len(unknown) > 60 else "")
        )
    if engine_hits.get("Vulkan"):
        w(
            "- **Vulkan names found**: the game may draw through Vulkan instead of NVN2; the runtime has no Vulkan front end "
            "for the game (its NVN2 layer would not be used)."
        )
    if not driver and not used:
        w("- No NVN at all: the graphics path is not NVN2, and the runtime's graphics layer does not apply as is.")

    # SDK imports
    w("\n## Imports from the SDK\n")
    sdk_exports = {s[0] for s in mods["sdk"].exports()} if "sdk" in mods else set()
    by_ns: dict[str, list[str]] = defaultdict(list)
    by_ns_all: dict[str, list[str]] = defaultdict(list)
    total = covered = 0
    for name, n in mods.items():
        if name == "sdk":
            continue
        for s in n.imports():
            if sdk_exports and s[0] not in sdk_exports:
                continue  # from a subsdk, not the SDK
            total += 1
            by_ns_all[namespace(s[0])].append(s[0])
            if s[0] in replaced:
                covered += 1
            else:
                by_ns[namespace(s[0])].append(s[0])
    lines = [
        f"# {origin}: imports from the sdk, replaced (+) or not (-) by the runtime; the game's names, do not commit"
    ]
    for ns in sorted(by_ns_all):
        lines.append(f"\n## {ns}")
        lines += [f"{'+' if x in replaced else '-'} {x}" for x in sorted(by_ns_all[ns])]
    imp = args.out.with_name("port_imports.txt")
    imp.parent.mkdir(parents=True, exist_ok=True)
    imp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    w(f"Every import, by namespace, with its state: `{imp.name}`.\n")
    w(
        f"Imports resolved by `sdk`: {total}; replaced by the runtime (same mangled name): {covered}. The rest run the "
        "SDK's own recompiled code: fine for pure libraries, a stop for code that opens a console service the runtime "
        "does not serve.\n"
    )
    w("| Namespace | Console service | Not replaced |")
    w("|---|---|---|")
    service_gap = 0
    for ns, items in sorted(by_ns.items(), key=lambda kv: (kv[0] not in SERVICE_NS, -len(kv[1]))):
        svc = SERVICE_NS.get(ns, "-")
        if svc != "-":
            service_gap += len(items)
        if svc != "-" or len(items) >= 5:
            w(f"| `{ns}` | {svc} | {len(items)} |")

    # services
    if "main.npdm" in files:
        svcs = npdm_services(files["main.npdm"])
        w(f"\n## Services in main.npdm ({len(svcs)})\n")
        w(", ".join(f"`{s}`" for s in svcs) or "none")

    # verdict
    w("\n## Summary\n")
    eng = [k for k, v in engine_hits.items() if v and k != "Vulkan"]
    w(f"- Engine hints: {', '.join(eng) or 'none'}.")
    if driver:
        w(
            f"- Graphics: NVN2, {len(set(driver) - listed)} driver procs new to the runtime, "
            f"{len(set(used) - done)} named in the game's code without an implementation."
        )
    w(
        f"- SDK: {total - covered} of {total} imports not replaced, {service_gap} of them in namespaces backed by a "
        "console service (each needs a host replacement before the game gets past it)."
    )
    w(
        "- Not known to recomp/versions.py: a patch table (spin loops: tools/cpu/spin_waits.py) and a version row are "
        "the first work of a port."
        if "main" not in mods or mods["main"].build_id[:16] not in known_builds
        else "- recomp/versions.py knows this build (a listed Orbitals dump)."
    )
    w(f"\n_{time.time() - t0:.0f} s._")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(L) + "\n", encoding="utf-8")
    tmp_dir.cleanup()
    print("\n".join(L))
    print(f"\nwritten to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    if not FROZEN:
        main()
    else:  # a console window that closes at the end: keep it open so the result (or the error) can be read
        try:
            main()
        except SystemExit as e:
            if e.code not in (None, 0):
                print(e.code if isinstance(e.code, str) else f"stopped ({e.code})")
        except Exception:  # noqa: BLE001 - shown to the player, who sends it
            import traceback

            traceback.print_exc()
        try:
            input("\nPress Enter to close.")
        except EOFError:  # no console to read from
            pass
