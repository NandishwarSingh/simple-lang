# 10. Concurrency

*(New in v0.4.)* Your machine has many CPU cores. Until now, Simple
programs used exactly one. This chapter fixes that — with two ideas you
can learn in ten minutes: `spawn` and channels.

## spawn: run a function on another core

```simp
fn greet() {
    print("hello from another thread");
}

fn main() {
    spawn greet();
    print("hello from main");
}
```

`spawn f(args);` starts `f` running **concurrently** — main doesn't wait
for it. The two prints race; either order is possible. That's the whole
syntax.

Two rules keep it sane:

- A spawned function must return nothing — results travel through
  channels (below). Spawning a value-returning function is a compile error
  that says so.
- When `main` ends, the program ends — running spawns and all. If you
  want their work, collect it before returning (channels again).

## Channels: how threads talk

Threads in Simple **never share memory**. There are no globals, and
everything that crosses between threads is a private copy. The *only*
connection between threads is a channel — a mailbox with a fixed number
of slots:

```simp
let ch = chan int(16);    // a mailbox carrying ints, 16 slots
send(ch, 42);             // put a value in (blocks if full)
let v = recv(ch);         // take a value out (blocks if empty)
```

`send` and `recv` are the whole API, and *blocking* is the feature: a
worker that calls `recv` on an empty channel simply sleeps until work
arrives. No polling, no locks, no callbacks — coordination falls out of
reading and writing.

## The worker pool — the pattern to remember

```simp
fn worker(jobs: chan int, results: chan int) {
    while (true) {
        let j = recv(jobs);
        if (j == -1) {           // the "poison pill": time to stop
            return;
        }
        send(results, j * j);
    }
}

fn main() {
    let jobs = chan int(32);
    let results = chan int(32);
    spawn worker(jobs, results);
    spawn worker(jobs, results);     // two cores now

    for (i in 1..11) {
        send(jobs, i);
    }
    send(jobs, -1);                  // one pill per worker
    send(jobs, -1);

    let mut total = 0;
    for (i in 0..10) {
        total = total + recv(results);
    }
    print(total);                    // 385 — always, regardless of timing
}
```

Note the shape: results are *summed*, so it doesn't matter which worker
finished first. Designing outputs that don't depend on timing is the
core skill of concurrent programming, and channels push you toward it.

Shutdown is a value, not a feature: agree on a sentinel (here `-1`) and
send one per worker. For string channels, a word like `"stop"` works —
see `examples/pipeline.simp`.

## Why there are no data races (really, none)

In most languages, threads sharing memory is the source of the worst
bugs in software — data races that corrupt state one time in a million.
Simple removes the category:

- Sending **copies**. Ints and structs are copied into the channel;
  strings are copied too (or handed over wholesale when the compiler can
  prove you were done with the value — same behavior, faster).
- Receiving gives you your own private value. Mutate it freely; nobody
  else can see it.
- Channels themselves are internally synchronized.

There is no rule to remember and no error message to learn, because
there is nothing you *can* write that races. The compiler adds zero new
checks for concurrency — the model makes the bugs unrepresentable.

## What it costs (measured, v0.4 lab)

- A `spawn` is a real OS thread: ~tens of microseconds to start. Spawn
  *workers* (a handful, reused), not one thread per tiny task.
- A send/recv is ~tens of nanoseconds when the channel has room — our
  channel machinery actually measured *faster than hand-written C
  pthreads code*, and tied Rust. When a thread must sleep and wake, the
  kernel charges microseconds — Go's lightweight goroutines win there,
  the honest trade of using real OS threads (for now).
- Parallel compute scales: the lab's 8-worker benchmark ran 5x faster
  than single-threaded Simple.

## Try it

1. Spawn 4 workers and split `is_prime` counting over 1..100000 among
   them by range. Compare the time against one worker (`time ./prog`).
2. Build a two-stage pipeline: one thread turns numbers into strings
   (`"n=" + str(i)`), a second shouts them. Shut both down with sentinels.
3. Delete one of the two `-1` pills in the worker-pool example and watch
   the program hang forever — then explain why. (That hang is honest:
   a worker is blocked on `recv` and nobody will ever send.)

**Next:** [Bare Metal →](11-bare-metal.md)
