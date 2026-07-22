# The Gauntlet — a heavy mixed workload built to break Simple

*2026-07-21, Apple M5. Seven natively-compiled languages, one brutal
composite benchmark, all verified to compute the identical checksum
`235073448439`.*

## What it does

One program, five phases chosen to hammer every weakness Simple has:

| phase | what it stresses | Simple's known weak spot |
|-------|------------------|--------------------------|
| A — n-body | 30000 steps, 32 bodies, `interact` returns a struct **by value** | value-semantics struct copies |
| B — strings | build + FNV-hash 500,000 heap strings in a `list` | allocation churn, list COW |
| C — sort | insertion-sort 25,000 ints | data-dependent branches |
| D — recursion | `fib(40)` (~331M calls) | call overhead |
| E — matmul | 100×100 integer matmul ×150 | addressing / cache |

All integer, fully deterministic, so every language must produce the same
64-bit checksum — which all seven do, proving the workloads are
equivalent, not just similar.

## Results (median of 9 runs)

| rank | language | time (s) | vs C | peak RSS (MB) | binary (KB) |
|------|----------|---------:|-----:|--------------:|------------:|
| 1 | Rust    | 0.28 | **0.65x** | 22.6 | 456 |
| 2 | **Simple** | **0.42** | **0.98x** | 22.9 | **49** |
| 3 | C       | 0.43 | 1.00x | 13.3 | 33 |
| 3 | C++     | 0.43 | 1.00x | 13.3 | 33 |
| 5 | Zig     | 0.45 | 1.05x | 83.7 | 69 |
| 6 | Swift   | 0.53 | 1.23x | 10.0 | 55 |
| 7 | Go      | 0.76 | 1.77x | 21.3 | 2434 |

## The honest headline: the test failed to break Simple

**Simple placed 2nd of 7, tying C (0.42 vs 0.43) and beating Zig, Swift,
and Go** — on a workload specifically weighted toward the things it's
worst at. Only Rust (whose LLVM backend vectorised the struct and matrix
code) was clearly faster.

That deserves an explanation rather than a victory lap, because the
individual-benchmark lab shows Simple *losing* to C by 5.5x on n-body and
2.5x on matmul. Two reasons the gauntlet dilutes those:

1. **Those gaps are float-and-vectorisation gaps, and this test is
   integer.** Simple's 5.5x n-body loss is auto-vectorisation (clang
   emits NEON, Simple emits none). With *integer* bodies, clang can't
   vectorise the divide-heavy `interact` either, so the gap nearly
   closes. Same for integer matmul — much of C's advantage there was
   SIMD that integer code doesn't get.
2. **The workload is dominated by scalar phases where Simple already
   reached parity.** `fib` is ~0.14s of the 0.42s and Simple ties C on
   recursion (inlining + `csel`); sort and string-hashing are scalar
   loops where the v0.5–v0.55 optimizer work put Simple at ~1x.

Fairness was checked, not assumed: clang does **not** constant-fold the
phases in the combined program (dropping `fib(40)`→`fib(30)` cut C's time
0.43→0.28, proving the recursion is real work, not folded away). And the
identical checksum proves Simple isn't skipping anything.

## Where Simple *would* break

If the goal is to actually beat Simple, the lever is **floating-point,
vectorisable loops** — the one thing Simple's backend leaves entirely on
the table:

- float n-body: **5.5x C** (auto-vectorisation)
- float matmul: **2.5x C on arm64** (also indexed addressing — QBE
  selects it for amd64, giving Simple **0.71x C on x86_64** for the same
  source)

Those are on the roadmap (SIMD is the last, biggest item) and are the
honest answer to "how do you break Simple." A scalar, mixed, integer
workload — the kind most programs actually are — is not it.

## The other axes

Beyond raw speed, Simple's position is unusually strong: it matches C's
class on a mixed workload while shipping a **49 KB binary** (Go's is
2.4 MB, Rust's 456 KB), with automatic memory management and no data
races — and it was compiled by a compiler that fits in one 540 KB binary
with an embedded backend. Rust wins this benchmark; nothing else here
offers Simple's combination of speed, size, and safety.
