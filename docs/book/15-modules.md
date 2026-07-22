# 15. Modules

*(New in v0.9.)* Everything so far lived in one file. That's fine for a
program you can hold in your head, and cramped for anything larger. This
chapter splits a program across files — the last thing standing between
Simple and code the size of a real project.

The whole feature is one keyword: `import`.

## Splitting a program

Put related functions in their own file:

```simp
// words.simp
fn is_space(c: int) -> bool {
    return c == 32 || c == 10 || c == 9;
}

fn word_count(s: str) -> int {
    let mut n = 0;
    let mut inword = false;
    for (i in 0..len(s)) {
        if (is_space(s[i])) { inword = false; }
        else { if (!inword) { n = n + 1; } inword = true; }
    }
    return n;
}
```

Then use it from another file by importing it:

```simp
// main.simp
import "words.simp";

fn main() {
    print(word_count("the quick brown fox"));   // 4
}
```

`import "words.simp";` makes everything in `words.simp` available here,
called by its plain name — `word_count(...)`, no prefix, no ceremony.
Splitting a file is purely organizational: the program behaves exactly
as if you'd pasted the files together.

Import paths are **relative to the file doing the importing**, so a
folder of related files moves as a unit:

```simp
import "textkit/words.simp";
import "textkit/tally.simp";
```

You compile the program by naming its entry file — `simplec main.simp`.
The compiler follows the imports and pulls in everything reachable. A
file nobody imports is never compiled. There is no project file, no
build config, no list of sources to maintain: **the imports are the
build.**

## One shared namespace

Simple keeps this deliberately simple: all files share **one namespace**.
An imported function is just... there, like it was always part of your
file. There's nothing to learn beyond `import`.

Two consequences follow, both intended:

- **Struct names are program-wide.** A `struct Token` means the same
  type everywhere, so two files that both `import "types.simp"` are
  talking about the *same* `Token`. Defining two different `struct
  Token`s in two files is an error — the compiler names both files and
  asks you to pick one.
- **Function names can repeat across files** (each file's helpers are
  its own business), but if you `import` two files that both define a
  plain `parse()` and you *call* `parse()`, that's ambiguous — and the
  compiler says so, naming both files.

## Aliases: when you want to be explicit

For the ambiguous case — or just for readability — import a file under a
name and qualify the call:

```simp
import "textkit/tally.simp" as freq;

fn main() {
    let counts = freq.tally("hello world");   // "the tally from freq"
    print(counts[108]);                        // count of 'l'
}
```

`import "..." as freq;` doesn't change what's imported — it adds a
handle. `freq.tally(...)` says *this* file's `tally`, unambiguously,
even if another imported file also has one. Use it when it makes the
code clearer; skip it when the plain name is obvious.

## Errors know which file they're in

When something's wrong in an imported file, the compiler points *there*,
not at your `main`:

```
textkit/words.simp:4: error: 'word_count' returns int, not str
```

Split code, precise diagnostics — the two aren't in tension.

## What's deliberately not here

- **No `pub`/`private`.** Everything is visible; the language stayed at
  its smallest. (If real programs prove they need to hide helpers, a
  visibility keyword can earn its place — but it hasn't yet.)
- **No packages, versions, or a registry.** That's a package manager's
  job, and Simple isn't one. Imports are file paths, full stop.
- **No circular-import errors to fear.** A imports B, B imports A — it
  just works. Each file is loaded once no matter how the arrows point.

With modules, the language is now big enough to build big things — and
small enough to still fit in your head. That was always the target.

---

**Next:** [The Road Ahead →](16-the-road-ahead.md)
