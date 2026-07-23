# Simple

A small systems programming language that compiles to real native executables.
Files end in `.simp`.

```
// examples/hello.simp
fn main() {
    print("hello, world");
}
```

```
$ ./simplec run examples/hello.simp
hello, world
```

## Design goals

- **Easy, high-level syntax** — if you can read C or JavaScript, you can read Simple
- **No OOP, no magic** — functions and data, nothing else
- **Native binaries** — no VM, no interpreter, no garbage collector
- **Memory that manages itself** — ARC frees heap values the instant their
  last user is done: no GC pauses, no leaks (`leaks` verified), nothing to learn
- **Concurrency as easy as Go** — `spawn` + channels, and data races are
  *unwritable*: threads never share memory, by construction
- **Bare-metal capable** *(planned)* — `unsafe` blocks, raw pointers, freestanding mode

## Documentation

- **[The Simple book](docs/book/README.md)** — learn the language from zero (start here)
- **[Compiler internals](docs/internals/README.md)** — how `simplec` works, stage by stage
- **[Language spec](docs/spec.md)** — the formal design document and roadmap
- **[Performance lab](perf/README.md)** — Simple vs C, C++, Rust, Go, Zig, Swift — run `make bench` after every version; reports live in `perf/results/`
- **[Bootstrapping analysis](docs/bootstrapping.md)** — what self-hosting requires and when it becomes possible
- **[Optimization plan](docs/optimization-plan.md)** — the ranked work queue: every known optimization scored by effect, effort, and what Simple's design makes cheap

Both books are updated in the same commit as any language or compiler change.

## The pipeline

```
source (.simp)
  → Lexer                     src/lexer.cpp      ✅
  → Parser → AST              src/parser.cpp     ✅
  → Type checking             src/sema.cpp       ✅
  → MIR + optimization passes src/codegen.cpp    ✅
    (inlining, strength reduction, const-fold + DCE, auto-vectorization
     — always on; --no-opt to disable)
  → QBE IR                    src/codegen.cpp    ✅
  → QBE → assembly            (external: qbe)    ✅
  → assemble + link           (via cc)           ✅
  → native executable
```

[QBE](https://c9x.me/compile/) is a small, readable compiler backend (~9k lines,
MIT) that does register allocation and instruction selection. Since v0.55 it is
**embedded in `simplec`** (`src/qbe/`) rather than shelled out to, and carries our
own ARM64 patches — immediate operands, `csel` if-conversion, `tst` — all marked
`simple(v0.55)`. `simplec` is one binary; `cc` assembles and links the output.

## Building

Requires a C++17 compiler and `make`. The QBE backend is embedded in
`src/qbe/` (MIT) and compiles into `simplec`, so there is nothing else to
install — `cc` is used to assemble and link the programs you compile.

```
make            # builds ./simplec
make test       # compiles + runs every example, checks output
```

## Using the compiler

```
./simplec program.simp            # compile → ./program (plus program.ssa, program.s)
./simplec program.simp -o out     # choose output name
./simplec run program.simp        # compile and run immediately
./simplec program.simp --emit-ssa # print the generated QBE IR
./simplec program.simp --no-opt   # skip the optimizer (debugging)
./simplec program.simp --tokens   # print the token stream (debug)
./simplec program.simp --target amd64_sysv    # cross-compile
```

Targets: `arm64`, `arm64_apple`, `amd64_sysv`, `amd64_apple`, `rv64`.

## Platform support

Verified on **arm64 and x86_64**, macOS and Linux, built with both clang
and gcc — the full test suite and all 14 benchmarks produce identical
results on every combination:

| host | build | tests | benchmarks |
|------|-------|-------|------------|
| macOS arm64 (native) | clang | pass | all correct |
| macOS x86_64 (cross-compiled, `--target amd64_apple`) | clang | pass | all correct |
| Linux x86_64 (native, under QEMU) | gcc 13 | pass | all correct |

The intermediate files are kept on purpose — read `program.ssa` (QBE IR) and
`program.s` (assembly) to watch your code descend to the metal.

## What works today (v0.95)

- types: `int`, sized ints (`i8`–`i64`, `u8`–`u64`), **`float`/`f32`**
  (real IEEE-754), `bool`, `str`, structs, fixed arrays `int[5]`, `list T`,
  `map[K]V`, channels, raw pointers `*T`
- **bare metal** — bitwise operators, `unsafe { }` blocks with raw
  pointers and pointer arithmetic, `extern fn` to call any C library
- **structs** — plain named-field data, nesting, struct returns (no OOP, ever)
- **arrays** — fixed-size, value semantics, `len()`, **runtime bounds checking**
- **lists** — `list T`, growable (`push`/`pop`/`len`, indexing); value
  semantics via copy-on-write; hold anything, even other lists; ARC-freed
- **maps** — `map[K]V` with int or string keys, **insertion-order iteration**,
  a from-scratch open-addressing table emitted as QBE *(v0.9)*
- **strings** — `+` concat, O(1) `len()`, `==` **content** compare, `s[i]` byte
  access, `substr`, `str(n)`/`int(s)` conversion, char literals `'A'`;
  **freed automatically by ARC** (leak-checker verified)
