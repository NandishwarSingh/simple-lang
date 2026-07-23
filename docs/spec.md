# The Simple Language — design & specification

Simple is a general-purpose **systems** language with a high-level feel:
C-family syntax, native compilation, no garbage collector, no OOP —
and (eventually) memory safety and effortless concurrency.

This document has two parts: what exists **today** (v0.1), and the **design**
for where the language is going.

---

> **Decisions locked 2026-07-19:** statements end in `;` • conditions take
> parentheses `if (x) {` • mutability is `let` / `let mut` • memory safety
> (v0.3) is **ARC** — automatic reference counting, chosen over a
> borrow-checker design because its user-facing rulebook is empty •
> structs are plain named-field data (no methods, ever) • aggregates have
> **value semantics** — assignment copies, never aliases • array types are
> C-style `int[3]` • strings are **immutable**.

## Part 1 — v0.1, implemented today

### Lexical structure

- Comments: `// line` and `/* block */`
- Integer literals: `42`, `0xFF` (64-bit signed)
- String literals: `"hi\n"` with escapes `\n \t \r \\ \" \0`
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`
- Keywords: `fn let mut return if else while for in true false break continue struct` plus `chan spawn unsafe extern list map error import as` and the type names

### Types

| type      | meaning                                             |
|-----------|-----------------------------------------------------|
| `int`     | 64-bit signed integer                               |
| `float`   | 64-bit IEEE-754 double (`f32` is the 32-bit form)   |
| `bool`    | `true` / `false`                                    |
| `str`     | immutable string (heap; freed automatically by ARC) |
| `T[N]`    | fixed-size array of N values of T (bounds-checked)  |
| `list T`  | growable list (value semantics, copy-on-write)      |
| `map K V` | hash map, K is str or int; insertion-order iteration (v0.9) |
| `error`   | `ok` or a failure with a message (v0.8)             |
| `Name`    | a user-defined struct                               |

All aggregate types have **value semantics**: `let b = a;`, argument
passing, and `return` all copy. Two variables never share mutable data.

### Variables

```
let x = 10;         // immutable — reassignment is a compile error
let mut y = 0;      // mutable
let z: int = 5;     // optional type annotation (checked against initializer)
y = y + x;
```

Immutable-by-default is the first stone of the safety story: the compiler
already refuses `x = ...` unless `x` was declared `let mut`.

Shadowing is allowed in inner scopes; redeclaring in the same scope is an error.

### Functions

```
fn add(a: int, b: int) -> int {
    return a + b;
}

