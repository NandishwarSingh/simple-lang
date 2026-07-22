# The optimization catalogue

*A ranked work plan, written 2026-07-20 against the v0.6 lab numbers.*

Every entry is scored on **effect** (measured where possible, estimated
otherwise), **effort**, and — the column that decides the order —
**effect per unit of effort**. The list is sorted best-ratio first.

**Baseline:** geometric mean across the 11 benchmarks is **1.86x C**.

| benchmark | vs C | what limits it |
|-----------|-----:|----------------|
| sieve     | 1.00x | — (at parity) |
| fib       | 1.03x | — |
| chanping  | 1.04x | — (Go's goroutines are 13x us, but C is not) |
| primes    | 1.06x | — |
| spawnwork | 1.33x | same body as collatz |
| collatz   | 1.35x | shifted operands, `csinc`, two branches/iteration |
| strbuild  | 1.50x | malloc traffic |
| sortint   | 1.67x | 3 surviving bounds checks, addressing |
| matmul    | 2.50x | no vectorization, address arithmetic |
| nbody     | 5.50x | no vectorization, structs live in memory |
| bitops    | 13.0x | popcount idiom not recognised |

Effort scale: **S** = focused change, low risk. **M** = a real pass with
its own tests. **L** = multi-day, touches architecture. **XL** = a
project in its own right.

---

> **Peepholes 2b + 2c: DONE (2026-07-20).** **primes now 1.00x C** (Simple
> beats clang: 0.17 s vs 0.18 s over 11 runs). Five of eleven benchmarks
> are now within 3% of C. Lab methodology also fixed — `RUNS` raised 5 → 9
> after a false "regression" turned out to be noise in the sub-100ms
> benchmarks. See `perf/results/v0.6-peep.md`.
>
> **Tier 1 status: DONE (2026-07-20).** Geomean **1.86x → 1.45x C**
> (predicted 1.48x). bitops 13.00x → **1.00x**; nbody 5.50 → 4.40; fib →
> 1.00x; sortint's last 3 bounds checks eliminated. Loop unrolling was
> implemented, measured, and **removed** — it regressed nbody, sieve and
> sortint by compounding in nested loops, and improved nothing. Full
> write-up in `perf/results/v0.6-tier1.md`.

## Tier 1 — do these first (high effect, low effort)

### 1. Loop-idiom recognition — S effort, huge effect
**Ratio: the best on the list by a wide margin.**

Match whole loop shapes and replace them with the instruction or call
that does the job:

- `while (x != 0) { x = x & (x - 1); n = n + 1; }` → hardware popcount
  (`fmov`+`cnt`+`addv` on ARM64). **bitops 13.0x → ~1.05x.**
- `for (i in 0..n) { a[i] = c; }` → `memset`. Hits sieve's init, matmul's
  zeroing, and every `[value; N]` fill.
- `for (i in 0..n) { a[i] = b[i]; }` → `memcpy`.
- Later: count-leading-zeros, byte-swap.

*Aggregate effect: geomean **1.86x → 1.48x** from this entry alone.*

Why it's cheap here: MIR is a small, regular CFG we own, and `for` loops
carry their trip count in the IR rather than being reconstructed from
arbitrary branches. LLVM needs a whole `LoopIdiomRecognize` pass to do
what we can pattern-match directly.

### 2. Bounds-check hoisting — S/M effort, moderate effect
Today a check is eliminated only when the index range is *statically*
provable. Hoisting handles the general case: for `for (i in 0..n) { a[i] }`
with `n` unknown, emit **one** check before the loop (`n <= len`) instead
of one per iteration. Kills the 3 survivors in sortint and makes bounds
checks effectively free in every counted loop.

Architecture bonus: the `for` variable is immutable and the bounds are
evaluated once — both language rules — so the hoisted check is trivially
valid. In C this needs loop-invariance proofs.

### 2b. ~~`cbz`/`cbnz` fusion~~ — ✅ DONE 2026-07-20
Landed in `arm64/emit.c`. **primes 0.19 → 0.17 s = 1.00x C — Simple now
beats clang there** (C 0.18 s). Inner loop 9 → 8 instructions.

### 2c. ~~CSE of frame-relative base addresses~~ — ✅ DONE 2026-07-20
Landed in `arm64/isel.c`: one address per slot per block, flushed at the
block top. sortint's inner loop 12 → 10 instructions (with 2b). Wall time
unchanged — the loop is now bound by the address arithmetic that indexed
addressing (#6) removes, confirming #6 as the next move.

### 3. Algebraic identities and wider constant folding — S effort, small but free
`x*1`, `x+0`, `x&-1`, `x|0`, `x^0`, `x-x`, `x/1`, double negation,
`(a+c1)+c2`, comparison folding, string-literal concatenation at compile
time. Individually tiny, collectively a few percent, and they *enable*
other passes by exposing constants.

### 4. ~~Loop unrolling with known trip counts~~ — REJECTED BY MEASUREMENT
Implemented and removed. Nested small loops compound (nbody's 8x8 became
64 inlined bodies), thrashing the instruction cache: nbody 0.22 → 0.36 s,
sieve 0.04 → 0.07, sortint 0.05 → 0.06, with **no benchmark improved**.
Out-of-order cores and branch prediction have largely absorbed the
benefit this optimization used to provide. Do not revisit without a
workload that demonstrates a need.

---

## Tier 2 — the real engineering (high effect, medium effort)

### 5. Scalar replacement of aggregates (SROA) — M/L effort, large effect on struct code
Break a struct that never escapes into individual registers, so its
fields never touch memory. nbody's `interact` builds a 4-field struct,
copies it to a return slot, then copies it into the array — all of which
becomes register moves.

**This is the single biggest architectural win available to us.** In C,
SROA requires alias analysis to prove nothing else can reach the struct.
Simple has *no aliasing in safe code*, so the proof is free. Estimated
**nbody 5.50x → ~2.8x** (vectorization still missing), geomean → 1.39x.

### 6. Indexed addressing modes — M/L effort, broad effect — **ATTEMPTED, REVERTED**
> **Status 2026-07-20:** implemented and reverted the same session. The
> naive fold (nop out the defining `add`/`mul`, build a `Mem`) is
> **unsound**: a defining instruction may live in another block, and the
> operands it referenced are not necessarily live at the use. Result was
> a segfault, caught by the golden tests. amd64 sidesteps this with a
> per-block address numbering (`anumber`/`amatch`) that *reconstructs*
> the address rather than deleting instructions. **Porting that machinery
> is the real task** — re-scoped from M to M/L. The payoff is now
> measured rather than estimated (see below), so it remains the top Tier
> 2 item.
>
> **Measured payoff:** Simple's matmul is **0.71x C on x86_64** (11
> indexed-addressing instructions, QBE selects them for amd64) against
> **2.50x on arm64** (zero). Same source, same MIR, same optimizer — the
> entire gap is this one backend feature. ARM64 can fold a scaled index into the access:
`ldr x0, [base, idx, lsl #3]` replaces our `lsl` + `add` + `add` + `ldr`.
QBE's ARM64 backend has *no* addressing-mode selection at all (only amd64
does). Measured on sortint's inner loop: 12 instructions, of which 4–5
are pure address arithmetic that this would remove. Helps matmul, sieve,
sortint, nbody — every array access in the language.