- **errors as values** — a built-in `error` type, `fail("msg")`, and
  first-class **multiple returns** `fn f() -> (int, error)`; no exceptions,
  no hidden control flow *(v0.8)*
- **I/O** — `argc()`/`arg(i)`, `input()`/`read_all()`, `read_file`/`write_file`,
  `exit(n)` — enough to read source and write output *(v0.85)*
- **modules** — split a program across files with `import` *(v0.9)*
- **concurrency** — `spawn` for threads, `chan T(n)` + `send`/`recv` for
  communication; no shared memory, no data races, no new error messages
- `let` / `let mut` — variables are **immutable by default**; assignment copies
- functions, recursion, parameters, return values
- `if` / `else if` / `else`, `while`, `for (i in 0..n)`, `break`, `continue`
- full arithmetic, comparison, and short-circuit logical operators
- `print(...)` for ints, floats, bools, and strings
- real compile errors with line numbers — type mismatches, immutability
  violations, missing returns, unknown fields, recursive structs
- **an optimizing compiler** — inlining, strength reduction, dead-code
  elimination, **and an auto-vectorizer** that emits 128-bit SIMD
  (NEON / SSE-AVX) for element-wise float loops and reductions — cross-arch
  bit-identical, always on (`--no-opt` to disable). See **Performance** below.

## Performance

Simple aims to **beat C**, and the [performance lab](perf/README.md) keeps it
honest: 14 benchmarks, identical algorithms, run against C, C++, Rust, Go, Zig,
and Swift (median of 9 runs, `-O2`-class flags, `perf/results/`).

On an Apple M5, Simple **beats or ties C on 6 of 14** and posts a **median of
1.27× C**:

| benchmark | Simple vs C | note |
|-----------|:-----------:|------|
| chanping | **0.88×** | channel round-trips — faster than C |
| gauntlet | **0.93×** | a brutal mixed workload *built to expose value-semantics costs* — still under C |
| primes / sieve / bitops | **1.00×** | ties C |
| vectorstorm | 2.12× | float SIMD — was ~5.6× before the auto-vectorizer landed |
| nbody | 5.50× | the last big gap (needs array-of-structs → struct-of-arrays) |

Every benchmark produces a **byte-identical result in all seven languages**, and
the auto-vectorizer's output is **bit-identical across NEON, AVX, and scalar** —
determinism is a guarantee, not an accident. The full table and methodology are
in [`perf/`](perf/README.md); the ranked optimization backlog is in
[`docs/optimization-plan.md`](docs/optimization-plan.md).

## Two showcase apps

**`examples/raytracer_pure.simp`** — the same ray tracer in **pure
Simple**: no C, no `extern`, no libraries. Real floating-point math
(v0.65) instead of fixed point, four worker threads, and a P3 PPM written
to stdout with only the built-in `print`.

```
./simplec examples/raytracer_pure.simp -o rtpure && ./rtpure > out.ppm
```

Renders in ~0.2s, **zero leaks**, deterministic across runs — and
**byte-identical between arm64 and x86_64**, because IEEE-754 basic
arithmetic is reproducible.

**`examples/raytracer.simp`** — the same ray tracer with reflections,
shadows and an orbiting camera, rendered by four worker threads, shown
live in an SDL2 window, and saved as a PPM image through C's `stdio` at
the end. Simple has no floating-point type, so **every number is 16.16
fixed point** — multiplication, division, square roots and vector
normalisation are all built from integer arithmetic and shifts.

```
sh tools/build_raytracer.sh && ./build/raytracer     # needs: brew install sdl2
```

Measured: 240 frames, **28.8 million primary rays** (plus reflection and
shadow rays), **7 million rays/sec**, **zero leaked bytes** — and the
four-thread render is **bit-for-bit deterministic across runs**, because
output depends on which scanline each worker computes, never on timing.
That is the no-shared-memory guarantee showing up as reproducibility.

## The plasma app


`examples/plasma.simp` is a single file that opens a **real graphical
window** and renders an animated plasma field with a bouncing particle
swarm, using every feature the language has:

```
./simplec examples/plasma.simp --link SDL2 --libdir /opt/homebrew/lib -o plasma
./plasma                                        # needs: brew install sdl2
```

A pool of four worker threads computes the image one scanline at a time
and streams finished rows back through a channel; the main thread blits
them into a pixel buffer and hands it to SDL2 over raw pointers. Sized
integers and bit-packing for colour, nested structs and value semantics
for the particles, recursion for the noise octaves and for
integer-to-string, `unsafe` only where pointers actually cross into C.

Measured: **~1100 fps headless** — 288,000 channel round-trips across
four threads — with **zero leaked bytes** under `leaks`. Windowed it
swings between 60 and 300 fps, which is the macOS compositor scheduling
the GPU rather than anything in the language; the headless number is the
one that measures Simple.

## Repository layout

```
src/        the compiler (C++17)
src/qbe/    the embedded backend (C99, MIT, with our ARM64 patches)
examples/   sample .simp programs
tests/      test runner + expected outputs
docs/       the book (docs/book), compiler internals (docs/internals), spec
perf/       the performance lab — benchmarks vs C, C++, Rust, Go, Zig, Swift
```