fn shout(msg: str) {        // no ->type means returns nothing
    print(msg);
}
```

- Parameter types are required; return type is optional (`void` if omitted).
- Functions may be called before their definition (whole-file scope).
- Non-void functions must return on **every** path — checked at compile time.
- Every program needs `fn main()` (returning nothing or `int`).

### Statements & expressions

```
if (cond) { ... } else if (cond2) { ... } else { ... }
while (cond) { ... break; ... continue; ... }
```

Operators, tightest first:

| precedence | operators            | operands | result |
|------------|----------------------|----------|--------|
| 1 (unary)  | `-` `!`              | int/bool | same   |
| 2          | `*` `/` `%`          | int      | int    |
| 3          | `+` `-`              | int      | int    |
| 4          | `<` `<=` `>` `>=`    | int      | bool   |
| 5          | `==` `!=`            | int/bool | bool   |
| 6          | `&&`                 | bool     | bool   |
| 7          | `\|\|`               | bool     | bool   |

`&&` and `||` short-circuit: the right side never runs if the left decides.

### Structs (v0.2)

```
struct Point { x: int, y: int }
struct Rect { a: Point, b: Point }
```

Plain named-field data — no methods, no inheritance, no constructors.
Literals name every field: `Point { x: 1, y: 2 }` (any order, none
missing). Access/assign with `p.x` / `r.a.y = 5;` (assignment requires the
root variable to be `let mut`). Structs nest; recursive structs are a
compile error (they'd be infinitely large — they become representable with
pointers/optionals, later). Struct returns give multiple return values.

### Arrays (v0.2)

`int[3]`, `Point[8]`, `int[2][3]` (2 rows of 3, indexed `g[r][c]`).
Fixed length, part of the type. Literals: `[1, 2, 3]`. **Every index is
bounds-checked at runtime**; out of range stops the program with
`runtime error: index 9 out of bounds (length 5)` and exit code 1.

### Strings (v0.2, tools added v0.7)

Immutable. `+` concatenates, `len(s)`, `==`/`!=` compare **contents**
byte-for-byte over the full length — never identity, and NUL-safe (a
`str` may hold any byte via `'\0'`, `s[i]`, or `substr`, so comparison
uses the length header + `memcmp`, not `strcmp`).
**v0.7 string tools:** `s[i]` reads the byte at i (an int 0..255,
bounds-checked); `substr(s, a, b)` returns the slice `[a, b)` (bounds
clamped); `str(n)` formats an int as decimal; `int(s)` parses a leading
integer (0 if none). Character literals `'A'` are int byte values, so a
lexer can write `c >= 'a' && c <= 'z'`.

### `for` loops (v0.2)

```
for (i in 0..n) { ... }     // i = 0 .. n-1; bounds evaluated once
```

The loop variable is freshly bound, immutable, and scoped to the body.
`break`/`continue` work as in `while`.

### Floating point (v0.65)

`float` (= `f64`) and `f32`. Literals are written with a decimal point or
exponent: `3.14`, `1.0`, `1.5e-3`. Arithmetic `+ - * /` and comparisons
work; `%` and bitwise ops do not (floats have no remainder or bit ops).
No implicit int<->float mixing — convert with `float(x)` / `int(x)`, and
literals adapt f64<->f32 in context just like int literals do.

**Determinism holds.** IEEE-754 `+ - * /` are correctly rounded, and QBE
does no FMA contraction, so float programs produce **byte-identical
output across arm64 and x86_64** — verified with the pure ray tracer.
The only platform-dependent floats are libm transcendentals, which are
`extern fn` and therefore visibly opt-in.

### Raw pointers, `null`, and the interop rule (v0.6)

Inside `unsafe` only:

- `*T` raw pointers, `&x` address-of, `*p` dereference, scaled pointer
  arithmetic.
- **Any pointer may stand in for any other.** `let p: *u32 = &bytes[0];`
  is allowed. This is the single place in the language where a conversion
  happens without being asked — decided 2026-07-21 on the grounds that
  `unsafe` already means the compiler has stopped vouching for you, and
  demanding cast ceremony there buys nothing while making C interop
  unusable.
- `null` — the null pointer. Needed because passing `NULL` is pervasive
  in C APIs, not occasional.

**The guardrail (principle, not a feature):**

> **The safe language never grows to accommodate C.** Anything required
> only for interop lives behind `unsafe`, where it costs the reader of
> ordinary Simple code exactly nothing.

This is what keeps "as simple as possible" honest while still being
"powerful enough to write an OS": chapters 1–10 of the book never mention
a pointer.

### Concurrency (v0.4)

```
fn worker(jobs: chan int, results: chan int) { ... }

let jobs = chan int(32);      // a channel: a mailbox with 32 slots
spawn worker(jobs, results);  // run on another OS thread (detached)
send(jobs, 5);                // blocks only when full
let v = recv(results);        // blocks only when empty
```

- `chan T` is a type (channels of any type, including structs and other
  channels); `chan T(n)` creates one with capacity n ≥ 1.
- Spawned functions must return nothing; results travel through channels.
  `main` exiting ends the program. Shutdown by sentinel values.
- **No shared memory, ever:** everything sent is the receiver's private
  copy (strings are deep-copied — or transferred outright when the
  runtime sees the sender's reference was unique). Channels are the one
  shared object and are internally synchronized. Data races are
  unrepresentable; the compiler adds zero new checks.
- Values still buffered when a channel's last reference drops are freed
  by the channel's element destructor — no leaks.

### Errors as values (v0.8)

```
fn parse_port(s: str) -> (int, error) {
    if (len(s) == 0) { return 0, fail("empty port"); }
    return int(s), ok;
}

