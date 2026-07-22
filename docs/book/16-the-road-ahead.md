# 9. The Road Ahead

> **Everything in this chapter is designed but not yet implemented.** Syntax
> may still evolve. Each section is labeled with its target version. When a
> feature lands, it gets a real chapter and this one shrinks.

## v0.2 — data: structs, arrays, real strings ✅ shipped

Structs, fixed arrays with bounds checking, string building, and `for`
loops are real now — [chapter 9](09-structs-arrays-strings.md) teaches
them. What v0.2 deliberately left open: built strings aren't freed yet.
That debt is called in by the very next milestone.

## v0.3 — memory: freed for you, without a garbage collector ✅ shipped

This landed. Since v0.3, every program frees its strings automatically:
the leak checker reports zero bytes across the whole example suite, the
allocation benchmark's memory dropped from 308 MB to 1.5 MB, and speed
*improved* — the string header ARC added also made `len()` O(1). Here's
the design that shipped, kept for the story:

**The problem.** Heap memory (strings, arrays, structs) must be freed
exactly once. C makes *you* do it — and you'll forget, or do it twice.
Garbage collectors do it at runtime, pausing your program at unpredictable
moments. Rust does it at compile time, but first you spend weeks learning
ownership, moves, and lifetimes.

**Simple's answer: automatic reference counting (ARC).** The compiler
attaches an invisible counter to every heap value: *how many variables
point at this right now?* Bind it to another name — counter goes up. A
variable goes out of scope — counter goes down. Counter hits zero — freed,
that instant, right there.

What this means for you as the programmer: **nothing changes. That's the
feature.**

```simp
fn keep(msg: str) { ... }

fn main() {
    let s = "hello";
    let t = s;        // both name the same value — fine
    keep(s);          // fine
    print(s);         // fine
    print(t);         // fine — no moves, no borrows, no new errors, ever
}                     // last reference dies here → freed, exactly once
```

You write code as if memory were magic, and the compiler makes it true —
deterministically, with no collector pausing your program. This is the same
model Swift uses to ship phones.

The fine print, for the curious:

- Counter bumps cost a few cycles. The optimizer (v0.5) deletes the
  provably-unneeded ones at compile time — most of them, in practice.
- Reference *cycles* (a value that points back at itself) are ARC's classic
  leak — but they're impossible in Simple until recursive structs exist,
  and `weak` references will arrive together with those.
- `let` vs `let mut` is unchanged — mutability safety already works today.

## v0.4 — concurrency: as easy as Go ✅ shipped

`spawn` + channels are real — [chapter 10](10-concurrency.md) teaches
them. The lab measured our channel machinery *beating hand-written C
pthreads code*, and data races aren't errors to catch — the model makes
them unwritable. The known trade, on the record: spawns are OS threads
(coarse-grained); a Go-style M:N scheduler can arrive later with zero
syntax changes.

## The road to 1.0

Everything below is designed-but-unbuilt, in build order:

- **v0.6 — bare metal foundations.** Sized integers (`u8`, `u32`, …),
  bitwise operators, `unsafe { }` blocks with raw pointers and volatile
  access (every dangerous line greppable), and `extern fn` to call any
  C library directly.
- **v0.7 — programs that do things.** Growable lists, string
  indexing/slicing and number↔text conversion, and floats. *(Shipped.)*
  Program arguments, stdin, and files were planned here but moved to
  v0.85; recursive data uses a list plus indices, so the once-planned
  heap `box` was dropped entirely.
- **v0.8 — errors as values.** *(Shipped.)* Go-style `(T, error)`
  returns, `ok`/`fail`, multiple return values — the no-exceptions
  promise kept in full. [Chapter 12](12-errors-as-values.md) tells the
  story.
- **v0.85 — talking to the outside.** *(Shipped.)* `argc()`/`arg(i)`,
  `input()`/`read_all()`, `read_file` returning `(str, error)` exactly
  as the sequencing intended, `write_file`, and exit codes.
  [Chapter 13](13-talking-to-the-outside.md) tells the story — a Simple
  program is now a full Unix citizen.
- **v0.9 — hash maps, then modules.** *(Shipped.)* Phase 1: `map`
  joined `list` ([chapter 14](14-maps.md)), optimized until it beat
  Go's map and closed within 10% of Rust's on an 11.5M-operation
  benchmark. Phase 2: `import` ([chapter 15](15-modules.md)) — the end
  of the single-file era, with a flat namespace and per-file
  diagnostics. The full lab re-ran with zero regression after each.
- **v0.95 — the vectorization engine.** The big one: an optimizer pass
  that turns Simple's scalar float loops into real SIMD/NEON. Today a
  deliberately vectorization-heavy benchmark runs 4.3× slower than C
  *because* clang vectorizes and Simple doesn't; v0.95 exists to close
  exactly that gap, measured by exactly that benchmark.
- **v1.0 —** spec freeze, the last optimization passes, and a long-term
  dream now visible from here: rewriting Simple's own compiler *in
  Simple*.
- **Later — freestanding.** No libc, no runtime, your `main` *is* the
  machine, a Simple kernel booting in QEMU. Deliberately parked past
  1.0: speed and self-hosting earned the earlier slots.

## What Simple will never have

Classes and inheritance, exceptions, a garbage collector, macros, lifetime
annotations. If a feature needs them, we change the feature.

---

*You've reached the end of the book (for now). Build something! And when you
outgrow v0.1, come back — this chapter becomes real chapters, in order.*
