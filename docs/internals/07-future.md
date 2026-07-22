# 7. Future Architecture

What's designed but not built, and *how* it will be built. Language-level
"why" lives in [the spec](../spec.md) and [book ch. 9](../book/09-the-road-ahead.md);
this chapter is the implementation plan.

## MIR (v0.5) — ✅ implemented

> Shipped 2026-07-19; as-built notes in [codegen ch. 5](05-codegen.md).
> Results: fib 1.70x→**1.06x C** (ties Rust, beats Go), collatz
> 3.17x→1.88x (all div/rem eliminated; the rest is machine-level loop
> shape — QBE's department, per the doctrine), spawnwork 3.67x→**1.67x**,
> chanping ties C at 1.00x. `--no-opt` proved byte-identical to v0.4
> emission before any pass landed, and the differential harness (optimized
> vs unoptimized outputs must match on every example) caught a real
> copy-propagation bug on day one. One amendment: no separate `--emit-mir`
> flag — MIR prints as QBE text by design, so `--emit-ssa` already shows
> the post-optimization module.

Decided 2026-07-19 (user): **MIR only** (HIR stays reserved until a
feature needs desugaring); optimizations **always on** with `--no-opt`
for compiler debugging and lab A/B runs (no -O level zoo); rollout is
**pass-by-pass with a lab checkpoint after each pass** so every pass's
contribution is on the record.

```
AST ─sema─▶ typed AST ─build─▶ MIR ─passes─▶ MIR ─print─▶ QBE IR
```

**The IR:** functions of basic blocks; blocks of typed instructions with
explicit terminators (jmp/jnz/ret/hlt). Deliberately slot-based and
non-SSA — the same shape we emit today (QBE rebuilds SSA anyway; doing it
twice buys nothing). ARC's retain/release become first-class instructions
the passes can see. `--emit-mir` joins `--emit-ssa` as a debug view.
Division of labor is a hard rule: MIR does only what requires *language*
knowledge; registers, scheduling, and machine peepholes stay QBE's job.

**The five unfair advantages the passes exploit** (all Simple-specific):

1. *Whole-program visibility* — no separate compilation, no function
   pointers, no closures: the call graph is exact and static. LTO-grade
   optimization by default. (Also: stop `export`ing non-main functions.)
2. *No aliasing, by construction* — value semantics means calls can never
   modify caller variables and nothing overlaps; no alias analysis, ever.
3. *`let` is an oracle* — immutable-by-default hands us constant/copy
   propagation pre-proven by the type system; immutable strings make
   `len(s)` loop-invariant for free.
4. *Lengths in the types + honest `for` loops* — bounds-check elimination
   is constant arithmetic, not value-range analysis.
5. *ARC conventions are theorems* — borrowed args mean calls never release
   caller refs; no-shared-heap means counts are thread-local: retain/
   release pair elision is a local pattern match, even across calls. The
   only escape routes are five named instructions (return, send, spawn,
   store-into-escaping aggregate).

**Pass order and predictions** (checkpoint the lab after each):

1. *MIR build + printer, no passes* — must be zero-regression vs v0.4 on
   all benchmarks and all 9 golden tests, plus `--no-opt` differential
   testing (optimized vs not, outputs must match on every example).
2. *Inlining* — splice callee blocks (slot MIR makes it near-textual);
   always inline single-call-site functions, size-capped otherwise;
   unroll recursion 2–3 levels (the fib trick). Predict fib 0.63→~0.42 s.
3. *Strength reduction* — `(x % 2ᵏ) == 0` → masked compare (sign-safe);
   signed `x / 2ᵏ` → sign-fixup + `sar`; `x * 2ᵏ` → `shl`; magic-number
   division by other constants later. Predict collatz 1.65→~0.7 s,
   spawnwork 0.33→~0.15 s.
4. *Constant folding + DCE* — cascades with inlining; dead functions and
   unreachable blocks die.
5. *ARC pair elision and bounds-check elision* — gated on the dependency
   policy: each needs a benchmark that shows the cost first (a
   string-sharing benchmark and an array-heavy benchmark respectively).

Estimated size: MIR core + builder ≈ the current codegen relocated, plus
~150–300 lines per pass. The printer inherits the current emission code.

## ARC insertion (v0.3) — ✅ implemented

> Shipped 2026-07-19; the as-built description now lives in
> [codegen ch. 5](05-codegen.md). Deviations from this plan: no separate
> `runtime/arc.c` — retain/release/concat are emitted as QBE IR like the
> rest of the runtime; and the mimalloc evaluation concluded **reject**
> (system malloc matched it on strbuild). The rest landed as designed,
> including the return-of-local elision and non-atomic counts.

Semantics (decided 2026-07-19, replacing the earlier borrow-checker
design): every heap value carries a reference count maintained by
compiler-inserted code. There are **no new user-facing rules and no new
error messages** — ARC is pure code generation.

Performance analysis (2026-07-19) sharpened the design. Key structural
fact: **value semantics already shrank ARC's job to strings only** —
structs/arrays are stack values, so the refcount traffic Swift drowns in
mostly doesn't exist here. Cost ranking for string code:
malloc/free (~50–200 cycles) ≫ byte memcpy ≫ refcount bump (1–2 cycles) —
so the *allocator* is the perf lever at this stage, not count elision.

Implementation plan:

- **Representation:** every heap string gets a 16-byte header
  `{refcount: i64, len: i64}`; the value pointer points at the bytes
  (still NUL-terminated, so `puts` keeps working). String *literals* are
  emitted with the same header and an immortal sentinel count (−1) —
  retain/release skip immortals with one predictable branch, and the
  language has exactly one string representation. Bonus: `len()` becomes
  an O(1) load instead of `strlen`, and concat stops walking its inputs —
  **v0.3 should make string code faster while fixing the leak.**
- **Counts are non-atomic, permanently.** Decided 2026-07-19: v0.4
  channels **deep-copy strings on send** (Erlang's model), so no
  refcounted memory ever crosses a thread. Every program keeps
  1–2-cycle counting; data-race freedom is by construction, not analysis.
- **Ownership conventions:** function results are +1 (caller owns);
  plain arguments are borrowed (**no retain** — the caller's reference
  pins the value for the call); `let` copies retain; reassignment
  releases the old value; scope exit releases live strings.
  `return local_str;` elides retain+release as a codegen peephole (skip
  the scope release) — no MIR needed for the biggest pair.
  Statement-level temporaries (the `a + b` inside `a + b + c`) are
  released at end of statement.
- **Aggregates:** copying a struct/array with k str fields adds k
  retains; scope exit releases recursively (per-type release helpers —
  the "destructor"). Rare in practice; a MIR elision target later.
- **Sema is untouched.** ARC adds no judgments and no errors.
- **Verification:** every example runs under macOS `leaks --atExit`;
  the `strbuild` lab benchmark lands *before* ARC does, so v0.2's leak
  and v0.3's fix are both on the record, then mimalloc gets evaluated
  against that baseline per the dependency policy.
- **MIR optimization (v0.5):** general retain/release pair elision;
  Swift's ARC optimizer is the reference point.
- **Cycles:** unrepresentable until recursive struct references exist;
  `weak` is specified together with them, not before.

## Concurrency runtime (v0.4) — ✅ implemented

> Shipped 2026-07-19; built exactly to this plan (as-built notes now in
> [codegen ch. 5](05-codegen.md)). One addition beyond the plan: channels
> carry an element-destructor pointer (`$rc_rel_*` of the element type),
> run on items still buffered when the last reference drops — so dropped
> channels can't leak queued strings. Lab: beat hand-written C pthreads
> on chanping (0.30 s vs 0.34 s), tied Rust; spawnwork scaled 5.0x on 8
> workers; Go's 0.02 s chanping stands as the predicted OS-thread cost.

Decided 2026-07-19: buffered channels only (cap ≥ 1), no `close()`
(sentinel shutdown), `spawn` requires void functions. Full rationale in
the spec; implementation plan:

- **Everything emitted as QBE IR** — pthreads functions
  (`$pthread_create`, `$pthread_mutex_lock`, …) are just symbols, like
  `$printf`. No C runtime file, preserving the single-file-out pipeline.
- **Types:** `TypeKind::Chan{elem}`; `chan int` in types, `chan int(16)`
  as the creation expression. A channel value is an 8-byte handle
  (pointer), legal in structs/arrays/params like `str`. `typeHasStr`
  generalizes to `typeHasRc` (str or chan, transitively).
- **Channel block** (one malloc): refcount, elemsize, cap, count, head,
  tail + generously-padded inline `pthread_mutex_t` and two condvars
  (not-full, not-empty) + the ring buffer (`cap × elemsize`).
  `$simple_chan_send/recv` are type-agnostic lock → wait-while-full/empty
  → memcpy → signal → unlock. Scalar and struct messages never allocate
  on the hot path.
- **No atomics needed** (QBE has none): the channel handle's refcount —
  the only cross-thread count in the language — is guarded by the
  channel's own mutex (`$simple_chan_retain/release`). Retains of
  channels are rare, coarse events; string counts stay non-atomic.
- **`spawn f(a, b)`** → args deep-copied into a malloc'd packet
  (strings via `$simple_strcopy`, channels retained); per-function
  generated trampoline `$spawn_f(l %packet)` unpacks, calls `$f`,
  releases packet contents, frees, returns. `pthread_create` +
  immediate `pthread_detach` — fire-and-forget; results via channels.
- **Move-if-unique send:** before deep-copying a string element, check
  `refcount == 1` → transfer the pointer instead (the sender's reference
  was dying anyway). Race-free to check because the no-shared-heap
  invariant makes every string's count thread-local. Produce-and-send
  becomes O(1).
- **Sema additions** (tiny): spawn requires a void user function; chan
  element types must be sendable (everything is); `send`/`recv` builtin
  typing. No new safety errors exist — with no globals and copy-only
  channels, data races are structurally impossible.
- **Lab additions:** `chanping` (1M token ping-pong: raw channel cost vs
  Go/Rust-mpsc/C/C++ hand-rolled queues) and `spawnwork` (job-queue
  fan-out of CPU-bound work). Prediction on record: ~tie on spawnwork
  (compute dominates), lose to Go on blocking-heavy chanping (goroutine
  parking beats kernel wakeups), compete when buffers absorb the jitter.

## Bare metal (v0.6+)

- `unsafe { }` — parser marks the region; sema relaxes only pointer rules
  inside. Raw pointer type `*T` with `load`/`store`/arithmetic.
- `extern fn puts(s: *u8) -> int;` — declarations that emit QBE calls with
  no body; the C ABI comes free since QBE already speaks it.
- `--freestanding` — driver skips libc linking (`cc -nostdlib`), codegen
  stops emitting `printf`-based `print`, entry point becomes `_start`.
  Requires sized types (`u8`, `u16`, `u32`, `i32`) which QBE's `w`/`b`/`h`
  types already support.

## Testing strategy as this grows

`make test` (golden outputs) stays the backbone, and `make bench` (the
[performance lab](../../perf/README.md)) runs after every version — ARC's
retain/release costs will show up there the moment v0.3 lands, which is the
point. A `tests/compile-fail/` harness (programs that *must not* compile,
each with an expected error substring) is worth adding with v0.2's richer
type errors. ARC itself also needs leak checks: run examples under
`leaks --atExit` on macOS in CI fashion.