let (port, e) = parse_port(input);
if (e != ok) { print(e.msg); return; }
```

- `error` is a built-in type. `ok` is the no-error value; `fail(msg)`
  builds an error from a `str`; `e.msg` reads its text (read-only, `""`
  on ok); compare with `==`/`!=`. Errors are ordinary values: struct
  fields, `list error`, `chan error`, `spawn` arguments all work.
- **Multiple return values** (general, not error-specific):
  `fn f() -> (T1, T2, ...)`, `return a, b;`, `let (x, y) = f();`.
  `_` discards one slot. Receiving is mandatory — a bare `f();` on a
  multi-return function does not compile, so failure can't be dropped
  by accident, only by a visible `_`.
- No `?`/`try` propagation sugar; passing up is an explicit
  `if (e != ok) { return 0, fail(context + e.msg); }`.
- `extern fn` cannot return multiple values (no C ABI story needed).
- Representation: an error is a nullable ARC string (null = ok);
  multi-returns fill a caller-provided buffer laid out like a struct.

### IO (v0.85)

```
fn main() -> int {
    if (argc() < 2) { print("usage: tool <file>"); return 2; }
    let (txt, e) = read_file(arg(1));
    if (e != ok) { print(e.msg); exit(1); }
    let out = write_file("copy.txt", txt);
    return 0;
}
```

- **Arguments, C convention:** `argc()` counts including the program
  path; `arg(0)` is that path, `arg(i)` is bounds-checked (traps like
  an array overrun). Each call returns a fresh string.
- **Stdin:** `input()` reads one line (newline stripped), `read_all()`
  slurps to EOF. Both return `""` at end of input.
- **Files:** `read_file(path) -> (str, error)` — receiving both is
  mandatory, so a missing file can't be mistaken for an empty one;
  `write_file(path, data) -> error`. Messages carry the path
  (`cannot open <path>`). Whole-file only; streaming/append may come
  later without breaking these.
- **Exit codes:** `main` may return `int` (absent = 0); `exit(n)` ends
  the program from anywhere, releasing all live values on the way out
  (the exit path is leak-clean by construction).
- Implementation detail: `main` is the C entry point and captures
  argc/argv into two globals at its prologue; readers are emitted
  runtime helpers over libc (`getchar`, `fopen`/`fread`/`fwrite`) — no
  new dependency, `cc` still links only libc.

### Maps (v0.9)

```
let mut ages = map str int;
ages["ada"] = 36;                      // insert or overwrite
if (has(ages, "ada")) { print(ages["ada"]); }
del(ages, "ada");                      // absent key: quiet no-op
for (k in ages) { ... }                // keys, in insertion order
```

- `map K V`: K is `str` or `int`; V is any type. `map str int` alone
  creates an empty map. Value semantics + COW, like `list`.
- Reading a missing key **traps** with `runtime error: key not found:
  <key>` — same philosophy as array bounds. `has(m, k)` checks first;
  the runtime caches a successful `has` so the following `m[k]` costs
  no second probe.
- **Iteration is insertion-ordered and deterministic** on every
  architecture (integer-only hashing: FNV-1a for str, splitmix64 for
  int). A loop iterates the map as it was when it began; body mutations
  COW away and are visible after the loop.
- Maps cannot be compared with `==` (compare contents yourself).
- Layout: Python-dict style — insertion-ordered entry array + compact
  i32 open-addressed index (calloc'd, biased sentinels), one-entry
  lookup cache in the header. `perf/bench/mapbench` (11.5M mixed ops
  across 7 languages): Simple beats Go's map, ~1.1x Rust's, ~1.8x C's
  hand-rolled table.

### Built-ins

- `print(x)` — prints an `int`, `bool`, or `str` followed by a newline.
- `len(x)` — length of a `str` (runtime) or array (compile-time constant).
- `send(ch, v)` / `recv(ch)` — blocking channel operations (v0.4).
- `fail(msg)` — builds an `error` from a `str` (v0.8); `ok` is its
  zero. (Shadowable like any name; not reserved words.)
- `argc()` / `arg(i)` / `input()` / `read_all()` / `read_file(p)` /
  `write_file(p, d)` / `exit(n)` — the IO surface (v0.85).
- `has(m, k)` / `del(m, k)` — map membership and removal (v0.9);
  `len` and `m[k]` indexing extend to maps.

---

## Part 2 — the design ahead

Ordered roughly by roadmap. Syntax below is the plan, open to change.

### v0.3 — memory: automatic reference counting (ARC) ✅ shipped

> **Decision 2026-07-19:** ARC replaces the earlier ownership/borrow-checker
> design. Reason: even a simplified borrow checker makes every user learn
> what a "move" is before they can pass a string around. ARC's user-facing
> rulebook is empty — and an empty rulebook is the project's whole thesis.
>
> **Shipped 2026-07-19.** Verified: 0 leaked bytes on the full example
> suite under `leaks --atExit`; strbuild memory 308 MB → 1.5 MB at equal
> speed (the O(1)-len header paid for the counting). The mimalloc
> experiment ran and was **rejected** — system malloc matched it.

No GC, no manual `free`, and nothing to learn:

```
fn keep(msg: str) { ... }

