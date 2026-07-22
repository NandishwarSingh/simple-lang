# 13. Talking to the Outside

*(New in v0.85.)* Until now every Simple program was a sealed box: it
computed, it printed, it ended. This chapter opens the box — program
arguments, standard input, files, and exit codes. It's a short chapter,
because the API is small on purpose: seven builtins, and you already
know the error handling that powers them.

This milestone was *deliberately sequenced after errors*. Opening a file
that isn't there is the textbook failure-as-data case, and we refused to
ship file IO with a guessy "empty string means something went wrong"
convention. Now `read_file` can tell you what happened, because
[chapter 12](12-errors-as-values.md) gave it the words.

## Arguments: `argc()` and `arg(i)`

C convention, the one every shell and OS speaks: `arg(0)` is the
program's own path, real arguments start at `arg(1)`, and `argc()`
counts them all.

```simp
// ./greet Ada Grace
fn main() {
    for (i in 1..argc()) {
        print("hello " + arg(i));    // hello Ada, hello Grace
    }
}
```

`arg(i)` is bounds-checked like everything else in Simple: asking for
an argument that isn't there stops the program with the same clear
runtime error as an array overrun — never garbage, never undefined
behavior.

## Standard input: `input()` and `read_all()`

```simp
let name = input();       // one line, newline stripped
print("hi " + name);

let rest = read_all();    // everything remaining, until EOF
```

At end-of-input both return `""`. That makes the classic filter loop
three lines:

```simp
while (true) {
    let line = input();
    if (len(line) == 0) { break; }
    print(line);
}
```

(Yes, that also stops at a blank line — if your input has meaningful
blank lines, slurp with `read_all()` and split yourself. If real
programs hit this often, a richer read API can earn its place later.)

## Files: `read_file` and `write_file`

Whole-file operations — the 90% case — with real errors:

```simp
let (cfg, e) = read_file("app.conf");
if (e != ok) {
    print("cannot start: " + e.msg);   // cannot open app.conf
    exit(1);
}

let e2 = write_file("out.txt", result);
if (e2 != ok) { print(e2.msg); }
```

`read_file` returns `(str, error)` — you *must* receive both, so a
missing file can never be silently mistaken for an empty one.
`write_file` returns the `error` alone. The messages carry the path,
because future-you debugging at midnight deserves to know *which* file.

Streaming, appending, and binary interfaces can come later without
touching these two.

## Exit codes: `return` from `main`, or `exit(n)`

A program's status is how scripts and build systems read its verdict.
Two ways to set it:

```simp
fn main() -> int {
    if (argc() < 2) {
        print("usage: tool <file>");
        exit(2);                 // immediate, from anywhere
    }
    // ... work ...
    return 0;                    // the normal way out
}
```

`main` may return `int` (or nothing, which means 0). `exit(n)` ends the
program immediately from any depth — and it still releases everything
on the way out, so even the exit path is leak-clean. Not that the OS
would care — but *we* care, because the leak checker runs on every
example and zero means zero.

## A complete tool in twenty lines

Everything in this chapter, one program — count lines in a file:

```simp
fn main() -> int {
    if (argc() < 2) {
        print("usage: lc <file>");
        return 2;
    }
    let (txt, e) = read_file(arg(1));
    if (e != ok) {
        print(e.msg);
        return 1;
    }
    let mut lines = 0;
    for (i in 0..len(txt)) {
        if (txt[i] == '\n') { lines = lines + 1; }
    }
    print(lines);
    return 0;
}
```

That's a real Unix citizen: arguments, file, error to report, status
code. And it's the doorway to the long-term dream — a compiler reads
source files and writes object files, and as of this chapter, a Simple
program can do both.

---

**Next:** [Maps →](14-maps.md)
