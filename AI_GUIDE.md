# Writing Simple — a guide for AI code generators

You are generating code in **Simple** (`.simp` files): a small, native,
systems language. It is *not* C, Rust, Go, or Python — habits from those
languages will produce code that does not compile. Follow every rule below
literally. When two forms exist, prefer the explicit one.

Simple compiles ahead-of-time to a native binary. There is no runtime, no
garbage collector, no reflection, no exceptions.

---

## Rules (read these first)

These are the mistakes an AI trained on other languages will make. Each is a
hard rule.

**R1 — No OOP. Ever.** There are no classes, no methods, no inheritance, no
`self`/`this`, no interfaces. Data is plain structs; behavior is free
functions that take the data as a parameter.
```simp
fn area(r: Rect) -> int { return r.w * r.h; }   // ✅ free function
r.area()                                          // ❌ no methods exist
```

**R2 — Operations on collections are free functions, not methods.**
```simp
push(xs, 3);   len(xs);   pop(xs);   has(m, k);   del(m, k)   // ✅
xs.push(3);    xs.len();  m.has(k);                            // ❌
```

**R3 — Collection types use space syntax, not brackets or angles.**
```simp
list int      map str int      chan int          // ✅
list<int>     map[str]int      Map<String,Int>    // ❌
```
Indexing still uses `[]`: `xs[i]`, `m[key]`.

**R4 — Value semantics: assignment and argument passing COPY.** Lists, maps,
strings, and structs are copied (lazily, copy-on-write). A function cannot
mutate its caller's value. There are no references in safe code.
```simp
fn bump(xs: list int) { push(xs, 9); }   // mutates the COPY; caller unchanged
```
To "share" or link data, store **integer indices** into a list — never a
pointer or reference.

**R5 — Immutable by default.** `let` binds a constant; `let mut` allows
reassignment. Assigning to a `let` is a compile error.
```simp
let mut i = 0;  i = i + 1;   // ✅
let n = 0;      n = 1;       // ❌ compile error
```

**R6 — Every value of a multi-return MUST be received.** A bare call that
drops returns is a compile error. Use `_` to discard.
```simp
let (v, e) = read_file(p);   // ✅
let (v, _) = read_file(p);   // ✅ discard the error deliberately
read_file(p);                // ❌ compile error: returns unreceived
```

**R7 — Errors are values, returned as `(T, error)`. No exceptions.** There is
no `try`, `catch`, `throw`, or `?`. Build an error with `fail("msg")`, the
no-error value is `ok`, compare with `!=`, read the message with `.msg`.
Propagate explicitly.
```simp
fn parse(s: str) -> (int, error) {
    if (len(s) == 0) { return 0, fail("empty"); }
    return int(s), ok;
}
let (n, e) = parse(s);
if (e != ok) { return 0, fail("parse: " + e.msg); }   // wrap and pass up
```

**R8 — There is no `for x in list`. Iterate by index.**
```simp
for (i in 0..len(xs)) { let x = xs[i]; ... }   // ✅
for x in xs { ... }                            // ❌ not valid
```
`0..n` is **half-open**: it runs `n` times, `0` through `n-1`. `for (k in m)`
iterates a map's keys (in insertion order).

**R9 — Conditions need parentheses, must be `bool`, and braces are always
required.** No truthiness: an `int` in a condition is a compile error.
```simp
if (count > 0) { ... }   // ✅
if (count) { ... }        // ❌ int is not bool
if (x > 0) print(x);      // ❌ braces required
```

**R10 — Threads never share memory. Communicate through channels.** `spawn`
runs a function on a new thread; pass data with `send`/`recv`. There is no
shared mutable state, no locks, no atomics — by design this makes data races
impossible.

**R11 — Floating-point literals need a decimal point.** `2` is an `int`; `2.0`
is a `float`. Mixing needs an explicit conversion.
```simp
let x = 3.0 / 2.0;        // ✅ 1.5
let y = float(7) / 2.0;   // ✅ convert first
let z = 7 / 2;            // this is INTEGER division -> 3
```

**R12 — Conversions are function calls.** `int(s)`, `str(n)`, `float(n)`,
`u8(x)`, `i32(x)`, `u64(x)`, … There is no `as`, no implicit widening.

**R13 — `s[i]` on a string returns an `int` (the byte), not a character or
string.** `==` on strings compares **content**. Build strings with `+` and
`str()`; there is no string interpolation and no `printf`.
```simp
let c = name[0];                 // int, e.g. 83 for 'S'
if (name == "Simple") { ... }    // content compare
print("n=" + str(count));        // format by concatenation
```

