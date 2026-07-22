# 1. Introduction

## What is Simple?

Simple is a general-purpose **systems programming language**. That means it's
made for building the software underneath other software: command-line tools,
servers, game engines, drivers — and one day, entire operating systems.

Systems languages have a reputation: C gives you power but lets you shoot your
foot off, and Rust gives you safety but demands months of study first. Simple's
bet is that you can have most of the power and most of the safety with a
fraction of the learning curve.

A taste:

```simp
fn is_prime(n: int) -> bool {
    if (n < 2) { return false; }
    let mut d = 2;
    while (d * d <= n) {
        if (n % d == 0) { return false; }
        d = d + 1;
    }
    return true;
}

fn main() {
    print(is_prime(97));    // true
}
```

If you can read that without a manual, the design is working.

## The principles

**Compiles to native code.** A Simple program becomes a real executable —
actual machine instructions, no virtual machine, no interpreter. `./program`
just runs.

**No garbage collector.** Memory is freed automatically the instant its
last user lets go — the compiler inserts reference counting for you
(chapter 9 explains how). There's nothing for you to manage and no
collector to pause your program, which matters when you're writing an OS
kernel or anything real-time.

**No OOP.** There are no classes, no inheritance, no methods, no `this`.
Simple has functions and data. That's not a missing feature — it's a decision.
Most of the complexity in large codebases comes from object hierarchies, and
Simple opts out entirely.

**Immutable by default.** Variables can't change unless you say `mut`.
The compiler catches accidental modification — a whole category of bugs —
before your program ever runs.

**Easy concurrency (planned).** Running code on multiple cores will be one
keyword: `spawn`. Threads communicate through channels, like Go.

## What you need to know already

This book assumes you've programmed a *little* in some language — you know
what a variable and a loop are. It does not assume you know C, Rust, or
anything about compilers.

Ready? Let's compile something.

**Next:** [Getting Started →](02-getting-started.md)
