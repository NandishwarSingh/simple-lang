# 6. Functions

## Defining and calling

```simp
fn add(a: int, b: int) -> int {
    return a + b;
}

fn main() {
    print(add(2, 3));      // 5
    print(add(add(1, 2), add(3, 4)));   // 10
}
```

Anatomy: `fn name(param: type, ...) -> return_type { body }`.

- **Parameter types are required.** The compiler can't guess what `add`
  should accept — and honestly, neither can the next person reading it.
- The `-> type` says what the function gives back. `return` hands a value
  back to the caller and ends the function immediately.
- Functions can be defined in any order — `main` at the top calling helpers
  below it is perfectly fine.

## Functions that return nothing

Leave off the `->`:

```simp
fn cheer(name: str) {
    print("go");
    print(name);
}

fn main() {
    cheer("Simple");
}
```

A bare `return;` exits such a function early. Trying to use its "result"
(`let x = cheer("hi");`) is a compile error — there's no value to use.

## Every path must return

If a function promises an `int`, the compiler proves it delivers one no
matter which way execution goes:

```simp
fn sign(n: int) -> int {
    if (n > 0) {
        return 1;
    } else if (n < 0) {
        return -1;
    }
    // ← compile error: what if n == 0? We fall off the end.
}
```

```
program.simp:1: error: function 'sign' must return int on every path
```

Add a final `return 0;` and it compiles. In C this bug ships and returns
garbage at 2am; in Simple it doesn't build.

## Recursion

A function can call itself. The classic: each Fibonacci number is the sum of
the previous two.

```simp
fn fib(n: int) -> int {
    if (n < 2) {
        return n;          // base case — without this it recurses forever
    }
    return fib(n - 1) + fib(n - 2);
}

fn main() {
    let mut i = 0;
    while (i <= 10) {
        print(fib(i));     // 0 1 1 2 3 5 8 13 21 34 55
        i = i + 1;
    }
}
```

Every recursive function needs a **base case** that returns without recursing,
or it calls itself until the stack runs out.

## `main` is special

- Every program must have exactly one `fn main()`.
- It takes no parameters and returns nothing (or `int`, if you want to set
  the process exit code — `0` means success by convention).

## Try it

1. Write `fn max(a: int, b: int) -> int` and test it.
2. Write `fn factorial(n: int) -> int` recursively (`factorial(5)` is 120).
3. Delete a `return` from `sign` above and read the error.
4. Write `fn is_vowel_count_even(word: str) -> bool` — count the vowels by
   walking the bytes with `word[i]` up to `len(word)`, then return whether
   the count is even. Chapter 9 covers string indexing in full.

**Next:** [Understanding Compile Errors →](07-understanding-errors.md)