**R14 — Reading a missing map key stops the program.** Guard with `has` first.
```simp
if (has(m, k)) { use(m[k]); } else { ... }   // ✅
```

**R15 — `print` takes exactly one value** (`int`, `float`, `bool`, or `str`).
No format string, no multiple arguments, no trailing options.

**R16 — These do not exist in Simple. Do not emit them:** classes/methods,
generics/templates, traits/interfaces, closures/lambdas, operator
overloading, `null`/`nil`/`None` (use `error` or a sentinel), string
interpolation, the ternary `?:`, `switch`/`match`, enums, `for-in` over
collections, references (`&`), and pointers outside `unsafe`.

---

## Reference

### Comments & structure
`//` line comments only. Every statement ends with `;`. Program entry is
`fn main() { ... }`, or `fn main() -> int { return code; }` to set an exit
code.

### Types
| type | notes |
|------|-------|
| `int` | 64-bit signed (the default integer) |
| `i8 i16 i32 i64`, `u8 u16 u32 u64` | sized integers |
| `float` | 64-bit IEEE-754; `f32` for 32-bit |
| `bool` | `true` / `false` |
| `str` | immutable text, heap-managed |
| `T[N]` | fixed array of `N` `T` (e.g. `int[5]`, 2-D `int[3][4]`) |
| `list T` | growable list |
| `map K V` | hash map; `K` is `int` or `str`; insertion-order iteration |
| `chan T` | channel |
| `struct Name { ... }` | plain data (define with `struct`) |
| `*T` | raw pointer — **only inside `unsafe`** |

### Variables
```simp
let name = expr;        // immutable
let mut count = 0;      // mutable
let x: int = 5;         // explicit type annotation (usually inferred)
```

### Operators
- Arithmetic: `+ - * / %` (`/` on ints truncates; on floats is real division)
- Comparison: `== != < <= > >=`
- Logical: `&& || !` (operands must be `bool`; `&&`/`||` short-circuit)
- Bitwise: `& | ^ << >> ~`
- Unary: `-x`, `!b`, `~n`
- `+` also concatenates strings.

### Control flow
```simp
if (c) { ... } else if (d) { ... } else { ... }
while (c) { ... }
for (i in 0..n) { ... }     // half-open range, i is immutable in the body
for (k in m) { ... }        // map keys, insertion order
break;   continue;   return expr;   return;   // (bare return for void)
```

### Functions & multiple returns
```simp
fn add(a: int, b: int) -> int { return a + b; }
fn greet(name: str) { print("hi " + name); }        // no return type = void
fn divmod(a: int, b: int) -> (int, int) { return a / b, a % b; }
let (q, r) = divmod(17, 5);
```
Recursion is fine. Parameters are received by value (copies).

### Structs
```simp
struct Point { x: int, y: int }          // fields comma-separated
struct Rect  { a: Point, b: Point }      // structs nest
let p = Point { x: 1, y: 2 };            // construct: all fields required
print(p.x);                              // field read
let mut r = Rect { a: p, b: Point { x: 9, y: 9 } };
r.b.y = 20;                              // nested field assignment
```
No methods. Write `fn f(p: Point) -> ...` and call `f(p)`.

### Fixed arrays
```simp
let mut a = [0; 5];                       // 5 ints, all 0  (type int[5])
let mut g: int[3][4] = [[0; 4]; 3];       // 2-D, explicit type
a[2] = 7;   let v = a[1];   let n = len(a);
```
Bounds are checked at runtime; an out-of-range index stops the program.

### Lists
```simp
let mut xs = list int;      // empty list
push(xs, 10);               // append
let last = pop(xs);         // remove & return last
let n = len(xs);
let v = xs[0];   xs[0] = 99;
```
Passing a list to a function copies it (copy-on-write); returning moves it out.

### Maps
```simp
let mut m = map str int;    // keys str, values int
m["a"] = 1;                 // insert / update
if (has(m, "a")) { print(m["a"]); }   // guard reads — a miss halts the program
del(m, "a");                // remove (no-op if absent)
let n = len(m);
for (k in m) { print(k + "=" + str(m[k])); }   // insertion order
```

### Strings
```simp
let s = "Sim" + "ple";      // concatenation
let n = len(s);             // O(1) length in bytes
let b = s[0];               // int byte value (83), NOT a string
let ok = s == "Simple";     // content comparison
let part = substr(s, 0, 3); // "Sim"  (start inclusive, end exclusive)
let t = str(42);            // int -> str
let k = int("42");          // str -> int (0 if not numeric; check yourself)
```
Char literals like `'A'` are `int` byte values. Escapes: `\n \t \" \\`.

