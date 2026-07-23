# 5. Control Flow

## `if` and `else`

```simp
fn main() {
    let temperature = 30;
    if (temperature > 25) {
        print("hot");
    } else if (temperature > 15) {
        print("mild");
    } else {
        print("cold");
    }
}
```

The rules:

- The condition goes in parentheses and **must be a `bool`**. `if (1)` is a
  compile error — no truthiness, no surprises.
- The braces are **always required**, even for one statement. This kills the
  classic C bug where an indented line *looks* inside the `if` but isn't.
- Chain as many `else if` as you like; the first true branch wins.

## `while`

`while` repeats its body as long as the condition holds:

```simp
fn main() {
    let mut i = 1;
    while (i <= 5) {
        print(i);
        i = i + 1;
    }
}
```

The loop variable needs `let mut` — it changes every iteration.

## `for`

When you're counting over a range, `for` says it more directly:

```simp
fn main() {
    for (i in 0..5) {
        print(i);           // 0, 1, 2, 3, 4
    }
}
```

`0..5` is a half-open range: it includes `0` and stops **before** `5`, so
this runs five times. The loop variable (`i` here) is declared for you and is
immutable inside the body — you can read it but not reassign it, which is
exactly what you want for a counter. Use `while` when the stopping condition
isn't a simple range.

## `break` and `continue`

`break` exits the loop immediately. `continue` skips to the next iteration:

```simp
fn main() {
    let mut i = 0;
    while (true) {           // loop forever...
        i = i + 1;
        if (i % 2 == 0) {
            continue;        // skip even numbers
        }
        if (i > 9) {
            break;           // ...until this
        }
        print(i);            // 1 3 5 7 9
    }
}
```

`while (true)` + `break` is the standard way to write "loop until something
happens in the middle". Using `break` or `continue` outside a loop is a
compile error.

## Putting it together: FizzBuzz

The classic interview question — print 1 to 20, but multiples of 3 say
"Fizz", multiples of 5 say "Buzz", and multiples of both say "FizzBuzz":

```simp
fn main() {
    let mut i = 1;
    while (i <= 20) {
        if (i % 15 == 0) {
            print("FizzBuzz");
        } else if (i % 3 == 0) {
            print("Fizz");
        } else if (i % 5 == 0) {
            print("Buzz");
        } else {
            print(i);
        }
        i = i + 1;
    }
}
```

Note the order: the `% 15` test must come first, because a multiple of 15
is also a multiple of 3 — and the first true branch wins.

## Try it

1. Print the powers of 2 below 1000 (1, 2, 4, 8, ...).
2. Given `let n = 1234;`, print its digits in reverse using `% 10` and `/ 10`.
3. Change FizzBuzz to skip (not print) plain numbers, using `continue`.

**Next:** [Functions →](06-functions.md)
