# The Simple Programming Language

*The official book for learning Simple.*

Simple is a small systems programming language that compiles straight to native
machine code. This book teaches it from zero — if you've written a little code
in any language, you can follow along.

> **Book status:** tracks the compiler at **v0.9 (maps + modules)**. Every code
> sample in chapters 1–15 compiles and runs today. Chapter 16 previews features that
> are designed but not built yet, and says so clearly.
> This book is updated in the same commit as any language change.

## Contents

1. [Introduction](01-introduction.md) — what Simple is and why it exists
2. [Getting Started](02-getting-started.md) — install, compile, run
3. [Variables and Types](03-variables-and-types.md) — `let`, `let mut`, `int`, `bool`, `str`
4. [Operators and Expressions](04-operators-and-expressions.md) — arithmetic, comparison, logic
5. [Control Flow](05-control-flow.md) — `if`, `else`, `while`, `break`, `continue`
6. [Functions](06-functions.md) — parameters, returns, recursion
7. [Understanding Compile Errors](07-understanding-errors.md) — the compiler is on your side
8. [Under the Hood](08-under-the-hood.md) — watch your code become machine code
9. [Structs, Arrays, and Strings](09-structs-arrays-strings.md) — data with shape *(new in v0.2)*
10. [Concurrency](10-concurrency.md) — `spawn`, channels, and racing-free threads *(new in v0.4)*
11. [Bare Metal](11-bare-metal.md) — sized ints, bitwise ops, `unsafe`, C interop *(new in v0.6)*
12. [Errors as Values](12-errors-as-values.md) — `error`, `fail`, multiple returns *(new in v0.8)*
13. [Talking to the Outside](13-talking-to-the-outside.md) — args, stdin, files, exit codes *(new in v0.85)*
14. [Maps](14-maps.md) — hash maps with insertion-order iteration *(new in v0.9)*
15. [Modules](15-modules.md) — splitting a program across files with `import` *(new in v0.9)*
16. [The Road Ahead](16-the-road-ahead.md) — the path to an OS

For how the compiler itself works, see the [internals book](../internals/README.md).
For the formal design document, see the [language spec](../spec.md).
