# Simple Compiler Internals

*The architecture book: how `simplec` works, file by file, decision by
decision.*

This is for anyone hacking on the compiler itself (including future us).
The teaching book for the *language* is [here](../book/README.md).

> **Maintenance rule:** any change to the compiler updates the matching
> chapter here, in the same commit. Stale internals docs are worse than none.

## Contents

1. [Pipeline Overview](01-pipeline.md) — the stages and what flows between them
2. [The Lexer](02-lexer.md) — `src/lexer.cpp`, `src/token.hpp`
3. [The Parser and AST](03-parser-and-ast.md) — `src/parser.cpp`, `src/ast.hpp`
4. [The Type Checker](04-type-checker.md) — `src/sema.cpp`
5. [Code Generation](05-codegen.md) — `src/codegen.cpp`, QBE IR
6. [The Driver](06-driver.md) — `src/main.cpp`, artifacts, exit codes
7. [Future Architecture](07-future.md) — HIR/MIR, ownership checking, runtime

## The ten-second tour

```
                     ┌──────────── simplec (C++17, src/) ────────────┐
.simp text ──lexer──▶ tokens ──parser──▶ AST ──sema──▶ typed AST ──codegen──▶ QBE IR
                                                                                │
                        executable ◀──cc (assemble+link)── .s ◀──qbe── .ssa ◀───┘
```

- One pass per stage, each in its own file, communicating through plain data
  structures. No global state.
- Errors are thrown as `CompileError{line, msg}` from any stage and caught
  once in `main` — first error wins, compilation stops.
- The compiler is deliberately boring C++: tagged structs instead of class
  hierarchies, `std::string` IR emission instead of builder APIs. Boring is
  readable, and readable is the point at this stage.

## Building & testing

```
make          # build ./simplec
make test     # compile+run every examples/*.simp, diff against tests/expected/
```

Add a feature → add an example exercising it → `./simplec examples/x.simp -o build/x && ./build/x > tests/expected/x.txt` → `make test` guards it forever.