### Errors (the `error` type)
```simp
fn open(path: str) -> (str, error) {
    if (len(path) == 0) { return "", fail("no path"); }
    return path, ok;
}
let (h, e) = open(p);
if (e != ok) { print(e.msg); return; }   // handle, or wrap: fail("open: " + e.msg)
```
`ok` = no error. `fail("...")` builds one. `e != ok` tests. `e.msg` reads the
message (a `str`). `main` cannot return `(T, error)`.

### Concurrency
```simp
fn worker(jobs: chan int, out: chan int) {
    while (true) {
        let j = recv(jobs);         // blocks until a value arrives
        if (j < 0) { return; }      // sentinel = shut down
        send(out, j * j);
    }
}
fn main() {
    let jobs = chan int(32);        // buffered channel, capacity 32
    let out  = chan int(32);
    spawn worker(jobs, out);        // spawn takes a function CALL
    spawn worker(jobs, out);
    for (i in 0..10) { send(jobs, i); }
    send(jobs, -1); send(jobs, -1); // one shutdown pill per worker
    let mut total = 0;
    for (i in 0..10) { total = total + recv(out); }
    print(total);
}
```
Never touch a value from two threads — send it through a channel instead.

### I/O
```simp
argc()                 // int: number of CLI args (arg 0 is the program path)
arg(i)                 // str: the i-th CLI arg
input()                // str: one line from stdin ("" at EOF)
read_all()             // str: the rest of stdin
read_file(path)        // (str, error)
write_file(path, data) // error
exit(code)             // terminate with an exit code
```

### Modules
```simp
import "textkit/words.simp";           // names shared globally: call word_count(...)
import "textkit/tally.simp" as freq;   // qualified: call freq.tally(...)
```
Paths are relative to the importing file. There is one flat program; an
imported function is called by its plain name unless you alias the import.

### `unsafe` and C interop (only when explicitly required)
```simp
extern fn puts(s: str) -> i32;         // declare a C function
extern fn malloc(size: u64) -> *u8;
fn main() {
    unsafe {                           // raw pointers only inside unsafe
        let p = malloc(8);
        // ... pointer work ...
    }
}
```
`*T` is a raw pointer; `*p` dereferences. Everything dangerous lives in
`unsafe { }`. Do not use this unless the task specifically needs C interop or
bare-metal access — safe Simple never needs it.

---

## Building large programs

Simple has no OOP, no generics, and no references — model larger systems with
just these primitives:

- **State** = structs (plain data) and free functions that transform them.
- **Collections** = `list` and `map`; grow them with `push` / index assignment.
- **Relationships** (graphs, trees, parent/child) = store an **integer index**
  into a `list`, not a pointer. e.g. a tree node holds `left: int, right: int`
  that index a `list Node`; `-1` means "none".
- **Errors** flow up the call stack as `(T, error)` return values — check and
  wrap at each layer.
- **Files** = one concern per file, pulled together with `import`.
- **Parallelism** = `spawn` workers that only communicate over channels.

This is enough to write compilers, servers, and simulations. The absence of
hidden control flow (no exceptions, no GC, no vtables) means generated code
does exactly what it says.

---

## Compile & run

```
simplec program.simp              # -> ./program (native binary)
simplec program.simp -o out       # choose the output name
simplec run program.simp          # compile and run immediately
simplec program.simp --no-opt     # disable the optimizer (for debugging)
```
A multi-file program is compiled by naming the file that contains `main`; its
`import`s are resolved automatically.

---

## A complete example

```simp
// wordfreq.simp — count words on stdin, print the most frequent.
fn is_space(c: int) -> bool { return c == 32 || c == 10 || c == 9; }

fn split(line: str) -> list str {
    let mut out = list str;
    let mut i = 0;
    let n = len(line);
    while (i < n) {
        while (i < n && is_space(line[i])) { i = i + 1; }   // skip spaces
        let start = i;
        while (i < n && !is_space(line[i])) { i = i + 1; }  // scan a word
        if (i > start) { push(out, substr(line, start, i)); }
    }
    return out;
}

fn main() {
    let text = read_all();
    let words = split(text);
    let mut counts = map str int;
    for (i in 0..len(words)) {
        let w = words[i];
        if (has(counts, w)) { counts[w] = counts[w] + 1; }
        else { counts[w] = 1; }
    }
    let mut best = "";
    let mut most = 0;
    for (k in counts) {
        if (counts[k] > most) { most = counts[k]; best = k; }
    }
    print("distinct: " + str(len(counts)));
    print("top: " + best + " (" + str(most) + ")");
}
```

That program uses value semantics, index iteration, free-function collection
ops, a `map`, string building by concatenation, and `read_all` — the whole
core of the language. If your generated code looks like this, it will compile.
