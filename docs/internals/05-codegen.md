# 5. Code Generation

**File:** `src/codegen.cpp` (entry: `genQBE(Program&) -> std::string`)

Input: a sema-checked AST (every `Expr::type` set, all rules verified).
Output: a complete QBE IL module as text. No validation happens here — see
the [type checker](04-type-checker.md) contract.

## QBE in sixty seconds

QBE IL is a tiny SSA language. What we use:

- Types: `l` (64-bit) for `int` and pointers, `w` (32-bit) for `bool`.
- Temporaries `%t1`, globals `$name`, labels `@name`. Constants inline.
- `%p =l alloc8 8` — stack slot (like C's `alloca`); `storel v, %p` /
  `%x =l loadl %p` for memory access.
- Arithmetic `add/sub/mul/div/rem`, comparisons like `csltl` (compare
  signed-less-than of `l`s → `w`).
- Control: `jnz cond, @a, @b`, `jmp @l`, `ret v`. Every block must end in
  one of these.
- `call $f(l %a, w %b)`; the `...` marker in a call marks where variadic
  args begin (ABI-critical for `printf` on ARM64 macOS).
- `data $s = { b "text", b 0 }` — global byte data.

## The central strategy: everything lives in stack slots

Real SSA construction (phi nodes, dominance frontiers) is the hard part of
compiler backends. We skip all of it:

- **Every variable gets a stack slot** (`alloc8`/`alloc4`); reads load,
  writes store. No phis ever needed — a `while` loop's variable updates are
  just stores to the same slot.
- **Every expression result is a fresh temporary** — trivially SSA.
- QBE's optimizer then promotes slots back into registers, deleting nearly
  all the loads/stores we emitted. We write naive IR; the binary is decent.

**The one trap:** `alloc` is dynamic, like `alloca` — an alloc inside a loop
body would grow the stack every iteration. So allocs are *collected during
codegen* (`allocs_`) and *emitted into the entry block* (`@start`), while
instructions stream into a separate buffer (`code_`); the two are stitched
together when the function ends. Never emit an alloc into `code_`.

## Function skeleton

```
export function l $fib(l %p0) {     # ret type l; 'main' is always w
@start
    %n_1 =l alloc8 8                # hoisted allocs first
    storel %p0, %n_1                # params copied into slots
    ...body...
    ret 0                           # fallback ret if control can fall off
}
```

Parameters arrive as `%p0, %p1...` and are immediately stored into ordinary
slots so all variables are uniform. `main` is emitted as returning `w` with
`ret 0` (the process exit code) regardless of its declared void-ness.

## Statements → blocks

Codegen tracks `terminated_`: did the current block already end with
`jmp`/`ret`? `placeLabel()` starts a new block, auto-emitting a fallthrough
`jmp` first if needed — QBE requires explicit terminators. After a
terminating statement, the rest of the statement list is *skipped*
(unreachable, and QBE would reject instructions after a `ret`).

- `if` → `jnz` + `@if_then` / optional `@if_else` / `@if_end`
- `while` → `@while_cond` / `@while_body` / `@while_end`; a stack of
  `{continue → cond label, break → end label}` pairs serves `break`/`continue`
- scoping mirrors sema: a scope stack maps names → slots; shadowing works
  because each `let` makes a *new* uniquely-numbered slot (`%x_3`)

## Expressions

Literals return themselves as operand text (`"42"`, `"1"`, `"$str_0"`).
Binary ops map 1:1 to QBE instructions. Two interesting cases:

- **`&&`/`||` short-circuit** — lowered to branches around the right-hand
  side, with the result passing through a dedicated (hoisted!) slot rather
  than a phi. `a && b`: if `a` is false, store 0 and skip `b` entirely.
- **`print`** — dispatches on the argument's sema-assigned type:
  `int` → `printf($fmt_int, ...)`, `str` → `puts`, `bool` → a tiny branch
  that `puts` either `$str_true` or `$str_false`.

## Strings and data

String literals are interned (`strPool_`): identical literals share one
`data` label. The emitter writes printable ASCII as `b "..."` chunks and
everything else (newlines, quotes, backslashes, any non-ASCII byte) as
numeric `b N` entries — so no QBE escaping rules to get wrong. Format
strings / `true` / `false` data are emitted only if actually used.

## Worked example

`examples/fib.simp --emit-ssa`, `main` only — annotated:

```
export function w $main() {
@start
	%i_1 =l alloc8 8                      # let mut i  (hoisted)
	storel 0, %i_1                        #   = 0
	jmp @while_cond_1
@while_cond_1
	%t1 =l loadl %i_1
	%t2 =w cslel %t1, 10                  # i <= 10
	jnz %t2, @while_body_2, @while_end_3
@while_body_2
	%t3 =l loadl %i_1
	%t4 =l call $fib(l %t3)               # fib(i)
	call $printf(l $fmt_int, ..., l %t4)  # print(...)
	%t5 =l loadl %i_1
	%t6 =l add %t5, 1
	storel %t6, %i_1                      # i = i + 1
	jmp @while_cond_1
@while_end_3
	ret 0
}
```

## Aggregates (v0.2)

Structs and arrays follow one rule: **an aggregate expression evaluates to
a pointer to stack storage**, and value semantics come from copying at
every boundary with QBE's `blit` (memcpy) instruction.

- **Layout:** every scalar field occupies 8 bytes (bools widen with
  `extuw` on store into aggregates; loads come back as `l` temporaries,
  which QBE accepts wherever a `w` is expected). Struct offsets are just
  running sums, memoized per struct in `layouts_`. No padding logic exists
  because nothing is ever smaller than 8.
- **Places:** `genPlace(expr)` returns the address of any assignable
  location or aggregate value: a variable's slot, `base + field offset`,
  or `base + index * elemsize` — with the **bounds check** emitted right
  there: compare, `jnz` to an `@oob` block that calls `$simple_oob` and
  `hlt`s, else fall through. Literal-index constant-folding is a future
  MIR job.
- **Copies:** `let` / assignment / `return` of aggregates are single
  `blit src, dst, size` instructions.
- **Calling convention** (ours — every callee is our own code, C interop
  comes later with `extern fn`): aggregate arguments pass as pointers and
  the *callee* blits into its own storage; aggregate returns pass a hidden
  out-pointer as the first parameter that the callee blits into at
  `return`. Struct-literal and call temporaries are hoisted allocs, safely
  reused across loop iterations because they're always copied out.
- **`for`** lowers like `while` plus a dedicated `@for_incr` block, which
  is what `continue` targets (so `continue` still increments).

## Strings and ARC (v0.3)

A string value is a pointer to NUL-terminated bytes, preceded by a
**16-byte header `{refcount, len}`** at pointer−16. Literals are emitted
in `data` with the same header and an immortal count of −1 (retain and
release skip negatives with one branch), so the whole language has exactly
one string representation and `puts` keeps working on the bytes.
The header made two things faster: `len()` is an O(1) load at pointer−8,
and `$simple_concat` reads both lengths instead of calling `strlen`.

**Length-prefixed means length-prefixed everywhere.** `==`/`!=` lower to
`$simple_streq` — length check, then `memcmp` over the whole span — *not*
`strcmp`, which was the original lowering and was a bug: a `str` can hold
embedded NUL bytes (`'\0'`, `s[i]`, `substr`), and `strcmp` stops at the
first one, so `"a\0x" == "a\0y"` wrongly reported equal and a
length-mismatched pair like `"a\0x" == "a"` did too. The header length is
the authority; the trailing NUL exists only so `puts`/C interop still
work.

Counting is **non-atomic permanently** — v0.4 channels deep-copy strings
on send, so refcounted memory never crosses a thread.

**Insertion rules** (all in this file; sema knows nothing about ARC):

- Ownership conventions: function results are **+1** (caller owns); plain
  arguments are **borrowed** — the caller's reference pins the value, so
  argument passing emits nothing. `str` *parameters* are marked
  `ownsRefs = false` in their slot: never retained, never released.
- `let x = <borrowed source>` (Var/Field/Index) retains; `let x = <owned
  producer>` (call result, concat, struct/array literal) transfers.
  `consumeOwned()` encodes this once; `StrLit` skips even the retain
  (immortals don't count). Reassignment retains the new value *then*
  releases the old (self-assignment safe).
- **Statement temporaries:** owned values not consumed into a variable
  (the `a + b` inside `print(a + b)`, a discarded call result) sit in
  `stmtTemps_` and are released at end of statement — also after every
  `if`/`while`/`for` condition evaluation.
- **Scope unwinding:** block ends release that scope's owned slots; loop
  bodies release theirs every iteration; `break`/`continue` unwind to the
  loop's recorded scope depth; `return` unwinds the whole function — with
  the **return-of-local elision**: `return local_str;` skips both the
  retain and that slot's release, transferring ownership for free.
- **Aggregates containing strings:** generated helpers
  (`$rc_ret_SPerson`, `$rc_rel_A3_s`, …, memoized per type) walk fields/
  elements and count every reachable string. They run at aggregate copy,
  overwrite, callee param copy, and scope exit. Programs whose aggregates
  hold no strings never generate or call them.

Verification gate for any change to this file: every example runs clean
under `leaks --atExit`, plus the churn stress test (struct copies,
discarded temps, early returns inside loops). The v0.3 lab run measured
all of this at **zero speed cost** (strbuild ratio *improved*, 1.71x →
1.50x vs C) — see `perf/results/v0.3.md`.

## Concurrency (v0.4)

Threads and channels, still with nothing linked but libc/pthreads —
`$pthread_create` etc. are called like `$printf`, and the whole runtime
is emitted QBE:

- **Channel block** (one malloc): refcount, elemsize, cap, count, head,
  tail, element-dtor pointer, then padded inline `pthread_mutex_t` + two
  condvars, then the ring buffer. `$simple_chan_send/recv` are
  type-agnostic: lock → wait-while-full/empty → memcpy → signal → unlock.
  The *element destructor* (an `$rc_rel_*` helper, or 0) runs on items
  still buffered when the last reference drops — dropped channels can't
  leak queued strings.
- **No atomics** (QBE has none): the channel handle's refcount — the only
  cross-thread count — is guarded by the channel's own mutex
  (`$simple_chan_retain/release`). String counts stay non-atomic.
- **Crossing a thread boundary** (`storeSendValue`, shared by send and
  spawn): scalars store; aggregates blit + `$rc_cpy_*` (deep-copies str
  fields, retains chan handles); strings go through `$simple_strcopy` —
  or `$simple_strmove` when the expression was an owned temporary:
  refcount 1 ⇒ transfer the pointer outright. The uniqueness check is
  race-free *because* no heap memory is shared between threads. Immortal
  literals pass through both uncopied (read-only forever ⇒ safe to share).
- **`spawn f(a, b)`**: args prepared into a malloc'd packet; a generated
  per-function trampoline `$spawn_f` unpacks, calls `$f` (aggregates
  passed as pointers into the packet — the callee's normal prologue
  copies), releases the packet's references, frees, returns. Created via
  `pthread_create` + immediate `pthread_detach`.
- The rc helper generator grew a third mode: `'r'` retain-all, `'d'`
  drop-all, `'c'` deep-copy-in-place — one walker, three ops.

## MIR and the optimizer (v0.5)

Emission no longer writes text — `emit()` parses each instruction into
`MInst {text, dst, ty, op, args}`, `placeLabel` starts `MBlock`s, and each
function becomes an `MFunc` (sig, allocs, blocks). The printer replays
stored text, so **an untouched module prints byte-identical to direct
emission** — that's how the skeleton proved zero-regression before any
pass existed. Pass-created instructions render their text via `mkMInst`.

Passes (run unless `--no-opt`; MIR ≈ QBE text by design, so `--emit-ssa`
shows the post-pass module):

- **`inlinePass`** — three rounds over an immutable snapshot of original
  bodies (so recursion unrolls 2–3 levels instead of exploding): clone
  blocks with `%i<n>_`/`@i<n>_` renaming, substitute params with caller
  operands, `ret v` → `copy` + `jmp @i<n>_cont`, split the call block.
  Self-recursion budget 400 insts; others: callee ≤ 80. Afterwards,
  unreferenced functions are dropped and only `main` stays exported.
- **`strengthPass`** — `(x % 2ᵏ) == 0` → single `and` when the remainder's
  only use is that compare (sign-safe by two's complement); signed
  `x / 2ᵏ` → the sar/shr/add/sar fixup; general `x % 2ᵏ` via the div
  sequence; `x * 2ᵏ` → `shl` (which also hits array-index scaling).
- **`foldDcePass`** (to fixpoint, ≤4 iters) — copy/const propagation
  **restricted to single-definition temps** (inlined multi-return
  functions define the call-result temp once per return site — the
  differential harness caught exactly this on day one); constant folding;
  `jnz const` → `jmp`; unreachable-block removal; DCE of pure
  instructions with unused results.

Verification gate for any pass change: all golden tests, the differential
check (every example compiled `--no-opt` and optimized must print the
same output), and `leaks --atExit`.

## v0.55 additions

MIR gained two passes, both resting on guarantees only Simple can make:

- **"The guard is a proof."** `knownDivisibleSlot()` finds blocks entered
  only via `jnz (x & (2ᵏ-1)) == 0`; inside such a block x is a known
  multiple of 2ᵏ, so `x / 2ᵏ` is exactly `sar x, k` — the four-instruction
  signed-division fixup disappears from the dependency chain.
- **Struct-copy elision.** An aggregate parameter the callee never
  assigns to is not copied at all: its slot *is* the caller's pointer
  (`%pN`), with `ownsRefs = false` so ARC treats it as borrowed, exactly
  like str/chan params. Sound only because Simple has no aliasing —
  nothing can mutate that storage during the call. Worth 1.8x on the
  nbody benchmark.
- Also: `x*3/5/9` → `shl`+`add`.

**Measured and removed:** loop rotation (see
`perf/results/v0.55.md`) — it hid loop-body diamonds from the backend's
if-conversion and regressed collatz.

## The backend, embedded (v0.55)

QBE 1.3 lives in `src/qbe/` (MIT, `src/qbe/LICENSE`) and compiles *into*
`simplec`; `main()` became `qbe_compile(in, out)` (`src/qbe.h`). No brew
dependency, no subprocess, reproducible builds. `cc` stays as the single
external dependency by policy. Local patches are all marked
`simple(v0.55)`:

- `arm64/isel.c` — **immediate operands** (`immarg()`): stock QBE
  materialized every constant into a register on ARM64; now `add/sub`
  (12-bit, optionally shifted), `and/or/xor/tst` (12-bit logical
  immediates), and shifts (0–63) stay immediate. Biggest single win of
  the release, and it helps every program.
- `arm64/isel.c` — **`selsel()`**: lowers QBE's internal `sel0`/`sel1`
  to `csel`, with flag fusion (`findflagdef()`) and `tst` folding
  (`seltst()`). The flag scan may pass over ordinary ALU instructions
  because on ARM64 only comparisons and calls write flags.
- `arm64/targ.c` — `.cansel = 1`: QBE's if-conversion pass existed but
  was **disabled on ARM64**. This one line is what makes branchless
  selects possible at all.
- `arm64/emit.c`, `ops.h` — `csel`/`fcsel` emission and the new `atst` op.
- `ifopt.c` — `MaxIns` 2 → 8, `MaxPhis` 2 → 3.

When touching `src/qbe/`, remember the Makefile's `$(QBEHDR)` dependency:
editing `ops.h` renumbers the op enum, and a partial rebuild will produce
translation units that disagree (symptom: `dying: no match for call(w)`).

## v0.6 additions

- **Sized integers.** `Type` carries `bits` + `uns`. QBE has only `w`
  (32-bit) and `l` (64-bit) registers, so narrower types live in `w` and
  are truncated at every boundary: `narrow()` after add/sub/mul/shl/neg
  on 8- and 16-bit types (32-bit wraps for free in `w`), width-correct
  `storeb/h/w/l` and sign-aware `loadsb/ub/sh/uh/sw/uw`. Unsigned types
  select `udiv`/`urem`/`shr` and the `cult*`/`cule*` comparison family.
- **Struct layout** is now naturally aligned (each field to its own size,
  the struct to its widest member), so `struct Packet { version: u8,
  flags: u8, length: u16, id: u32 }` is the 8-byte layout hardware
  expects.
- **Pointers.** `TypeKind::Ptr`; `AddrOf` returns a place address,
  `Deref` treats the pointer value as an address (so it works as both
  rvalue and assignment target), and `+`/`-` scale by the pointee size.
- **`extern fn`** declarations generate no body; variadic calls insert
  QBE's `...` marker at the right argument position.
- **Large copies call `memcpy`** above 128 bytes (`emitCopy`). QBE's
  `blit` unrolls into load/store pairs with no size cap — a 320 KB array
  copy became ~160k instructions and OOM-killed the compiler.
- **Initializer copy elision** (`destHint_`): an aggregate literal in a
  `let` is built directly in the new slot instead of a temporary plus a
  copy.
- **Bounds-check elimination** via integer **range analysis**
  (`ranges_`, `rangeOf`, `refine`, `widenForLoop`,
  `indexProvablyInRange`). An index is checked against the array length
  *from the type*; the range comes from:
  - `for (i in a..b)` → exactly `[a, b-1]` (immutable by language rule,
    bounds evaluated once);
  - **guards are proofs** — `refine()` narrows on `if`/`while`
    conditions and, crucially, across `&&` short-circuits: the right
    side of `a && b` is generated with `a` assumed, which is what makes
    `while (j >= 0 && a[j] > v)` provable;
  - **monotonic cursors** — `widenForLoop` inspects every assignment a
    loop body makes to a variable (`motionOf`): if they all decrement it
    keeps the *upper* bound, if all increment the *lower*, otherwise the
    range is dropped. No fixpoint iteration. This is what proves
    `j <= 798` in an insertion sort.
  - **no aliasing** means a call cannot change a local, so ranges
    survive calls with zero interprocedural work.
  - **Safety escape hatch:** `addrTaken_` collects every variable whose
    address is taken with `&`; those are never range-tracked, because an
    `unsafe` store through the pointer is invisible here.

  Results: sieve and matmul 8 checks → 0, sortint 8 → 3 (2.4x faster).
  Four adversarial programs in `tests/safety/` (run by `make test`) prove
  the necessary checks survive — including a variable mutated through an
  `unsafe` pointer.
- **Return-copy elision:** `return <aggregate literal>` builds directly
  into the caller's return slot, which is always a fresh temporary and
  therefore cannot alias what the literal reads.
- **Loop-idiom recognition** (`matchPopcount`, `emitPopcount`,
  `matchFill`), matched on the AST while the loop shape is still visible
  — LLVM has to rediscover these from a lowered CFG:
  - `while (x != 0) { x = x & (x-1); n = n+1; }` → a branch-free
    12-instruction SWAR population count. bitops 0.39 → 0.03 s, level
    with C.
  - `for (i in lo..hi) { a[i] = c; }` → `memset`, when the value is
    byte-uniform (1-byte elements, or zero).
- **Post-loop range refinement:** a loop guarded by `x >= k` whose body
  steps `x` down by at most `c` leaves `x >= k - c` on exit
  (`maxDownStep`) — it could not have iterated again without the guard
  passing. Proves `a[j+1]` safe after an insertion-sort inner loop;
  sortint went from 3 surviving checks to 0.
- **Algebraic identities** in `foldDcePass`: `x+0`, `x*1`, `x*0`,
  `x&-1`, `x|0`, `x^x`, `x-x`, zero shifts. Their real job is exposing
  constants for folding and DCE.

**Measured and removed:** full loop unrolling (see the note in the `For`
case, and `perf/results/v0.6-tier1.md`).

## Lists (v0.7)

`list T` is a growable, value-semantics container with **copy-on-write**.
A list handle points at a heap header
`[0]refcount [8]len [16]cap [24]elemsize [32]data`; the elements live in a
separately-malloc'd buffer (so growth reallocs the data, not the header —
the handle stays stable unless COW makes a new one). The runtime is
emitted as QBE (`listRuntime()`): `new`, `copy` (deep, with a per-element
retain fn), `retain`/`release` (release runs a per-element dtor at
refcount 0 then frees), `unique` (the COW: if shared, copy + release the
old reference), and `reserve` (grow).

- Lists are `isRcScalar` (an 8-byte handle), so the existing ARC plumbing
  gives `let b = a` a shared-with-retain and scope-exit release **for
  free** — COW then makes the sharing observably value-semantic: `push`,
  `pop`, and `l[i] = x` call `simple_list_unique` first, so a mutation of
  a shared list copies it and leaves the other reference untouched.
- **Parameters are the exception to borrowing:** a `list` param is
  *retained* on entry (owns a reference), not borrowed like `str`/`chan`,
  because a callee `push` would otherwise mutate the caller's list.
- **Across a thread boundary** a list is deep-copied
  (`storeSendValue` → `simple_list_copy` with a send-copy element fn that
  `strcopy`s strings and recursively copies nested lists), because the
  refcount is non-atomic — nothing heap-allocated is ever shared between
  threads. `spawnTramp` releases a list packet arg with
  `simple_list_release`, not the channel release (a real bug found and
  fixed during the ray-tracer test).
- Element rc is handled by generated per-type helpers:
  `listElemRetain` / `listElemDtor` / `listElemSendCopy`, all no-ops
  (passed as 0) when the element type holds no rc.

Verified: lists of ints, floats, strings, structs, and lists-of-lists;
value semantics; leak-free under `leaks`; the pure ray tracer's scene is
a `list Sphere` deep-copied across four threads, byte-identical to the
fixed-array version.

## Errors and multiple returns (v0.8)

Two features, one design rule: **reuse existing machinery, add no new
runtime concepts.**

**`error` is a nullable ARC string.** `ok` is the null pointer;
`fail(msg)` takes a reference to its message string and *is* that
pointer. Every rc operation routes through four null-safe wrappers
emitted on demand (`simple_err_retain/release/copy/msg`) — each is a
null check in front of the corresponding string routine, because
`simple_release` would crash on null. `e.msg` calls `simple_err_msg`,
which returns a copy of the message or a static immortal `""` for ok.
Consequences that fall out for free:

- `e == ok` / `e != ok` is an ordinary 64-bit compare against 0 —
  no special comparison code was added at all.
- struct fields, list elements, channel payloads, and spawn arguments
  of type `error` reuse the generated rc helpers; they only needed an
  `isErr` branch next to the existing `isStr` branches (including in
  `spawnTramp`, which previously assumed any non-str non-list rc
  scalar was a channel — the same latent-assumption bug class the
  list work hit before).
- `fail` of a string *literal* allocates nothing: the literal is
  immortal, the error is its pointer, retain/release no-op.

**Multiple returns reuse the aggregate-return path.** A function
`-> (T1, T2)` is `TypeKind::Multi` — a sentinel that never enters a
`Type` tree; the real list lives in `Function::rets`. Codegen treats
Multi exactly like a struct return: hidden `l %out` first parameter,
caller allocates the buffer (`multiSize`, slots packed like struct
fields with `multiOffset`). `return a, b;` stores each value at its
slot offset; named locals are retained and the normal scope unwind
releases them (no skip-slot trick — several values may name several
locals). `let (a, b) = f();` is the *only* consumer sema allows, so
the buffer's references transfer straight into the new variable slots
with zero extra rc traffic; `_` slots get an immediate release. The
optimizer needed no changes — the inliner already understood
`aggRet` functions, and a Multi function is just one of those.

Verified: `(int, error)`, `(str, error)`, `(Point, error)`,
`(int, int, int)`; destructuring in loops with early returns; errors
through `chan error` to worker threads; error fields surviving struct
copies; 11 misuse forms all rejected with specific messages; zero
leaks under `leaks` on every test.

## IO (v0.85)

Seven builtins, all riding on libc through the existing `cc` link — no
new dependency, and no parser changes at all (they're ordinary calls).

- **argc/argv capture:** `main` is the C entry point, so its emitted
  signature is `$main(w %argc, l %argv)` and its prologue stores both
  into two always-emitted globals (`$simple_argc/_argv`, 3 instructions
  — unconditional so the prologue never references undefined symbols).
  `argc()` is a plain load; `arg(i)` is a runtime helper that
  bounds-checks against the stored count (reusing `$simple_oob`, same
  trap as arrays) and wraps `argv[i]` into a fresh rc string.
- **stdin:** one helper, `$simple_read_stream(stopnl)`, shared by
  `input()` (stop at `\n`) and `read_all()` (stop at EOF): a `getchar`
  loop into a doubling `realloc` buffer — `getchar` avoids referencing
  the `stdin` FILE* global, whose symbol name differs across platforms.
- **read_file is a multi-return builtin:** it fills a caller `(str,
  error)` buffer exactly like a user multi-fn (`$simple_read_file(out,
  path)`), so `let (txt, e) = read_file(p)` flows through the same
  destructure path — which learned one thing: consult a small
  `builtinRets` table (mirrored in sema and codegen) before looking up
  user functions. Failure fills `("", "cannot open " + path)` using the
  immortal empty string and `$simple_concat`; the error owns the concat
  result's +1 directly. `write_file` returns its error as a plain
  single value.
- **exit(n) unwinds first:** it flushes statement temps and releases
  every live local *before* calling `exit`, then `hlt` terminates the
  block. This keeps `leaks -atExit` at zero even on early-exit paths —
  and avoids a real IR bug where pending temp releases would have been
  emitted into a dead block after the terminator.
- Flag hygiene: `needIo_` implies `needErr_` (empty-string data),
  `needConcat_`, and `needOob_` — the missing-`needOob_` case was
  caught by a link error (`_simple_oob` undefined) on the first
  IO-without-arrays program.
- Also fixed while here (pre-existing): `spawn push(...)` and friends
  crashed the *compiler* (uncaught map lookup) — every builtin is now
  in the spawn ban list.

Verified: args/stdin/files/exit end-to-end, statuses observed by the
shell (`return 7` → 7, `exit(3)` → 3), `arg(9)` traps, missing-file and
unwritable-path errors carry the path, golden test `io` runs under a
new stdin-redirect facility in the harness (`tests/stdin/<name>.txt`),
zero leaks on all paths including exit.

## Maps (v0.9)

`map K V` is the largest piece of emitted runtime yet (~340 lines of
QBE): a Python-dict layout — insertion-ordered entries array plus a
compact open-addressed index — because insertion-order iteration is a
*language promise* (determinism), not a nicety.

- **Header:** rc, live count, nentries (incl. dead), ecap, entries ptr,
  index ptr, icap (pow2), stride, keyIsStr, plus a one-entry lookup
  cache (lastkey/lastentry). Entry: state(8) | key(8) | value (padded
  to 8). Index: **i32 slots, calloc'd**, biased encoding 0=empty,
  1=deleted, j+2=entry — fresh index pages stay untouched (zero pages),
  and the i32 slots halve probe cache traffic vs i64.
- **Hashing is pure integer math** — FNV-1a for str keys, splitmix64
  finalizer for int keys — so hashes, probe sequences, growth points,
  and therefore *every observable behavior* are identical across
  architectures.
- **QBE does no inlining, so the runtime inlines itself:** `find` and
  `put` are kind-split (int/str) with the hash and key comparison
  inlined into the probe loop — no per-probe calls (memcmp only, on
  length-equal str candidates). `put` is single-pass: one probe both
  finds the key and remembers the first reusable slot; growth reprobes
  a clean table (rare, amortized).
- **The has-then-get idiom is recognized at runtime.** The language
  forces `if (has(m,k)) { m[k] }` (no entry API), so a successful
  `has`/`get`/`put`-update caches its entry in the header; the next
  lookup checks the cache first and **verifies the hit by key content**
  — a stale pointer can only miss, never lie. Anything that can move
  entries (insert, delete, grow, clone) clears it.
- **COW and threads reuse the list playbook:** `map_unique` (rc>1 →
  structural clone, retain keys/values) backs `m[k]=v` and `del`;
  `map_copy` (deep: strcopy keys, send-copy values) backs channels and
  spawn. Value-slot rc helpers are the *same* generated helpers lists
  use (`listElem*` keyed by element type), via a one-line adapter.
- **Iteration takes a snapshot reference:** `for (k in m)` retains the
  map into a hidden owned slot (name `for.map` — the `.` makes it
  unwritable from Simple) in its own scope, so `return`/`break` unwind
  it correctly; body mutations COW away and the walk stays stable. The
  loop variable is a *borrowed* key, like a str parameter.
- **Two lessons paid for in blood:** (1) an `alloc8` outside a QBE
  `@start` block is a dynamic alloca — one in grow's rehash loop grew
  the stack by 8 bytes *per entry* and segfaulted at 2M entries; every
  runtime slot now lives in `@start`. (2) During debugging, per-phase
  cross-checks against Python exonerated the map — a benchmark-file
  edit had silently doubled the workload, and "Simple disagrees with
  C" was actually "Simple answered a different question correctly."

Optimization was stepwise and lab-measured (the v0.9 gate): baseline
1.09s → inline-hash find/put 0.97 → i32 index 0.92 → lookup cache 0.87
→ split put 0.85 → calloc index 0.84 → pointer-identity cache round
**0.79s** on mapbench's 11.5M mixed ops. The last round rests on one
observation: *the map retains its keys, so pointer identity against a
live cached entry's key proves content* — which made the cache check
cheap enough to (a) inline at every `m[k]` read site (a hit does zero
calls), (b) bolt onto `put`'s entry (a read-modify-write like
`wc[w] = wc[w] + 1` probes once, not three times), and (c) drop the
content-verify call entirely. Entries-only `realloc` growth (index
untouched when only the entry array fills) came along in the same
round. Standings: Zig 0.38, C/C++ 0.43, Swift 0.66, Rust 0.72,
**Simple 0.79**, Go 0.87 — a from-scratch table in emitted IR beating
Go's runtime map and sitting 10% behind Rust's SwissTable, with the
full lab showing zero regression elsewhere.

## Growth notes

- **Auto-vectorization (v0.95) is built and emits real SIMD.** The
  legality check rides at for-loop emission (`classifyLoop`, exposed by
  `--vec-report`): because Simple has no aliasing and known lengths,
  legality is a *local per-lane test* — the analysis clang spends its
  whole budget on is nearly free here. The backend gained seven vector
  ops (`vload/vstore/vfadd/vfsub/vfmul/vfdiv/vdup`) typed as `Kd` so
  they reuse the FP register allocator, with the 128-bit width living in
  the emitter (`v.2d` on arm64, 3-operand AVX on amd64). Two shapes are
  emitted today: **element-wise** f64 loops (strip-mined 2-lane body +
  scalar remainder) and single-statement **reductions** (four
  independent 2-lane accumulators, combined in a fixed order — the
  unroll breaks the latency-bound dependency chain, and reassociation is
  licensed by the determinism decision). Register pressure over a cap
  forces a scalar fallback so a `Kd` vector never spills (which would
  drop its high lane). Every emitted result is bit-identical across
  NEON, AVX, and scalar. Measured (9-run lab): vectorstorm ~5.6x C → 2.2x C; a
  dot-product reduction runs 0.10s, ~4x faster than C `-O2`/`-O3`
  (which can't reassociate without `-ffast-math` and stays scalar).
  Ruled out by measurement: FMA is neutral on this hardware and doesn't
  fit QBE's two-arg instruction; 256-bit AVX is amd64-only (arm64 NEON
  is 128-bit). The accumulators are kept in memory, not registers:
  carrying them across the loop back-edge in a phi is unsafe under the
  `Kd` shortcut, since QBE resolves an un-coalesced phi with a 64-bit
  copy that drops a vector's high lane (the same reason a `Kd` vector
  can't be spilled). That last ~0.10s→0.06s step, like true spilling,
  needs a real 128-bit vector class. nbody (5.5x) still needs AoS→SoA
  for value-structs — a transform that keeps vectors inside one block.
- Collatz's residual gap is machine-level: shifted register operands
  (`add x,x,x,lsl #1`), `csinc`, bottom-tested loops.
- nbody's gap: the aggregate *return* copy, and struct field promotion.
- ARC pair elision and bounds-check elision are designed
  ([future](07-future.md)) but benchmark-gated: each needs a lab benchmark
  demonstrating its cost first, per the dependency policy.
