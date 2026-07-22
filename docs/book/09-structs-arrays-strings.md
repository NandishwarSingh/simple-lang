# 9. Structs, Arrays, and Strings

*(New in v0.2.)* So far every value has been a single number, truth, or
text. Real programs need *shapes* — a player with a position and health, a
board of cells, a name built out of pieces. This chapter is where Simple
gets its data.

## Structs: give a shape a name

```simp
struct Point {
    x: int,
    y: int,
}
```

A struct is a group of named fields. That's the whole feature — no methods,
no inheritance, no constructors. Data is data; functions act on it.

```simp
fn dist2(a: Point, b: Point) -> int {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    return dx * dx + dy * dy;
}

fn main() {
    let p = Point { x: 3, y: 4 };      // a struct literal
    print(p.x);                        // field access
    print(dist2(p, Point { x: 0, y: 0 }));   // 25
}
```

Struct literals must name **every** field (in any order) — there are no
half-built values in Simple. Structs nest freely (`struct Rect { a: Point,
b: Point }`, accessed as `r.a.x`), and functions can return them, which
finally gives you multiple return values:

```simp
struct DivMod { q: int, r: int }

fn divmod(a: int, b: int) -> DivMod {
    return DivMod { q: a / b, r: a % b };
}
```

Assigning to a field needs the *variable* to be `let mut` — mutability is a
property of the whole box: `r.b.x = 11;` works iff `r` is `let mut`.

## Arrays: many of the same thing

```simp
let mut xs = [3, 1, 4, 1, 5];     // type: int[5]
print(xs[0]);                     // 3
xs[2] = 42;
print(len(xs));                   // 5
```

Arrays have a **fixed length that's part of the type**: `int[5]` and
`int[6]` are different types. All elements share one type. Grids are arrays
of arrays: `let g: int[2][3] = [[1,2,3],[4,5,6]];` indexed `g[row][col]`.
Growable lists arrive in a later version, built on these.

**Every index is bounds-checked.** In C, `xs[9]` on a 5-element array reads
whatever memory happens to be there — the classic security hole (buffer
overflow). In Simple:

```
runtime error: index 9 out of bounds (length 5)
```

The program stops, cleanly, at the exact moment of the mistake. This is the
first piece of *runtime* safety in the language, joining the compile-time
kind you already know.

## Copies, not spooky links

Here's the rule that makes Simple data easy to reason about: **assignment
copies.** Structs and arrays are *values*, like ints.

```simp
let mut a = [1, 2, 3];
let mut b = a;        // b is a full, independent copy
b[0] = 99;
print(a[0]);          // 1 — a is untouched
```

In Java or Python, `b` and `a` would secretly point at the same array, and
mutating one would change the other — a legendary source of bugs. In
Simple, what you see is what exists. Two names never share mutable data.

## Strings, for real this time

Strings are now values you can build and inspect:

```simp
let name = "Sim" + "ple";       // concatenation
print(len(name));               // 6
print(name == "Simple");        // true

fn repeat(s: str, n: int) -> str {
    let mut out = "";
    for (i in 0..n) {
        out = out + s;
    }
    return out;
}
```

Strings are **immutable**: `+` always builds a new string rather than
modifying one in place, so sharing a string is always safe.

And as of v0.3, string memory manages itself: the compiler counts how many
variables use each string and frees it the instant the last one lets go —
no garbage collector, no pauses, nothing for you to do or learn. Build
strings in a loop forever; memory stays flat. (Chapter 10 has the story;
the lab measured the feature costing *zero* speed.)

## The `for` loop

Counting loops got their own syntax:

```simp
for (i in 0..5) {      // i = 0, 1, 2, 3, 4  (end is excluded)
    print(i);
}
```

`i` is created by the loop, immutable inside the body, and gone after it.
`break` and `continue` work exactly as in `while`. The bounds are evaluated
once, up front. Anything fancier than counting up by one — count-downs,
strides — stays an honest `while` loop.

## Try it

1. Make a `struct Player { hp: int, gold: int }`, write
   `fn pay(p: Player, cost: int) -> Player`, and check that the original
   is unchanged after a payment (value semantics!).
2. Sum the diagonal of a 3×3 grid.
3. Write `fn join3(a: str, b: str, c: str) -> str` using `+`.
4. Index an array with `len(xs)` (off by one, on purpose) and read the
   runtime error.

**Next:** [Concurrency →](10-concurrency.md)
