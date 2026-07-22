# 1. Pipeline Overview

## The stages

| stage      | file              | input          | output              |
|------------|-------------------|----------------|---------------------|
| lexer      | `src/lexer.cpp`   | source string  | `vector<Token>`     |
| parser     | `src/parser.cpp`  | tokens         | `Program` (AST)     |
| sema       | `src/sema.cpp`    | AST            | AST + type annotations |
| codegen    | `src/codegen.cpp` | typed AST      | QBE IR (string)     |
| qbe        | external binary   | `.ssa` file    | `.s` assembly       |
| cc         | system compiler   | `.s`           | linked executable   |

`main.cpp` drives the sequence and shells out for the last two stages.

## The target pipeline (from the design)

```
Lexer → Parser → AST → HIR → type check → MIR → our optimizations → QBE IR → QBE → asm → link
```

v0.1 goes **AST → QBE directly** — the HIR and MIR stages don't exist yet.
This is deliberate sequencing, not a change of plan: a thin end-to-end
pipeline first, then insert layers where they pay for themselves.
[Chapter 7](07-future.md) specifies what HIR and MIR will be.

## Why QBE (and not LLVM, or raw assembly)

- **QBE** is ~10k lines of C. Its IR is small enough to learn in an
  afternoon, its output is real optimized machine code, and it does the two
  genuinely hard backend jobs — register allocation and instruction
  selection — so we don't have to. It supports x86-64 and ARM64 including
  the Apple variants.
- **LLVM** would give stronger optimization and more targets at the cost of
  a giant dependency and an API that dominates the project. Wrong trade for
  a language whose implementation should be understandable.
- **Raw assembly** would teach the most and ship the least.

The interface to QBE is just text: we write `.ssa`, run `qbe`, get `.s`.
No linking against anything — which also means the backend is swappable
later without touching the language.

## Error handling contract

Every stage reports problems by throwing `CompileError{line, msg}`
(`src/diag.hpp`, via the `err()` helper). `main.cpp` catches it and prints:

```
file.simp:LINE: error: MSG
```

One error per run: the first problem stops compilation. Multi-error
reporting with recovery is possible later, but first-error-wins keeps every
stage free of "what if the input is already broken" code — after any stage
succeeds, the next stage may assume a well-formed input. That invariant is
used heavily (e.g. codegen never re-checks types).

## Memory conventions

The AST owns its nodes via `std::unique_ptr`; `Program` owns the `Function`s.
Sema and codegen hold raw non-owning pointers/references into it, valid
because the AST outlives both. Nothing is heap-allocated in the compiler
outside the AST and strings.
