# The Simple Performance Lab

Benchmarks Simple against **C, C++ (clang -O2), Rust (rustc -O2), Zig
(ReleaseFast), Swift (-O), and Go** on identical algorithms, measuring:

- **speed** — wall time, median of 5 runs after a warmup
- **memory** — peak RSS (`/usr/bin/time -l`)
- **binary size** and **compile time** — for the full picture

## Running it

```
make bench                      # writes perf/results/dev.md
python3 perf/run.py v0.2        # after a release: label it with the version

# cross-architecture: build every language for x86-64 and run under Rosetta 2
SIMPLE_LAB_ARCH=amd64 python3 perf/run.py v0.2-amd64
```

The `amd64` profile translates every language identically, so the vs-C ratios
still reflect each compiler's x86-64 code quality (absolute times carry the
shared Rosetta tax). It also confirms Simple's cross-compiled output is
correct on a second architecture — every benchmark's checksum must still agree.

**Run this after every version** and commit the labeled report to
`perf/results/`. The point is the *trend*: v0.2 adds heap strings, v0.3 adds
ARC — each report shows exactly what those features cost (or don't).

## Fairness rules

Benchmarks must be honest or they're worthless:

1. **Identical algorithm in every language.** Same recursion, same loop
   bounds, same math. No language-specific tricks, no SIMD, no memoization
   in one language but not another.
2. **Same integer width everywhere** — `int` in Simple is 64-bit, so C uses
   `long long`, Rust `i64`, Go `int64`.
3. **Comparable optimization levels** — `-O2`-class flags for all.
4. **Correctness gate** — the harness refuses to bless a run unless every
   language prints the identical result.
5. One process, one measurement — timings include process startup (that's
   real life, and it's why Go's runtime shows up in small benchmarks).

## Current benchmarks

| name    | exercises                                    |
|---------|----------------------------------------------|
| fib     | recursive fib(42) — function call overhead   |
| primes  | trial division < 3000000 — arithmetic, branches |
| collatz | steps for 1..5000000 — tight dependent loops |

All three use only v0.1 features (ints, functions, loops) so every language
competes on its code generator, not its standard library.

## Adding a benchmark

1. `mkdir perf/bench/<name>` with `<name>.simp/.c/.cpp/.rs/.go` — obeying
   the fairness rules; print exactly one line of output.
2. Add it to `BENCHES` in `run.py`.
3. When a new Simple version adds features (arrays, strings, spawn), add
   benchmarks that exercise them — that's how we watch ARC and the runtime
   pay their rent.