### 7. Induction-variable strength reduction — M effort, overlaps #6
Turn `base + i*8` recomputed each iteration into a pointer that
increments. Where #6 is a backend win, this is the MIR-level version and
also feeds unrolling. Pick one of #6/#7 first and re-measure before doing
the other — they compete for the same gap.

### 8. String allocation strategy — M effort, strbuild 1.50x → ~1.1x
strbuild is malloc-bound, not copy-bound. Options in increasing cost:
a size-class free list fed by ARC's deterministic frees (M); a bump
arena for provably short-lived strings (M/L, needs escape analysis);
small-string optimisation (L — changes the string representation
everywhere, including channels and ARC).

### 9. Channel single-producer/single-consumer fast path — M effort
A lock-free ring when a channel has one sender and one receiver, falling
back to the mutex otherwise. chanping already ties C; this is about
closing distance to Go without the full scheduler rewrite.

---

## Tier 3 — worthwhile, but earn their place later

### 10. Loop-invariant code motion — M effort, 5–10% broad
Hoist computations that don't change across iterations. Our advantage:
purity is trivial to establish (no aliasing, no globals, no exceptions),
where a C compiler must prove it.

### 11. Shifted-register operands — M effort, ~5% on collatz
`add x0, x1, x2, lsl #1` collapses the `lsl`+`add` pair our multiply
shift-add creates. Also `sub`, `and`, `orr`, `eor`.

