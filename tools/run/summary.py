"""One-screen summary of a game run's log: how it ended, how far it got, frame times, memory and failures.

Why: every run was read with the same dozen greps (frame lines, FATAL, first calls, mem: vram, image creation
failed...), 767 of them over the sessions to 01/11/2026. This prints those answers in ~15 lines.

Usage: uv run tools/run/summary.py build/run/NAME.log [...]
Host addresses of a FATAL: tools/run/sym.sh < build/run/NAME.log.
"""

import re
import statistics
import sys
from collections import Counter
from pathlib import Path

PERF = re.compile(r"perf: frame (\d+): interval avg ([\d.]+) ms max ([\d.]+) ms.*?(?:world (-?[\d.]+) s)?$")
FRAME = re.compile(r"(?:perf|present|vk_exec)\D*frame (\d+)")
VRAM = re.compile(r"mem: vram frame (\d+): vma \d+ blocks (\d+) MiB.*heap1 (\d+)/(\d+) MiB")
# "has another format ... rewritten": a file of ours that every run throws away (pipeline-keys.bin from 05/11/2026 to
# 06/11/2026: the boot prewarm read nothing for a day and no summary showed it)
FAIL = re.compile(r"\[host\] ((?:[\w ]+: )+)(.*?(?:[a-z] failed|not implemented|cannot \w+|over ORBITALS_\w+|device lost|another format.*rewritten))")


def pct(xs: list[float], q: float) -> float:
    return sorted(xs)[min(len(xs) - 1, int(q * len(xs)))]


def frames(label: str, xs: list[tuple[float, float]]) -> str:
    if not xs:
        return f"{label}: no perf lines (ORBITALS_TRACE=perf or ORBITALS_FLIGHT prints them every 60 frames; runtime/core/diag/perf.c perf_base)"
    avg = [a for a, _ in xs]
    return (
        f"{label}: {len(xs)} windows of 60 frames, interval median {statistics.median(avg):.1f} ms, "
        f"p95 {pct(avg, 0.95):.1f}, worst max {max(m for _, m in xs):.1f}"
    )


def host_time(lines: list[str]) -> list[str]:
    """The last ORBITALS_HOST_TIME census, its five costliest functions."""
    starts = [i for i, ln in enumerate(lines) if "host time: frame" in ln]
    if not starts:
        return []
    head = lines[starts[-1]].split("host time: ", 1)[1].split(";")[0]
    rows = []
    for ln in lines[starts[-1] + 1 :]:
        m = re.search(r"([\d.]+) ms/s render\s+([\d.]+) ms/s other\s+(\d+) calls\s+(\S+)", ln)
        if not m:
            break
        rows.append((float(m[1]) + float(m[2]), f"{m[4]} {m[1]}+{m[2]}"))
    top = ", ".join(r for _, r in sorted(rows, reverse=True)[:5])
    return [f"host time ({head}, ms/s render+other): {top}"]


def summary(path: Path) -> list[str]:
    lines = path.read_text(errors="replace").splitlines()
    text = "\n".join(lines)
    out = [f"== {path}"]

    fatal = [ln for ln in lines if "FATAL: " in ln or "fatal signal" in ln or "in lifted function" in ln]
    exits = [ln for ln in lines if re.search(r"window closed; exiting|frametime: exit", ln)]
    if fatal:
        out.append("end: " + " | ".join(ln.strip()[:200] for ln in fatal[:3]))
    elif exits:
        out.append("end: " + exits[0].split("] ", 1)[-1][:160])
    else:
        out.append("end: no exit line (killed: timeout, ctl stop, OOM; or still running)")

    last = max((int(m[1]) for m in FRAME.finditer(text)), default=0)
    out.append(f"last frame {last}")

    times = [(float(m[2]), float(m[3])) for ln in lines if (m := PERF.search(ln))]
    out.append(frames("frames", times))
    out += [ln.split("] ", 1)[-1][:160] for ln in lines if "frametime: exit" in ln]

    vram = [(int(m[1]), int(m[2]), int(m[3]), int(m[4])) for m in VRAM.finditer(text)]
    if vram:
        f, vma, used, budget = max(vram, key=lambda v: v[2])
        out.append(f"vram peak: heap1 {used}/{budget} MiB at frame {f} (vma {vma} MiB)")
    out += host_time(lines)

    fails = Counter(re.sub(r"0x[0-9a-f]+|\d+", "N", m[1] + m[2])[-90:] for ln in lines if (m := FAIL.search(ln)))
    if fails:
        out.append("failures: " + "; ".join(f"{n}x {k}" for k, n in fails.most_common(6)))
    first = sum("first call" in ln for ln in lines)
    if first:
        out.append(f"nvn first calls: {first}")
    return out


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        print("\n".join(summary(Path(p))))


if __name__ == "__main__":
    main()
