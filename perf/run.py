#!/usr/bin/env python3
"""The Simple performance lab.

Benchmarks Simple against C, C++, Rust, and Go on identical algorithms.

    python3 perf/run.py [version-label]      # e.g. v0.1; default "dev"

Writes perf/results/<label>.md and prints it. Methodology in perf/README.md.
"""
import platform
import re
import statistics
import subprocess
import sys
import time
from datetime import date
from pathlib import Path

PERF = Path(__file__).resolve().parent
ROOT = PERF.parent
BUILD = PERF / "build"
RESULTS = PERF / "results"
# Timed runs per binary, after one warmup; median reported. Raised from 5
# to 9 on 2026-07-20: at 5 runs the sub-100ms benchmarks (strbuild,
# chanping, sortint) swung by 20-40% between lab runs, which was enough to
# mistake noise for a regression.
RUNS = 9

BENCHES = {
    "fib": "recursive fib(42) — function call overhead",
    "primes": "count primes < 3000000 by trial division — arithmetic + branches",
    "collatz": "total Collatz steps for 1..5000000 — tight data-dependent loops",
    "strbuild": "build 1M strings by repeated concat — allocation pressure & memory management",
    "chanping": "100k message round-trips between two threads — channel machinery",
    "spawnwork": "8 workers split collatz 1..5M — parallel speedup",
    "nbody": "IDIOMATIC CLASS — particle interaction, written naturally in each "
             "language (C/C++/Go mutate through pointers; Simple/Rust use values)",
    "matmul": "200x200 integer matrix multiply — nested loops, 2-D indexing",
    "sieve": "Sieve of Eratosthenes to 500k — array writes, memory bandwidth",
    "bitops": "popcount + hash mixing over 2M values — bitwise ops, u64",
    "sortint": "insertion-sort 60 x 800 ints — data-dependent branches, swaps",
    "mapbench": "hash maps: 10M int-key ops + 1.5M str-key wordcount + churn — "
                "each language's native map (Simple's is 340 lines of emitted QBE)",
    "vectorstorm": "float SIMD — element-wise f64 loops + reductions, the "
                   "auto-vectorization showcase (quantized checksum, FMA-agnostic)",
    "gauntlet": "brutal mixed workload — value-struct copies, list COW, string "
                "churn, branchy sort, deep recursion, cache-bound matmul (all integer)",
}

import os
# Architecture profile. Default is the native host (arm64 here). Set
# SIMPLE_LAB_ARCH=amd64 to build every language for x86-64 instead — on Apple
# silicon those binaries run under Rosetta 2. Because *every* language is
# translated identically, the vs-C ratios still reflect each compiler's amd64
# code quality (absolute times carry the shared Rosetta tax).
ARCH = os.environ.get("SIMPLE_LAB_ARCH", "native")

# lang -> (source extension, build command)
_LANGS_NATIVE = {
    "C": ("c", lambda s, o: ["clang", "-O2", str(s), "-o", str(o)]),
    "C++": ("cpp", lambda s, o: ["clang++", "-O2", str(s), "-o", str(o)]),
    "Rust": ("rs", lambda s, o: ["rustc", "-C", "opt-level=2", "-C", "debuginfo=0",
                                 str(s), "-o", str(o)]),
    "Go": ("go", lambda s, o: ["go", "build", "-o", str(o), str(s)]),
    "Zig": ("zig", lambda s, o: ["zig", "build-exe", "-OReleaseFast", "-lc",
                                 "-femit-bin=" + str(o), str(s)]),
    "Swift": ("swift", lambda s, o: ["swiftc", "-O", str(s), "-o", str(o)]),
    "Simple": ("simp", lambda s, o: [str(ROOT / "simplec"), str(s), "-o", str(o)]),
}
_LANGS_AMD64 = {
    "C": ("c", lambda s, o: ["clang", "-arch", "x86_64", "-O2", str(s), "-o", str(o)]),
    "C++": ("cpp", lambda s, o: ["clang++", "-arch", "x86_64", "-O2", str(s), "-o", str(o)]),
    "Rust": ("rs", lambda s, o: ["rustc", "--target", "x86_64-apple-darwin",
                                 "-C", "opt-level=2", "-C", "debuginfo=0",
                                 str(s), "-o", str(o)]),
    "Go": ("go", lambda s, o: ["go", "build", "-o", str(o), str(s)]),  # GOARCH via env
    "Zig": ("zig", lambda s, o: ["zig", "build-exe", "-target", "x86_64-macos",
                                 "-OReleaseFast", "-lc", "-femit-bin=" + str(o), str(s)]),
    "Swift": ("swift", lambda s, o: ["swiftc", "-target", "x86_64-apple-macosx13.0",
                                     "-O", str(s), "-o", str(o)]),
    "Simple": ("simp", lambda s, o: [str(ROOT / "simplec"), str(s),
                                     "--target", "amd64_apple", "-o", str(o)]),
}
LANGS = _LANGS_AMD64 if ARCH == "amd64" else _LANGS_NATIVE


