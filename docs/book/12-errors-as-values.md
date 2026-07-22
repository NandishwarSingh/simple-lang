# 12. Errors as Values

*(New in v0.8.)* Simple promised from day one: **no exceptions**. Nothing
is thrown, nothing unwinds your stack, no invisible control flow. This
chapter is where that promise gets its other half — because programs do
fail, and a language needs a way to say so.

Simple's answer is the oldest and plainest one: **an error is a value**.
Functions that can fail *return* their error, and the caller looks at it.
If you've seen Go, this will feel like home.

## The `error` type and its two makers

`error` is a built-in type with exactly two ways to make one:

- `ok` — the value meaning *nothing went wrong*
- `fail("message")` — a real error, carrying the reason

```simp
fn check(n: int) -> error {
    if (n < 0) { return fail("negative: " + str(n)); }
    return ok;
}

fn main() {
    let e = check(-7);
    if (e != ok) {
        print(e.msg);          // negative: -7
    }
}
```

That's the whole surface: compare with `ok`, read `.msg`. An error is an
ordinary value in every way — store it in a `let mut`, put it in a
struct field, keep a `list error`, even send it down a channel to
another thread. It obeys the same value rules as everything else.

`.msg` is read-only (errors don't mutate — you make a new one), and on
`ok` it reads as `""`.

## Returning a value *and* an error

Most failing functions want to hand back a result *or* a reason. That's
what **multiple return values** are for — new in v0.8, and useful beyond
errors too:

```simp
fn parse_port(s: str) -> (int, error) {
    if (len(s) == 0) { return 0, fail("empty port"); }
    let n = int(s);
    if (n < 1 || n > 65535) { return 0, fail("port out of range: " + str(n)); }
    return n, ok;
}

fn main() {
    let (port, e) = parse_port("5432");
    if (e != ok) {
        print(e.msg);
        return;
    }
    print(port);               // 5432
}
```

`-> (int, error)` declares both. `return n, ok;` supplies both.
`let (port, e) = ...` receives both. Three pieces, all visible.

The compiler holds you to it:

- Every `return` in that function must supply **all** the values.
- Every call must **receive** all the values — `parse_port("x");` alone
  is a compile error. Failure is not ignorable by accident.
- Don't want one? Discard it *by name*: `let (_, e) = parse_port(s);`
  keeps only the error, `let (port, _) = ...` deliberately drops it.
  The `_` is your signature on the decision.

Multiple returns aren't reserved for errors:

```simp
fn divmod(a: int, b: int) -> (int, int) {
    return a / b, a % b;
}

let (q, r) = divmod(17, 5);    // q = 3, r = 2
```

## Passing errors up

When a function can't handle a failure itself, it returns it to *its*
caller — annotated with what it was doing at the time. This is the
pattern you'll use everywhere:

```simp
fn connect(host: str, port_s: str) -> (str, error) {
    let (port, e) = parse_port(port_s);
    if (e != ok) {
        return "", fail(host + ": " + e.msg);   // add context, pass it up
    }
    return host + ":" + str(port), ok;
}
```

A failure deep in the stack surfaces as a story:
`db.local: port out of range: 70000`. Every hop that touched it is in
the message, because every hop *chose* to add itself. Nothing about
this is special machinery — it's string concatenation and a return
statement, which is exactly the point.

## Why not exceptions?

Look at any call in Simple:

```simp
let (n, e) = parse_num(input);
```

Everything that can happen is on that line. The function returns two
things; you have both; the next line runs. Compare with a language
where `parse_num(input)` might *throw* — then any line is secretly also
a possible exit, every caller of every caller is part of the control
flow, and reading a function no longer tells you what it does.

The cost of Simple's way is honesty-by-typing: `if (e != ok)` appears
in your code as often as failure is possible. We think that's not a
cost. That's the program telling the truth.

## Errors travel like any value

Because an error is just a value, everything you already know applies.
A worker thread reports failures the same way it reports anything —
through a channel:

```simp
fn worker(jobs: chan int, results: chan error) {
    while (true) {
        let j = recv(jobs);
        if (j < 0) { break; }
        if (j % 3 == 0) { send(results, fail("job " + str(j) + " refused")); }
        else            { send(results, ok); }
    }
}
```

And a struct can carry one alongside its data:

```simp
struct Res { tag: str, err: error }
```

No wrapper types, no special channel-of-exceptions machinery. One rule
— errors are values — and the rest of the language does the work.

## What's deliberately not here

- **No exceptions, ever.** That promise is now kept in full.
- **No error codes to memorize.** An error is its message. If callers
  need to distinguish failures programmatically, that's a design signal
  the function should return distinct things — or that a richer error
  design should be argued for at [the roadmap](16-the-road-ahead.md).
- **No `?` propagation operator (yet).** Passing an error up is an
  `if` you can see. If the pattern earns an abbreviation, it will have
  to prove itself the way every feature does.

---

**Next:** [Talking to the Outside →](13-talking-to-the-outside.md)