fn main() {
    let s = "hello";
    let t = s;       // just works
    keep(s);         // just works
    print(s);        // just works — no moves, no borrows, no annotations
}                    // s freed here, automatically, exactly once
```

How it works: every heap value (string, array, struct with heap fields)
carries a hidden reference count. Binding it to another name or passing it
along increments the count; a reference dying decrements it; at zero the
value is freed *immediately*. `int` and `bool` are plain copies — never
counted.

Properties:

- **Deterministic.** Frees happen at known program points — the instant the
  last reference dies. No collector, no pauses. (This is Swift's model.)
- **Bare-metal friendly.** The entire "runtime" is one counter beside each
  allocation and ~50 lines of retain/release code over `malloc`/`free` — a
  kernel swaps in its own allocator.
- **Optimizable.** The MIR stage (v0.5) will delete provably-redundant
  retain/release pairs at compile time — most of them, going by Swift and
  Nim. The steady-state cost trends toward zero.
- **Cheap by design** (analysis 2026-07-19): value semantics means only
  *strings* are refcounted; argument passing needs no counting at all;
  and counts are **non-atomic forever** because channels deep-copy
  strings on send (see v0.4). The string header also stores the length,
  making `len()` O(1) — ARC arrives as a net speedup for string code.
- **Cycles** are ARC's one classic leak — but a cycle requires a value that
  can reference itself, and until Simple has recursive struct references,
  cycles are *impossible by construction*. `weak` references ship together
  with recursive structs, not before.
- `let` / `let mut` mutability checking is unchanged — that safety already
  works today and is orthogonal to memory management.

Resolved with v0.2: aggregates got eager-copy **value semantics** (fixed
sizes make copies cheap). When ARC lands, immutable strings share their
buffer on copy (safe because immutable); if large-array copying ever shows
up in the perf lab, copy-on-write is the designed escape hatch.

### v0.4 — concurrency: as easy as Go ✅ shipped

> **Shipped 2026-07-19.** Lab verified: our emitted channel runtime beat
> hand-written C/C++ pthreads on the ping-pong benchmark (0.30 s vs
> 0.34 s) and tied Rust; 8-worker fan-out scaled 5.0x; both concurrency
> examples leak-free including values left buffered in dropped channels.
> Go's goroutine parking wins the blocking-heavy case (0.02 s) — the
> predicted, accepted cost of real OS threads.

> **Design finalized 2026-07-19** (all three by user decision): channels
> are **buffered only** (capacity ≥ 1 — "a mailbox with n slots", one
> mental model); **no `close()`** — shutdown uses sentinel values
> ("poison pills"), and `main` exiting ends the program like Go; **spawn
> takes void functions only** — results always travel through channels,
> and spawning a value-returning function is a compile error that points
> you at channels. Threads are detached OS threads (coarse-grained:
> thousands of spawns, not millions); a green-thread upgrade later
> changes zero syntax. Performance design: channel ops are
> lock+memcpy+unlock on a pre-allocated ring (~tens of ns uncontended);
> sends **move strings instead of copying when refcount == 1** (the
> dying-temporary case) — safe to check precisely because no heap memory
> is ever shared between threads. Channel handles are the one shared
> object; their refcount is guarded by the channel's own mutex, so string
> counts stay non-atomic everywhere.

```
fn worker(jobs: chan int, results: chan int) {
    while (true) {
        let j = recv(jobs);
        send(results, j * j);
    }
}

