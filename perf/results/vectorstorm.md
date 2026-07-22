# Vectorstorm — a float/SIMD workload that actually breaks Simple

*2026-07-21, Apple M5. The follow-up to the gauntlet: where the integer
gauntlet *tied* C, this float, vectorization-heavy test was built to find
Simple's real ceiling — and it does. All seven languages compute the
identical checksum `811875086734`.*

## What it does

Three phases, all `f64`, chosen so the hot loops are **element-wise** —
each output depends only on its own index:

| phase | work | why it's here |
|-------|------|---------------|
| V — vector mix | 120,000 passes of `x[i] = x[i]*0.99 + y[i]*0.01` over 8,192 elements | pure SIMD sweet spot |
| P — polynomial map | 70,000 passes of a Horner polynomial per element | pure SIMD sweet spot |
| M — float matmul | 128×128 matmul ×200 | array addressing (reduction stays scalar) |

Element-wise loops are exactly what auto-vectorizers turn into NEON/AVX
(4–8 lanes per instruction) — and, because there's no reduction reordering
inside them, they compute **bit-identically** whether run scalar or
vector. That's what lets all seven languages agree on the checksum while
some do the work 4× faster. clang emits **80 NEON vector instructions**
for this file; Simple's backend emits none.

## Results (median of 9 runs)

| rank | language | time (s) | vs C | peak RSS (MB) | binary (KB) |
|------|----------|---------:|-----:|--------------:|------------:|
| 1 | C++     | 0.25 | 0.76x | 1.8 | 32 |
| 2 | C       | 0.33 | 1.00x | 1.8 | 32 |
| 2 | Rust    | 0.33 | 1.00x | 2.0 | 455 |
| 4 | Zig     | 0.43 | 1.30x | 1.8 | 68 |
| 5 | Go      | 0.88 | 2.67x | 4.5 | 2434 |
| 6 | **Simple** | **1.41** | **4.27x** | **1.8** | **48** |
| 7 | Swift   | 12.07 | 36.6x | 2.5 | 51 |

## The honest headline: this one breaks it

**Simple finishes 6th of 7 — 4.3× slower than C, beaten by every language
whose compiler auto-vectorizes** (C++, C, Rust, Zig) and even by Go, which
doesn't vectorize but gets fused multiply-add on arm64 that Simple's
backend doesn't emit. Only Swift is slower, for an unrelated reason (see
below).

This is the mirror image of the gauntlet, and deliberately so. The
gauntlet was scalar integer work and Simple tied C. Swap in floating-point
element-wise loops and the gap Simple hides in the lab shows up in full:

**Where Simple's 1.41s goes (measured):**

| phase | Simple time | share |
|-------|------------:|------:|
| V (vectorizable) | 0.80s | 57% |
| P (vectorizable) | 0.48s | 34% |
| M (matmul) | 0.13s | 9% |

**91% of Simple's time is in the two phases a vectorizing compiler shreds.**
clang does phases V+P in a fraction of the time because it processes two
doubles per NEON instruction; Simple runs one scalar `fmul`/`fadd` at a
time. There is no algorithmic trick hiding here — it is purely that
Simple's QBE-based backend has no auto-vectorization pass. This is the
single biggest known item on the optimization roadmap, and this benchmark
is what it costs today.

## Why Swift is 36× slower (it's not vectorization)

Swift's 12s is a different failure: idiomatic `[[Double]]` is an array of
heap-allocated arrays with copy-on-write and bounds checks on every
element access, and the matmul hammers exactly that. It's a real Swift
footgun, not a compiler-backend limit — flat-buffer Swift would be far
faster — but it's what the natural translation produces, so it stands.

## What this says about Simple

Nothing here is a surprise or an embarrassment; it's the honest boundary
of a language whose backend is deliberately small. On **scalar** work — the
gauntlet, and most real programs — Simple ties C. On **vectorizable float**
work it pays a ~4× tax until SIMD lands. The two benchmarks together are
the real picture:

- **Gauntlet (scalar/integer mix):** Simple 2nd of 7, ties C.
- **Vectorstorm (float/SIMD):** Simple 6th of 7, 4.3× C.

Same language, same 48 KB binaries, same automatic memory management and
data-race freedom in both. The gap is one named, roadmapped feature —
auto-vectorization — and vectorstorm is the benchmark that will measure it
the day it ships.

## Methodology notes

- Standard per-language flags (clang/clang++ `-O2`, `rustc -C opt-level=2`,
  `zig -OReleaseFast`, `swiftc -O`, `go build`) — identical to the rest of
  the perf lab. No `-march=native`, no `-ffast-math`.
- Determinism across seven float implementations is real: the element-wise
  loops don't reorder, and the final result is quantized to
  `int(sum * 1e6)`, which absorbs any last-bit FMA differences. Verified —
  all seven print `811875086734`.
- Sources: `perf/bench/vectorstorm/vectorstorm.{simp,c,cpp,rs,go,zig,swift}`.
