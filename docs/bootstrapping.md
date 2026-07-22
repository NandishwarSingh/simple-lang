# Bootstrapping Simple

*Written 2026-07-20, before committing to the path to v1.*

Self-hosting — rewriting `simplec` in Simple — is the traditional proof
that a language is real. This document works out what it would actually
take, so the v0.7–v0.9 milestones can be shaped to arrive there rather
than needing a detour later.

## What the compiler actually needs from the language

Going feature by feature through the existing C++ compiler (~5,500 lines
across lexer, parser, sema, MIR, codegen, driver):

| need | used for | status |
|------|----------|--------|
| structs, arrays, strings | tokens, AST nodes, types | ✅ have |
| recursion | recursive-descent parsing, tree walks | ✅ have |
| integer types, bitwise | offsets, layout, flags | ✅ have (v0.6) |
| `extern fn` → C | `malloc`, odd OS corners | ✅ have (v0.6) |
| **file I/O, args, exit codes** | reading sources, writing output, tool behavior | ✅ have (v0.85, native) |
| **growable lists** | token stream, statement lists, block lists | ✅ have (v0.7) |
| **string building & inspection** | error messages, IR emission, identifiers | ✅ have (v0.7) |
| **recursive data** (indices into a list) | the AST itself | ✅ via v0.7 lists |
| **errors as values** | `CompileError` propagation | ✅ have (v0.8) |
| **hash maps** | symbol tables, string interning, layouts | ✅ have (v0.9) |
| **modules** | 12+ source files | ✅ have (v0.9) |

Nothing on that list is exotic, and nothing needs a feature we have
rejected. **The roadmap produces every one of them before self-hosting** —
**every row above is now checked off** — errors, IO, hash maps, and
modules all shipped. The language has everything the existing C++
compiler needs from it; the self-hosting port is now a matter of labour,
not missing features.

## The one genuine design problem: recursive data

An AST is a tree of nodes that reference other nodes. Simple *forbids*
`struct Node { next: Node }` because it would be infinitely large. Every
real compiler needs a tree.

**Decision (2026-07-21): no `box`, no heap indirection, no pointer-linked
recursive data.** Trees are built the *arena* way — a flat `list` plus
integer indices:

```
struct Expr {
    kind: int,
    ival: int,
    lhs: int,       // index into the expr list, or -1 for none
    rhs: int,
}
// the whole AST is one `list Expr`; children are indices into it
```

Why this over `box`:

- **No cycle weakness.** ARC's one flaw is reference cycles. With no
  reference type in the safe language, a cycle is *unconstructible* —
  "no leaks, ever" stops being conditional. The only references in safe
  Simple are list indices, which are just ints.
- **It is how fast compilers already do it.** Contiguous nodes,
  no pointer chasing, freed all at once. Data-oriented, cache-friendly.
- **It keeps the language smaller.** No `box`, no `weak`, no
  strong-vs-weak reasoning — the exact cognitive load ARC was chosen to
  avoid.

The cost is threading the node list through the tree-walking functions
(each takes the `list Expr` as a parameter). Verbose, but every function
is explicit about what it reads, and real data-oriented compilers accept
the same trade. **Self-hosting needs indices-over-lists, not `box`** —
so it needs only v0.7's lists, nothing more.

## Why bootstrap at all

Beyond the milestone, it is the most demanding test the language could
face:

- **~5,500 lines of real, gnarly code** — string manipulation, recursion,
  tables, error paths. Every ergonomic gap shows up immediately.
- **It benchmarks itself.** "Compiler compiles compiler" is a workload we
  care about, unlike synthetic loops.
- **It proves the safety story.** If ARC, bounds checks and no-aliasing
  survive a compiler, they will survive most things.
- **It is a forcing function for the standard library.** You cannot fake
  your way through file I/O and hash maps.

## Realistic staging

Do **not** attempt a full rewrite. Stage it so each step is independently
useful and independently verifiable:

1. **v0.7–v0.9 as planned** — lists, strings, errors, modules (no box).
2. **Port the lexer first** (~250 lines). It needs only strings, lists
   and an enum. Verify by tokenising every `.simp` file in the repo and
   diffing against the C++ lexer's `--tokens` output. A weekend, and it
   proves the ergonomics.
3. **Port the parser** (~600 lines). The AST is a `list Expr` with
   index-linked children. Verify against the C++ parser by comparing IR.
4. **Port sema, then MIR/codegen.** By this point the language is proven;
   the rest is labour.
5. **The moment of truth:** `simplec-in-simple` compiles itself, and the
   binary it produces is byte-identical to the one that compiled it
   (the standard fixed-point test).

Keep the C++ compiler alive throughout as the reference implementation
and differential oracle — exactly the role the `--no-opt` differential
test plays today.

## Verdict

**Self-hosting is a v1.x goal, and the current roadmap already builds
everything it requires** — and, as of the 2026-07-21 decision to drop
`box`, it needs *less* than we thought: the AST is a `list Expr` with
index-linked children, so self-hosting requires only v0.7's lists. The
lexer port is the first concrete step and can begin the moment v0.7
lands.