fn main() {
    let jobs = chan int(16);       // buffered channel, capacity 16
    let results = chan int(16);
    spawn worker(jobs, results);   // moves its arguments into the thread
    spawn worker(jobs, results);   // channels are shareable by design

    let mut i = 0;
    while (i < 10) { send(jobs, i); i = i + 1; }
    i = 0;
    while (i < 10) { print(recv(results)); i = i + 1; }
}
```

- `spawn f(args)` runs `f` concurrently. v1 uses one OS thread per spawn;
  a Go-style M:N green-thread scheduler can replace it later with **zero
  syntax changes**.
- Channels are the only shared state, and everything that crosses one is a
  **private copy**: scalars copy trivially, aggregates copy by value as
  always, and strings are **deep-copied on send** (decided 2026-07-19).
  No heap memory is ever shared between threads — data-race freedom is by
  construction, and refcounts stay non-atomic in every program, threaded
  or not. (Erlang's model: the copy per message is the price of full-speed
  everything-else.)
- `send`/`recv` block; `chan T(n)` buffers up to `n` values.

### v0.5 — MIR: the optimization layer ✅ shipped

> **Shipped 2026-07-19.** fib went from 1.70x C to **1.06x** (ties Rust,
> beats Go 1.5x); collatz 3.17x → 1.88x with every division instruction
> eliminated; parallel spawnwork 3.67x → **1.67x**; chanping ties C at
> 1.00x. The `--no-opt` skeleton was proven byte-identical to v0.4 before
> any pass landed, and differential testing is now a permanent gate.

> **Design finalized 2026-07-19** (user decisions): **MIR only** — HIR
> stays reserved until a feature needs desugaring; optimizations are
> **always on** (`--no-opt` exists for compiler debugging only — no
> -O level menu, there is one Simple and it is fast); passes land
> **one at a time with a lab checkpoint after each**, so every pass's
> contribution is public record.

```
Lexer → Parser → AST → type checking
      → MIR   a control-flow-graph IR owned by us; ARC ops are visible
      → passes: inlining, strength reduction, const-fold + DCE,
                then (benchmark-gated) ARC pair elision, bounds elision
      → QBE IR → QBE → assembly → linker → executable
```

The passes exploit what the language guarantees: the whole program is
always visible (no function pointers, no separate compilation), nothing
aliases (value semantics), `let` bindings are compile-time-known
constants, array lengths live in the types, and ARC's borrow/ownership
conventions are theorems the optimizer may assume. Full design in
[internals ch. 7](internals/07-future.md).

### Modules (v0.9)

```
// main.simp
import "lexer.simp";              // names usable unqualified
import "util/tally.simp" as t;    // or qualified: t.tally(...)

