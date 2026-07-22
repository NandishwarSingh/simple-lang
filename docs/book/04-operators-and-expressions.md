# 4. Operators and Expressions

## Arithmetic

The five arithmetic operators work on `int` and produce `int`:

```simp
fn main() {
    print(7 + 3);    // 10
    print(7 - 3);    // 4
    print(7 * 3);    // 21
    print(7 / 3);    // 2   ← integer division: the remainder is dropped
    print(7 % 3);    // 1   ← the remainder itself
}
```

`/` and `%` are the pair to understand: division truncates toward zero, and
`%` gives what was left over. `%` is everywhere in real code — `n % 2 == 0`
is "n is even", `i % 10` is the last digit of `i`.

Unary minus negates: `-x`.

## Comparison

Comparisons take two `int`s and produce a `bool`:

```simp
print(5 < 7);     // true
print(5 <= 5);    // true
print(5 > 7);     // false
print(5 >= 7);    // false
```

Equality works on `int` and on `bool`:

```simp
print(5 == 5);        // true
print(5 != 7);        // true
print(true == false); // false
```

You cannot compare an `int` with a `bool` — that's a compile error, because
it's never what you meant.

## Logic

`&&` (and), `||` (or), and `!` (not) work on `bool`:

```simp
let age = 25;
let has_ticket = true;
print(age >= 18 && has_ticket);   // true
print(age < 13 || age > 65);      // false
print(!has_ticket);               // false
```

`&&` and `||` **short-circuit**: the right side doesn't run at all if the left
side already decides the answer. `false && anything` never looks at
`anything`; `true || anything` doesn't either. This matters once function
calls appear on the right side — they simply don't happen.

## Precedence

Operators bind in this order, tightest first:

| level | operators           |
|-------|---------------------|
| 1     | `-x` `!x` (unary)   |
| 2     | `*` `/` `%`         |
| 3     | `+` `-`             |
| 4     | `<` `<=` `>` `>=`   |
| 5     | `==` `!=`           |
| 6     | `&&`                |
| 7     | `\|\|`              |

So `2 + 3 * 4 == 14` is `true`, and `a && b || c` means `(a && b) || c`.
When in doubt, use parentheses — `(2 + 3) * 4` — they cost nothing and the
next reader will thank you.

## `print`

`print` is a built-in that takes exactly one value — `int`, `bool`, or
`str` — and writes it out followed by a newline:

```simp
print("total:");
print(2 + 2);
print(2 + 2 == 4);
```

```
total:
4
true
```

## Try it

1. Predict, then check: `print(10 % 3 * 2);`
2. Predict, then check: `print(1 + 2 == 3 && 4 > 5 || true);`
3. Write an expression that's `true` exactly when a number `n` is a
   two-digit number (10–99).

**Next:** [Control Flow →](05-control-flow.md)
