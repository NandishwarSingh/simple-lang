# 3. The Parser and AST

**Files:** `src/ast.hpp` (node definitions), `src/parser.cpp` (recursive descent)

## The AST shape

Three node categories, each one tagged struct (a `kind` enum + a union of
fields, some unused per kind) rather than a class hierarchy with virtual
dispatch. With ~7 expression kinds, tagged structs mean every pass is one
`switch` you can read top to bottom.

```cpp
Expr  { kind, line, type, ival, str, op, lhs, rhs, args }
Stmt  { kind, line, name, isMut, hasType, declType, expr, body, elseBody }
Function { name, params, ret, body, line }   →  Program { funcs }
```

Field reuse conventions worth knowing:

- `Expr::str` is the string-literal value **or** variable name **or** callee name.
- Unary expressions use `lhs` only.
- `Stmt::expr` is the let-initializer / assigned value / condition / return value.
- An `else if` chain is not a special form: the parser puts a single nested
  `If` statement into `elseBody`. Passes never know `else if` exists.
- `Expr::type` is meaningless until sema runs — codegen requires sema first.

## The parser

Hand-written recursive descent — the approach used by the real compilers we
admire (Go, Rust, Clang), and the easiest to give good errors from.

Core machinery (all ~10 lines each): `peek`, `next`, `at(k)`, `eat(k)`
(conditional advance), and `expect(k, what)` which throws
`expected X, found Y` on mismatch.

### Statements

`parseStmt` dispatches on the leading token (`let`, `if`, `while`, `return`,
`break`, `continue`, `{`). Anything else is parsed as an *expression*, and
then:

- if the next token is `=` and the expression was a bare variable → it's an
  assignment (anything else on the left is "invalid assignment target");
- otherwise it's an expression statement.

This "parse first, classify after" trick avoids needing lookahead to tell
`x = 5;` from `f(x);`.

### Expressions: precedence climbing

One function per precedence level, each calling the next-tighter one and
looping on its own operators:

```
parseExpr = parseOr → parseAnd → parseEquality → parseComparison
          → parseAdd → parseMul → parseUnary → parsePrimary
```

All binary operators are left-associative; unary recurses into itself
(`--x` parses, sema rejects nothing — it's minus minus x, fine). A call is
recognized in `parsePrimary`: identifier followed by `(`.

Adding an operator = one line in the right level's `while` condition
(plus lexer token + sema typing + codegen case).

## Error philosophy

The parser stops at the first error (see [pipeline](01-pipeline.md)). No
recovery, no sync points — which means no risk of misleading cascade
errors, at the cost of one-error-per-run. Revisit when files get big.

## v0.2 additions

- **Types are structural now:** `Type` carries a kind plus `sname` (struct)
  or `alen`/`elem` (array, `elem` is a `shared_ptr` so `Type` stays
  copyable). `parseType` accepts any identifier (sema validates struct
  names) and folds `[N]` suffixes right-to-left so `int[2][4]` means "2
  arrays of 4" and `g[r][c]` indexes in written order.
- **Postfix chains:** `parsePostfix` sits between unary and primary,
  looping `.field` and `[index]` onto any primary — so `pts[1].x`,
  `f().a`, `g[r][c]` all parse with no special cases.
- **New expression kinds:** `ArrayLit`, `Index`, `Field`, `StructLit`
  (fieldNames + args in written order). Struct literals (`Name {`) are
  recognized in primary position — unambiguous *because* conditions
  require parentheses, a free payoff from that syntax decision.
- **Assignment targets are expressions now:** `Stmt::lhs` holds a
  Var/Field/Index chain ("parse expression first, classify on `=`" still
  works). `for (i in a..b)` is its own statement kind with `expr`/`expr2`
  bounds — not parser-desugared, so error lines stay honest.
- **Top level:** `struct` declarations join `fn`, collected into
  `Program::structs`.