fn main() { let toks = tokenize(src); }
```

- `import "path";` at the top of a file (before any declaration) pulls
  in another file. Paths are relative to the importing file. Compilation
  starts at one root file and follows imports transitively; only
  reachable files are built. No manifest, no search path — the import
  graph is the build.
- **Flat namespace, everything visible** (no `pub`; user's choice
  2026-07-21). Imported names are called directly. Struct names are
  program-wide (a type means the same thing everywhere; a duplicate
  across files is an error). Function names may repeat across files but
  a call resolving to two visible files is an ambiguity error.
- `import "path" as x;` adds a qualifier: `x.fn(...)` names that file's
  function explicitly — for disambiguation or readability.
- Diamonds dedupe (canonical-path identity) and import **cycles are
  fine** (each file loads once). Diagnostics name the file the error is
  in, not the root.
- Implementation: the driver builds the whole-program AST from many
  files; sema assigns each function a program-unique link name (bare
  when unique, `name.f<id>` when it recurs — `.` can't appear in a
  Simple identifier), so single-file output is unchanged. Whole-program
  compilation and the whole-program optimizer are untouched — Simple was
  always compiled whole-world.

### The road to 1.0 (full roadmap, set 2026-07-19)

**v0.55 — own the backend** ✅ shipped 2026-07-19:
> QBE embedded (one compiler binary, brew dependency gone). Backend
> patches: ARM64 immediate operands (stock QBE used none — the biggest
> win), `csel` if-conversion (QBE's pass existed but was *disabled on
> ARM64*), flag fusion and `tst`. MIR gained "the guard is a proof"
> (a known-even value needs no division fixup) and struct-copy elision
> (an unassigned aggregate parameter is never copied — sound only
> because Simple has no aliasing). Results: collatz 1.88x → **1.35x** C,
> spawnwork 1.67x → **1.22x**, fib **1.03x**, primes **1.00x**.
> Loop rotation was implemented, measured, and **removed** (it hid
> diamonds from if-conversion). The new idiomatic-class benchmark
> **refuted** the value-semantics-beats-C hypothesis as measured
> (5.75x C) — but produced the copy-elision optimization. Full record in
> `perf/results/v0.55.md`.

Original plan:
- **Embed QBE into `simplec`**: its ~10k lines of MIT-licensed C99 move
  into `src/qbe/` and compile into the compiler binary. The brew
  dependency disappears; the subprocess handoff disappears; builds
  become fully reproducible. Dependency count: 2 → 1 (only `cc` remains,
  for assemble+link).
- **Patch the backend**: a real `sel` instruction emitting `csel` —
  MIR decides *when* (language knowledge), QBE provides *how*.
- **The machine-aware MIR bundle** (each lab-checkpointed):
  *if-conversion* — pure-arm if/else diamonds become selects, legal
  because Simple guarantees 0/1 booleans and side-effect-free arms;
  targets the entire remaining collatz (1.88x) / spawnwork (1.67x) gap,
  which is branch mispredictions; *block layout* for loop fallthrough;
  *multiply shift-adds* (`x*3` → `shl`+`add`); *loop rotation* (one
  fewer jump per iteration).
- **Beat-C attempts** (front 2 of the doctrine): deeper recursion
  inlining than clang (fib under 0.36 s), channel fast-path inlining
  (chanping under C). Plus the first **idiomatic-class benchmark**
  (n-body-style structs) where value semantics should beat pointer-C
  structurally.

**v0.6 — bare metal foundations.** ✅ shipped 2026-07-20
> Sized integers `i8..i64`/`u8..u64` (wrapping at their own width,
> naturally aligned in structs), the full bitwise operator set with
> **Go's precedence** (so `a & b == c` reads correctly, unlike C),
> explicit conversions `u8(x)` with integer *literals* adapting to
> context, `extern fn` C interop (including variadics), `unsafe { }`
> blocks with raw pointers `*T`, `&x`, `*p`, and scaled pointer
> arithmetic, plus `[value; N]` array fill. Optimizations: large copies
> now call memcpy (QBE unrolls `blit` without limit — a 320 KB copy
> OOM-killed the compiler), aggregate literals build in place, and
> **bounds-check elimination** using array lengths from the type plus
> immutable `for`-range variables (matmul: 8 checks → 0).

Original plan:
- sized integers `u8 u16 u32 u64 i32` (hardware registers, packets,
  file formats) and bitwise operators `& | ^ << >> ~` (today only the
  logical `&&`/`||` exist)
- `unsafe { }` blocks: raw pointers `*T`, address-of, deref, pointer
  arithmetic, `volatile_read`/`volatile_write` — every dangerous line
  greppable
- `extern fn` — call any C function/library; also the gateway to file
  I/O and OS APIs before we write our own

**v0.7 — programs that do things** (ergonomics, riding on `extern fn`):
- growable arrays (`list int`, chan-style built-in — no generics system) ✓
- string tools: indexing/slicing, `int↔str` conversion, parsing ✓
- floats (f64/f32, byte-identical cross-arch) ✓
- ~~program arguments, stdin/file reading, exit codes~~ → **moved to
  v0.85** (file IO wants v0.8's error design first)
- `box T` heap indirection: **dropped** — no cycles are possible under
  ARC and recursive data is expressed with a list + indices (arena
  pattern), which is also the self-hosting plan

**v0.8 — errors as values.** ✓ *Shipped 2026-07-21.* The design
discussion happened first (Result-style `T!` vs `T?` optionals vs
Zig-style error sets vs Go-style multi-returns) and the user chose
**Go-style `(T, error)`**:

- built-in `error` type: `ok` (the no-error value), `fail(msg)` makes
  one, `e.msg` reads its text (read-only; `""` on ok), compare with
  `e != ok`. An error is an ordinary value — struct fields, `list
  error`, `chan error` all work.
- **multiple return values**, general-purpose: `fn f() -> (T1, T2)`,
  `return a, b;`, `let (x, y) = f();`, `_` discards a slot. Results
  cannot be silently dropped: a bare `f();` on a multi-return function
  is a compile error — discarding takes an explicit `_`.
- no `?` propagation operator: passing an error up is a visible `if
  (e != ok) { return 0, e; }`. An abbreviation must earn its keep
  later, like every feature.
- representation: an `error` is a nullable string pointer (null == ok)
  reusing the ARC string machinery via null-safe wrappers; multiple
  returns reuse the aggregate-return path (caller-allocated buffer,
  struct-style slot layout). Neither adds a new runtime concept.
- explicitly rejected for v0.8: user-chosen error types (needs
  generics), error codes, exceptions (forever).

**v0.85 — IO: talking to the outside.** ✓ *Shipped 2026-07-21.* The
unbuilt half of the old v0.7 plan, deliberately sequenced after v0.8 so
file IO could use real errors. API chosen by the user (via guided
questions): **`argc()`/`arg(i)`** (C convention, not `args() -> list
str`), **`input()`/`read_all()`** with `""` at EOF, **`read_file ->
(str, error)` / `write_file -> error`**, `exit(n)` + `main -> int`.
See "IO (v0.85)" in Part 1. The self-hosting blocker is gone: a Simple
program can now read source files and write output files.

**v0.9 — hash maps, then modules.** (Reordered 2026-07-21.) Phase 1
✓ *shipped 2026-07-21*: `map` as a built-in collection alongside
`list` — implemented, then optimized in six lab-measured steps
(inline hashing/compares in find/put, i32 calloc'd index with biased
sentinels, a pointer-identity lookup cache — sound because the map
retains its keys, so identity to a live key proves content — an inline
cache fast path at every m[k] read site, a put fast path for
read-modify-write, and entries-only realloc growth: 1.09→0.79s), then
the full lab re-run with **zero regression**. mapbench (11.5M ops):
Simple 0.79s beats Go 0.87, within 10% of Rust 0.72; C 0.43. Phase 2 (next): multiple
files, imports, visibility.
Required before any program the size of an OS can be organized. (Ends
the single-file era; the whole-program optimizer keeps working —
compilation stays whole-world by design.)

**v0.95 — the vectorization engine.** *(In progress.)* A real
auto-vectorization pass: recognize vectorizable loops (float first —
element-wise maps, saxpy shapes, reductions) and emit SIMD/NEON instead
of scalar code, on arm64 (NEON) and amd64 (SSE/AVX). Largest known perf
gap was: vectorstorm at **~5.6x C** because clang vectorized and Simple
didn't (91% of its time in the two vectorizable phases). The engine
below closed it to **2.12x C**.

Decisions locked (2026-07-21): cross-arch determinism is the bar (FMA and
vector-order reductions allowed); the canonical float-reduction tree order
is fixed now; build the general QBE engine (no kernel shortcut).

Milestones — **all core milestones landed 2026-07-22**: (1) legality
analysis (`--vec-report`, exploits no-aliasing so legality is a local
per-lane check); (2) QBE v128 backend — solved *without* the pervasive
K-class change by typing vectors as `Kd` (they reuse the FP register
file) and putting the 128-bit width entirely in the emitter (new
`vload/vstore/vfadd/vfsub/vfmul/vfdiv/vdup` ops; re-tuned the lexer
perfect hash for the new keywords); (3) NEON (arm64, `v.2d`) + AVX
(amd64, 3-operand `vmulpd` — SSE 2-address would drop the high lane on
a reused value); (4) spill made impossible by a register-pressure gate
(a Kd-typed vector can't be spilled soundly, so large loops stay
scalar); (5) codegen emits strip-mined 2-lane loops (+scalar remainder)
for element-wise f64 loops over 1D stack arrays with constant in-range
bounds; (6) **reductions** — a single-statement f64 reduction
(`s = s + a[i]`, `d = d + a[i]*b[i]`, `p = p * (1+a[i])`) accumulates
into a 2-lane vector, then a horizontal combine folds the lanes. The
accumulator is initialised with a vector op, never a scalar store: a
scalar store to its fixed slot would let load-forwarding hop the
`vstore` and return stale data. Reassociating the reduction across
lanes is licensed by the determinism decision above. **Result:
vectorstorm ~5.6x C → 2.12x C (1.41s→0.53s, 9-run lab median, element-wise loops unrolled); a dot-product reduction
runs 0.10s (four independent accumulators break the latency chain),
about 4x faster than C `-O2`/`-O3 -march=native` (0.38s) — C cannot
reassociate a float reduction without `-ffast-math`, so it stays scalar
while Simple vectorises *and* unrolls it. Every result is bit-identical
across NEON, AVX, and scalar** — cross-arch determinism held. Guarded
by the opt/no-opt and arm64/amd64 differentials on every example.

Two dead ends were ruled out by measurement, not assumption: **FMA** is
neutral-to-negative on this hardware (C is no faster with it) and does
not fit QBE's two-argument instruction, and **256-bit AVX** is amd64
only — arm64 NEON is 128-bit — so it cannot help the M-series host.
Integer lanes would *not* unlock the integer matmul either: it is 64-bit
and NEON has no 64-bit-lane multiply, so not even clang vectorises it.

The reduction accumulators live in memory (a `vload`/`vstore` per
iteration), which caps the dot product at ~0.10s versus C's ~0.06s.
Register-resident accumulators via phi nodes were tried and are unsafe
under the `Kd` shortcut: when QBE cannot coalesce a phi it resolves it
with a copy, and a copy of a `Kd` vector is a 64-bit move that drops the
high lane — wrong whenever coalescing fails. Closing that last gap
therefore needs the real 128-bit vector class, deferred as high-risk.
Remaining (future): f32 lanes, lists, and AoS→SoA for value-structs
(the nbody win) — all of which keep vectors inside a single block.

Prior groundwork: arm64 indexed-addressing (matmul) was attempted at
Phase 0 and reverted (a regalloc coalescing bug in slot-base
materialisation — see the NOTE in arm64/isel.c); 16-byte array alignment
shipped.

**Deferred past 1.0 — freestanding: the OS door.** `--freestanding`
(no libc, no runtime), custom entry point, panic handler, ARC over a
user-provided allocator, volatile MMIO, linker scripts, the
kernel-in-QEMU demo. Skipped for now (decision 2026-07-21): speed
(v0.95) and self-hosting earned the pre-1.0 slots.

**v1.0 — polish and proof.** Spec freeze, book completion, the
benchmark-gated passes (ARC pair elision, bounds elision), possibly the
green-thread M:N upgrade (zero syntax changes; chanping's Go gap closes).
Long-term dream, unlocked by v0.7+: **self-hosting** — rewrite `simplec`
in Simple.

Continuous tracks alongside every milestone: the perf lab grows a
benchmark per feature; both books update in the same commit as any
change; every dependency faces the lab before admission.

**Toolchain policy** (revised 2026-07-19 by user decision): `simplec` is
one binary with QBE embedded (v0.55); **`cc` stays as the one permanent
external dependency** — it assembles and links, it's on every dev
machine, and writing our own assembler/linker is explicitly a non-goal
(Zig and Go each spent years there for benefits we don't need). The OS
path needs nothing more: kernels are built the way Linux is —
`cc -ffreestanding -nostdlib -T kernel.ld` with a cross-target clang —
no libc, no signing, boots in QEMU.

**The beat-C doctrine** (set 2026-07-19 — the goal is to *beat* C, not
tie it), three fronts:
1. *Parity on identical scalar code is the floor, not the goal* — same
   algorithm compiled to the same instructions runs at the same speed;
   v0.55's if-conversion finishes this front, then we stop chasing it.
2. *Outright wins where clang under-delivers*: deeper recursion
   inlining than clang's budget (fib), and inlining our channel/runtime
   fast paths into callers — cross-boundary inlining C's libpthread
   calls can never receive (chanping).
3. *Structural wins, systematically*: value semantics = zero aliasing
   (C reloads around every pointer store; we keep values in registers),
   whole-program compilation always (real C projects don't LTO), and
   effortless parallelism. Measured via a second, honestly-labeled lab
   class: **idiomatic benchmarks** — same task, natural code per
   language — alongside the identical-algorithm class, starting with a
   struct-heavy n-body-style simulation in v0.55.

### Performance & dependency policy

Decided 2026-07-19 after evaluating a menu of acceleration technologies
(ISPC/SIMD libraries, OneTBB/libuv/Tokio, BLAS/LAPACK, ArrayFire/CUDA,
jemalloc/mimalloc):

- **The perf lab drives optimization work, not articles.** A dependency
  enters the compiler or runtime only when a benchmark in `perf/` proves it
  pays — and never in freestanding mode.
- Our measured v0.1 gaps (fib 1.7x C, collatz 3.2x C) are missing
  *inlining* and *strength reduction*: small, dependency-free MIR passes.
- **Accepted, for later:** swapping the allocator to **mimalloc** at v0.3 —
  ARC funnels every allocation through `simple_rc_alloc`, making the swap a
  one-line change; ship on system malloc first, benchmark, then decide.
- **Rejected:** embedded threading runtimes (v0.4 is ~200 lines of
  pthreads + channels; TBB/libuv would bloat the runtime or change our
  blocking semantics), native BLAS/matrix mapping (numerics-DSL feature —
  users get BLAS via `extern fn` C interop instead), GPU offload (research
  project, orthogonal to the OS goal), and SIMD backends (QBE has no vector
  types; our gaps are scalar; a future LLVM backend would bring
  auto-vectorization for free if ever justified).

### Non-goals (permanently)

- Classes, inheritance, method dispatch
- Exceptions (errors will be values)
- A garbage collector
- Macros / metaprogramming (at least for a long time)
- Lifetime annotations — if a feature needs them, the feature changes
