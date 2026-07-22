# 2. The Lexer

**Files:** `src/token.hpp` (token model), `src/lexer.cpp` (scanner + `tokName`)

## Job

Turn a source string into a flat `vector<Token>`, ending with an `Eof`
token. Tokens carry their line number — every later error message depends
on that being right.

## The token model

```cpp
struct Token {
    Tok kind;          // enum: keywords, punctuation, Ident, IntLit, StrLit, Eof
    std::string text;  // identifier name, or decoded string-literal bytes
    int64_t ival;      // integer literal value
    int line;
};
```

One struct for every token kind, with unused fields idle — wasteful in
principle, simple in practice.

## How the scanner works

`lex()` is a single loop over the characters with one `i` cursor. Each
iteration dispatches on the current character class:

- **whitespace** — skipped; `\n` increments `line`.
- **comments** — `//` to end of line; `/* */` non-nesting, newlines counted,
  unterminated is an error reported at the *opening* line.
- **identifier/keyword** — `[A-Za-z_][A-Za-z0-9_]*`, then one hash lookup in
  the `kKeywords` map decides `Ident` vs keyword. Keywords are not special
  in the scanner itself.
- **number** — decimal or `0x` hex, accumulated as `unsigned long long`
  with an overflow check against `INT64_MAX` *per digit* (so huge literals
  fail cleanly, not by wrapping).
- **string** — escapes are decoded *here* (`\n` becomes byte 10 in
  `Token::text`), so the rest of the compiler never thinks about escaping
  again. Codegen re-encodes safely for QBE. Unterminated at the opening line.
- **operators** — single `switch` with one-character lookahead for the
  two-char tokens (`->`, `==`, `!=`, `<=`, `>=`, `&&`, `||`).

## Deliberate errors

Lone `&` and `|` produce a targeted message ("did you mean '&&'?") instead
of a generic unexpected-character error, because that typo is inevitable.
When bitwise operators arrive (bare-metal work), these become real tokens.

## `tokName`

Also in this file: `tokName(Tok) -> const char*` gives every token a
printable name (`"';'"`, `"identifier"`). The parser's `expected X, found Y`
errors are built from it. New token = new `tokName` case, same commit.

## Invariants the parser relies on

1. The stream always ends with exactly one `Eof`.
2. Every token's `line` is the line it *started* on.
3. `StrLit.text` holds decoded bytes, not source spelling.