def sh(cmd, env=None):
    return subprocess.run(cmd, capture_output=True, text=True, env=env)


def build(bench, lang):
    ext, cmdf = LANGS[lang]
    src = PERF / "bench" / bench / f"{bench}.{ext}"
    out = BUILD / f"{bench}_{ext}"
    env = None
    if lang == "Go":  # keep Go's build cache inside the repo
        env = dict(os.environ, GOCACHE=str(BUILD / "gocache"))
        if ARCH == "amd64":
            env["GOARCH"] = "amd64"
    t0 = time.monotonic()
    r = sh(cmdf(src, out), env=env)
    dt = time.monotonic() - t0
    if r.returncode != 0:
        print(f"BUILD FAILED {bench}/{lang}:\n{r.stderr}", file=sys.stderr)
        return None
    return {"exe": out, "compile_s": dt, "size": out.stat().st_size}


def run_once(exe):
    """Returns (wall seconds, max RSS bytes, stdout)."""
    t0 = time.monotonic()
    r = subprocess.run(["/usr/bin/time", "-l", str(exe)],
                       capture_output=True, text=True)
    wall = time.monotonic() - t0
    m = re.search(r"([\d.]+)\s+real", r.stderr)
    if m:
        wall = float(m.group(1))
    rss = None
    m = re.search(r"(\d+)\s+maximum resident set size", r.stderr)
    if m:
        rss = int(m.group(1))
    return wall, rss, r.stdout.strip()


def measure(exe):
    run_once(exe)  # warmup
    walls, rsss, out = [], [], None
    for _ in range(RUNS):
        w, r, o = run_once(exe)
        walls.append(w)
        rsss.append(r)
        out = o
    return statistics.median(walls), statistics.median(rsss), out


def tool_versions():
    def first_line(cmd):
        r = sh(cmd)
        return (r.stdout or r.stderr).strip().splitlines()[0]
    return [
        first_line(["clang", "--version"]),
        first_line(["rustc", "--version"]),
        first_line(["go", "version"]),
        "simplec (QBE backend via `qbe`, linked with system cc)",
    ]


def machine():
    r = sh(["sysctl", "-n", "machdep.cpu.brand_string"])
    cpu = r.stdout.strip() or platform.processor()
    return f"{cpu}, {platform.system()} {platform.release()}, {platform.machine()}"


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "dev"
    BUILD.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)

    lines = []
    lines.append(f"# Performance lab — Simple {label}")
    lines.append("")
    lines.append(f"- date: {date.today().isoformat()}")
    lines.append(f"- machine: {machine()}")
    if ARCH == "amd64":
        lines.append("- **target arch: x86-64** — every language built for x86-64 and "
                     "executed under Rosetta 2; the shared translation tax cancels in "
                     "the vs-C ratios")
    lines.append(f"- method: median of {RUNS} runs after 1 warmup; "
                 "max RSS via /usr/bin/time -l; identical algorithms, 64-bit ints, -O2-class flags")
    for v in tool_versions():
        lines.append(f"- {v}")
    lines.append("")

    for bench, desc in BENCHES.items():
        print(f"[{bench}] building + running...", flush=True)
        rows = {}
        outputs = {}
        for lang in LANGS:
            b = build(bench, lang)
            if b is None:
                continue
            wall, rss, out = measure(b["exe"])
            rows[lang] = {**b, "wall": wall, "rss": rss}
            outputs[lang] = out

        agree = len(set(outputs.values())) == 1
        cwall = rows.get("C", {}).get("wall")

        lines.append(f"## {bench}")
        lines.append("")
        lines.append(f"*{desc}*")
        lines.append("")
        result_note = list(outputs.values())[0] if agree else "MISMATCH: " + str(outputs)
        lines.append(f"All outputs agree: **{'yes' if agree else 'NO — INVALID RUN'}** "
                     f"(result: `{result_note}`)")
        lines.append("")
        lines.append("| language | time (s) | vs C | max RSS (MB) | binary (KB) | compile (s) |")
        lines.append("|----------|---------:|-----:|-------------:|------------:|------------:|")
        for lang, r in sorted(rows.items(), key=lambda kv: kv[1]["wall"]):
            rel = f"{r['wall'] / cwall:.2f}x" if cwall else "-"
            lines.append(
                f"| {lang} | {r['wall']:.2f} | {rel} | {r['rss'] / 1048576:.1f} "
                f"| {r['size'] / 1024:.0f} | {r['compile_s']:.2f} |")
        lines.append("")

    report = "\n".join(lines) + "\n"
    out_path = RESULTS / f"{label}.md"
    out_path.write_text(report)
    print(report)
    print(f"written: {out_path}")


if __name__ == "__main__":
    main()