### 12. Bottom-tested loops (rotation, done correctly) — M/L effort, ~5%
Saves one branch per iteration. **Note the earlier failure:** rotating in
MIR hid loop-body diamonds from if-conversion and *regressed* collatz.
It must happen in the backend *after* if-conversion, not before.

### 13. `csinc` / `csinv` / `csneg` — M effort, small
Fold an increment/negation into a conditional select. Collatz's
`3n+1`-then-select is the motivating case.

### 14. Interprocedural constant propagation — M effort, moderate
We compile whole-program by construction (no separate compilation, no
function pointers), so the call graph is exact and this needs no
conservatism. Specialise functions on constant arguments.

### 15. Escape analysis → stack-allocate non-escaping strings — M/L effort
ARC already tracks ownership and there is no aliasing, so "does this
value outlive the frame" is much easier here than in a typical language.
Feeds #8.

### 16. Tail-call optimisation — S/M effort, robustness more than speed
Turns self-tail-recursion into a loop. Modest performance value; real
value is not blowing the stack, which matters for a systems language.

### 17. Load/store pair (`ldp`/`stp`) — M effort
Two adjacent 8-byte accesses in one instruction. Speeds up every struct
copy that survives SROA, and our `blit` expansion.

---

## Tier 4 — big projects, big payoffs

### 18. Auto-vectorization — XL effort, largest remaining measured gap
Measured, not assumed: clang emits **80 SIMD register references and 23
NEON instructions** for nbody; we emit **zero**. This alone explains
nbody (5.50x) and much of matmul (2.50x).

With #1 also done, vectorization would take the geomean to **~1.23x**.
But it is genuinely an LLVM-scale subsystem: dependence analysis, cost
modelling, and a vector type system in the backend. Do not start it
before Tiers 1–2 are exhausted.

### 19. Green threads (M:N scheduler) — L/XL effort
Go does chanping in 0.02s against our 0.27s because parking a goroutine
costs ~100ns while waking an OS thread costs microseconds. We already
match C here, so this is about beating Go specifically. Zero syntax
change (the v0.4 design guaranteed that), but a real scheduler.

---

## Language gaps found by writing a real app (2026-07-20)

Writing `examples/plasma.simp` — a full SDL2 application — surfaced two
things the language is missing for serious C interop. Neither blocked the
app, but both cost a workaround:

Both were **fixed on 2026-07-21**, along with a linking flag:

- **Pointer interconversion inside `unsafe`** — any `*T` may stand in for
  any `*U`. `extern fn` declarations can now be honest about `void*`.
- **`null`** — usable only in `unsafe`.
- **`--link NAME` / `--libdir DIR`** — plasma builds in one command like
  every other example.

Writing the app also uncovered **three real compiler bugs**, all now
fixed with regression tests: sized-int function parameters were stored
at the wrong width; pointer equality compared only the low 32 bits; and
the pointer-arithmetic path fired for *every* binary operator, so `p ==
null` compiled to a subtraction.

## Tier 5 — measured or reasoned to be poor value

- **ARC retain/release pair elision.** Designed in v0.3 but never
  benchmarked as significant, because value semantics means only strings
  are refcounted and argument passing needs no counting at all. Our own
  data says the traffic isn't there.
- **Allocator replacement.** mimalloc was tested in v0.55 and
  **rejected** — system malloc matched it.
- **Instruction scheduling.** Modern out-of-order cores reorder anyway;
  QBE's register allocation is already decent.
- **Parallel compilation.** We compile in 0.04s.

---

## Recommended order

1. Loop-idiom recognition (**geomean 1.86x → 1.48x**)
2. Bounds-check hoisting
3. Algebraic identities + folding
4. Loop unrolling
5. SROA (**→ ~1.39x with #1**)
6. Indexed addressing *or* induction-variable strength reduction — measure, then decide
7. String allocation strategy
8. Re-measure everything, then reconsider Tier 3 in whatever order the
   new numbers demand

Tiers 1–2 together should land the geomean near **1.25–1.35x C** without
a single architectural gamble. Vectorization is the only path below
~1.2x, and it should be started only once the cheap wins are gone.

**Standing rule:** every entry gets a lab checkpoint of its own. Anything
that fails to earn its place gets deleted, as loop rotation already was.
