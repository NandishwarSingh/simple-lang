# 4. The Type Checker

**File:** `src/sema.cpp` (entry: `analyze(Program&)`)

## Job

Prove the program is well-typed, or throw the most helpful `CompileError`
possible. Side effect (relied on by codegen): every `Expr::type` gets filled
in.

This stage owns *all* language rules that aren't grammar:
types, mutability, scoping, arity, return-path coverage, loop-only
statements. Codegen contains **zero** checks — if sema passes, codegen must
succeed. Keep it that way: a new rule goes here, never there.

## Structure

Two passes over the program:

1. **Signature collection.** Map every function name → `Function*`; reject
   duplicates and a user-defined `print`. Then validate `main` exists with
   the right shape. Collecting first is what allows calls to functions
   defined later in the file.
2. **Body checking.** `checkFunction` per function: a statement walk
   (`checkStmt`) driving an expression walk (`checkExpr`), with this state:

```cpp
scopes_    // vector of {name → {Type, isMut}} — a stack, one map per brace depth
cur_       // function being checked (for return statements)
loopDepth_ // break/continue legality
```

## Scoping

`scopes_` is pushed/popped around every block: function params get their own
outermost scope, the body is a second scope (so params can be shadowed),
and each `if`/`while`/bare block pushes another. `lookup` walks the stack
top-down — innermost name wins, which *is* shadowing. Redeclaration is only
an error within a single map.

## Expression rules (the table codegen trusts)

| expression        | requirement                     | result type |
|-------------------|---------------------------------|-------------|
| `+ - * / %`       | both `int`                      | `int`       |
| `< <= > >=`       | both `int`                      | `bool`      |
| `== !=`           | same type; `int` or `bool` only | `bool`      |
| `&& \|\|`         | both `bool`                     | `bool`      |
| `-x` / `!x`       | `int` / `bool`                  | same        |
| call              | arity + each arg vs param       | declared ret|
| `print(x)`        | 1 arg, not void                 | `void`      |

No implicit conversions anywhere. Conditions must be exactly `bool`.
`print` is special-cased by name before function lookup — it's a builtin,
not a `Function`.

## Return-path analysis

Non-void functions must provably return on every path. `stmtsReturn` is a
conservative structural check: a statement list returns if *any* statement
does; `Return` does; `If` does when both branches exist and both do; a block
does if its list does; **`while` never counts** (its condition might be false
on entry — even `while (true)`, which we don't special-case yet).
Conservative = may demand a redundant final `return`, never accepts a
missing one.

## Error message style

House rules, learned from the errors people actually hit:

- Name the thing: `'count'`, `'add'` — never "variable" or "the function".
- State the rule *and* the fix when the fix is one thing:
  `cannot assign to immutable variable 'x' (declare it with `let mut`)`.
- Types by name (`int`, `bool`), never internal representations.

## v0.2 additions

- **Struct table:** pass 1 registers all struct declarations (duplicate
  names, duplicate fields, unknown field types are errors), then a DFS
  rejects recursive layouts ("struct 'Node' contains itself") — an
  infinite-size type must never reach codegen's size computation.
- **Structural type equality:** `operator==` on `Type` recurses through
  array element types; `typeName` prints array dims outside-in so errors
  show `int[2][4]` as written.
- **New expression rules:** `ArrayLit` (elements all one type), `Index`
  (array base, int index), `Field` (struct base, declared field),
  `StructLit` (every field exactly once, right types). `+` now also takes
  `str`+`str`; `==`/`!=` accept `str` (content comparison); aggregates
  can't be compared or printed. `len(x)` joins `print` as a reserved
  builtin (str or array).
- **Lvalue checking:** assignment targets are Var/Field/Index chains; the
  checker walks to the *root variable* — mutability is a property of the
  whole box, so `r.b.x = 1;` needs `r` to be `let mut`. Assigning into a
  temporary (`f().x = 1;`) is an error.
- **`for`:** bounds must be ints; the loop variable is bound immutable in
  a fresh scope. For return-path analysis, `for` (like `while`) never
  guarantees a return — the range may be empty.

## Modules (v0.9)

Sema owns cross-file name resolution; the driver only hands it one
merged `Program` with per-decl `fileId`s and a per-file `imports` list.
The model the user chose is **flat namespace, everything visible**: an
imported file's names are usable directly, no `pub`.

- **Program-wide names, per-file uniqueness.** A struct name must be
  unique across the *whole* program (types are global). A function name
  may **repeat across files** — that's the point of aliases — but never
  twice in one file, and a Simple function may never share an extern's
  name. The duplicate-struct and double-`main` errors name *both*
  files.
- **Link names.** Because a function name can recur, sema assigns each
  function a `linkName`, unique program-wide: the bare name when it's
  unique (so single-file output is byte-identical to before) or
  `name.f<fileId>` when it recurs. `.` can't occur in a Simple
  identifier, so these never collide with user names. Codegen emits and
  calls purely by `linkName`; sema rewrites every resolved call's
  `Expr::str` to it.
- **Resolution (`resolveFn`).** From the calling function's file:
  *unqualified* names see the caller's own file plus its plainly
  imported files; two visible candidates is an ambiguity error that
  points at both files and suggests `as`. A *qualified* `alias.name(…)`
  (parsed only when `alias` was declared with `as`) resolves to exactly
  that file. Externs are the one shared symbol — any file may call them.
- **Error attribution.** `CompileError` carries an optional `file`;
  `run()` wraps `runImpl()` and stamps the current decl's file on any
  error that didn't already set one, so a type error in an imported
  file reads `imported.simp:line: …`, not the root's name.

## Growth notes

- **v0.3 ARC** needs no checking pass at all — it's pure insertion in
  codegen/MIR (see [future](07-future.md)). Sema's only contribution is
  knowing which types are heap-allocated. No new errors, no new files here.
