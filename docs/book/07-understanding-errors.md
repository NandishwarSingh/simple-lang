# 7. Understanding Compile Errors

New programmers read "error" and feel judged. Reframe it now: every compile
error is a bug that **didn't make it into your running program**. The
compiler is the fastest, most patient code reviewer you'll ever have, and in
Simple the errors are written to be read.

Every error has the same shape:

```
file.simp:LINE: error: what went wrong (and often, how to fix it)
```

Here's a field guide to the ones you'll meet, each with the code that causes it.

## The immutability guard

```simp
let total = 100;
total = 200;
```
```
error: cannot assign to immutable variable 'total' (declare it with `let mut`)
```

Either you forgot `mut`, or — more often, and more usefully — you're
modifying something you didn't mean to. Look before you add `mut`.

## Type mismatches

```simp
let x = 5 + true;
```
```
error: '+' needs int operands, got int and bool
```

```simp
if (count) { ... }
```
```
error: condition must be bool, got int
```

Simple never converts types for you. Where C would silently treat `count` as
a truth value, Simple asks you to say what you mean: `if (count != 0)`.

## Name problems

```
error: undefined variable 'totl'
```
Usually a typo. Also happens when the variable exists in a *different* scope —
names declared inside `{ }` don't exist outside them.

```
error: 'x' is already defined in this scope
```
Two `let x` in the same braces. Shadowing in an *inner* scope is fine.

## Function contract violations

```
error: 'add' takes 2 argument(s), got 3
error: argument 1 of 'add' must be int, got bool
error: function 'sign' must return int on every path
error: 'cheer' returns nothing, but this returns a value
```

The signature `fn add(a: int, b: int) -> int` is a contract, and the
compiler enforces both sides of it: callers must pass the right things, the
body must return the right thing, always.

## Loop and statement rules

```
error: 'break' outside of a loop
error: this expression does nothing (its result is unused)
```

The second one catches lines like `x + 1;` — computing a value and throwing
it away is almost always a mistake (you probably meant `x = x + 1;`).

## A debugging strategy

1. **Read the line number first.** Go there. The problem is usually on that
   line or the one above it.
2. **Fix the first error only, then recompile.** Simple stops at the first
   problem it finds, so there's never a wall of cascading errors to triage.
3. **Believe the error, not your memory.** If it says `total` is immutable,
   it is — even if you're sure you wrote `mut` (you didn't).

**Next:** [Under the Hood →](08-under-the-hood.md)
