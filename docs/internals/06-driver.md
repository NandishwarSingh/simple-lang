# 6. The Driver

**File:** `src/main.cpp`

The un-glamorous glue: CLI parsing, file I/O, subprocess orchestration,
error printing. Kept dumb on purpose.

## Command forms

```
simplec file.simp [-o out]     # compile
simplec run file.simp          # compile + exec, forwarding the exit code
simplec file.simp --emit-ssa   # print IR to stdout, build nothing
simplec file.simp --tokens     # print token stream, build nothing
```

Flag parsing is a hand-rolled loop; `run` is recognized only as the first
argument. Anything unrecognized prints usage and exits 1.

## Artifact naming

Output defaults to the source path minus `.simp` (so
`examples/fib.simp → examples/fib`); `-o` overrides. Two intermediates are
written next to the output and **deliberately kept**:

```
<out>.ssa   the QBE IR we generated
<out>.s     the assembly QBE generated
<out>       the executable
```

Keeping them is a feature (the book tells learners to read them). A
`--quiet`/cleanup flag can come later.

## Multi-file loading (v0.9)

The driver, not the parser, owns the module graph. Given the root
`.simp`, it does a worklist walk:

- Each file is parsed on its own; the parser returns that file's
  `imports` list plus its decls (untouched — no cross-file knowledge in
  the parser).
- Files are deduplicated by **canonical path** (`realpath`), so a
  diamond (two files importing the same third) loads it once, and an
  import **cycle** simply resolves — the second visit finds the file
  already assigned an id.
- Import paths are resolved **relative to the importing file's
  directory**, so a package moves as a unit.
- Every decl is tagged with its `fileId` as it's merged into one
  whole-program `Program`. `files[0]` is always the root.

Only files reachable from the root are compiled — a `.simp` sitting in
the directory that nobody imports is never read. There is no manifest,
no search path, no build system: the import graph *is* the build.

`activeFile` tracks which file is being parsed so a parse error names
the right file; sema does the same for later stages (below).

## Cross-compilation (v0.6)

`--target <name>` picks a QBE backend (`arm64`, `arm64_apple`,
`amd64_sysv`, `amd64_apple`, `rv64`) instead of the host default, and the
driver passes the matching `-arch` to `cc` so the assembler and linker
agree. Because MIR and all optimization passes are target-independent,
cross-compiling exercises the same pipeline — only the final backend
differs.

Verified across arm64 macOS (native), x86_64 macOS (cross-compiled,
executed via Rosetta), and x86_64 Linux (compiler rebuilt with gcc under
QEMU): identical output from all golden tests and all 14 benchmarks,
and the optimized-vs-`--no-opt` differential matches on every platform.
That covers two architectures, two OSes, two host compilers, and both
x86_64 ABIs.

## The external stages

```cpp
qbe -o <out>.s <out>.ssa      // IR → assembly
cc <out>.s -o <out>           // assemble + link against libc
```

Both run via `std::system` with single-quoted paths; failures print a hint
(`brew install qbe`) rather than raw exit codes. `cc` is used instead of
invoking `as`+`ld` directly because it knows the platform's linking
incantations (SDK paths on macOS, crt files on Linux) — free portability.

Exit-code plumbing: `runCmd` decodes `system()`'s status with
`WIFEXITED`/`WEXITSTATUS`; `simplec run` forwards the program's real exit
code, so `.simp` programs compose in shell scripts.

## Error surface

All `CompileError`s from any stage land in one `catch` here and print
`file:line: error: msg` to stderr. Unopenable input files are caught before
lexing. Nothing else in the compiler touches stderr or exits.
