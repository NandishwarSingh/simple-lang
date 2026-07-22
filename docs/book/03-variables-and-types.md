# 3. Variables and Types

## `let` — variables that don't vary

```simp
fn main() {
    let answer = 42;
    print(answer);
}
```

`let` binds a name to a value. The compiler figures out the type from the
value (`42` is an `int`), so you rarely write types on variables.

Here's the important part: **a `let` variable can never be changed.**

```simp
fn main() {
    let answer = 42;
    answer = 43;        // ← compile error
}
```

```
program.simp:3: error: cannot assign to immutable variable 'answer' (declare it with `let mut`)
```

This feels strict at first, but it's one of Simple's best features. Most
variables in real programs are set once and only *read* afterwards. When you
make that the default, the compiler catches every accidental overwrite — and
when you read code, `let` is a promise: *this value is what it was on day one.*

## `let mut` — opting into change

When a variable genuinely needs to change, say so:

```simp
fn main() {
    let mut count = 0;
    count = count + 1;
    count = count + 1;
    print(count);       // 2
}
```

The rule of thumb: **write `let`, and only add `mut` when the compiler
complains.** Your code ends up with exactly as much mutability as it needs and
no more.

## The types

Simple v0.1 has three types:

| type   | example values      | what it is                        |
|--------|---------------------|-----------------------------------|
| `int`  | `42`, `-7`, `0xFF`  | 64-bit signed integer             |
| `bool` | `true`, `false`     | a truth value                     |
| `str`  | `"hello\n"`         | text (read-only literals, v0.1)   |

Notes:

- `int` is always 64-bit. No `short`/`long`/`unsigned` zoo — one integer
  type you can trust. (Sized types like `u8` arrive with the bare-metal work.)
- Integer literals can be hex: `0xFF` is 255.
- Strings support escapes: `\n` newline, `\t` tab, `\"` quote, `\\` backslash.
- Real string manipulation (concatenation, length) comes in v0.2 when strings
  become heap values.

## Type annotations

You can state a type explicitly with `:`

```simp
let count: int = 0;
let ready: bool = false;
```

This is optional and mostly useful as documentation. The compiler still
checks it: `let x: bool = 5;` is a compile error, not a conversion.

Simple **never converts types silently**. An `int` is never secretly a `bool`;
`if (1)` is an error, not "truthy". What you write is what happens.

## Scopes and shadowing

Braces create scopes, and inner scopes can *shadow* outer names:

```simp
fn main() {
    let x = 1;
    {
        let x = 100;     // a different x, only inside these braces
        print(x);        // 100
    }
    print(x);            // 1
}
```

Declaring the same name twice in the *same* scope is an error — that's
almost always a mistake, so the compiler treats it as one.

## Try it

1. Declare a `let` variable and try to change it. Read the error.
2. Print `0xFF` and check you get 255.
3. Write `let flag: int = true;` and see how the compiler explains the problem.

**Next:** [Operators and Expressions →](04-operators-and-expressions.md)
