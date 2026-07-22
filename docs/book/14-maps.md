# 14. Maps

*(New in v0.9.)* Lists hold things in a row; maps hold things by name.
`map K V` is Simple's second built-in collection — a hash table that
looks up, inserts, and deletes in constant time, wrapped in the same
value rules as everything else in the language.

```simp
let mut ages = map str int;
ages["ada"] = 36;
ages["grace"] = 45;
print(ages["ada"]);        // 36
print(len(ages));          // 2
```

Keys are `str` or `int`. Values are anything — ints, floats, strings,
structs, lists, even other maps.

## Reading, and the missing-key rule

Reading a key that isn't there is a mistake, and Simple treats it the
way it treats reading past the end of an array — it stops your program
with a clear message:

```simp
print(ages["nobody"]);
// runtime error: key not found: nobody
```

No null, no accidental zero that pretends to be data. If a key might be
absent, ask first:

```simp
if (has(ages, "ada")) {
    print(ages["ada"]);
}
```

This makes the counting idiom explicit:

```simp
if (has(counts, w)) { counts[w] = counts[w] + 1; }
else                { counts[w] = 1; }
```

(Two probes where one might do? The runtime notices: a successful
`has` remembers where it found the key, and the `m[k]` on the next
line reuses that position. You write the honest version; the map makes
it fast.)

## Deleting

`del(m, k)` removes a key. Deleting a key that isn't there is a quiet
no-op — the map ends up in the same state either way, so there's
nothing to trap:

```simp
del(ages, "grace");
print(len(ages));          // 1
```

## Iteration: insertion order, everywhere, forever

```simp
let mut m = map str int;
m["b"] = 2; m["a"] = 1; m["c"] = 3;
for (k in m) {
    print(k + "=" + str(m[k]));
}
// b=2   a=1   c=3 — in that order, on every machine, every run
```

Most languages give you hash-table order — which changes between runs
(Go randomizes it on purpose). Simple promises **insertion order**,
because a program that prints different output on different days isn't
deterministic, and determinism is a core promise. This costs nothing:
the map keeps its entries in arrival order internally.

One subtlety, handled: if the loop body *mutates* the map, the
iteration keeps walking the map **as it was when the loop started**.
Insertions and deletions during the loop take effect — you'll see them
after — but the walk itself is stable. No iterator invalidation, no
undefined behavior, no rule to memorize.

## Value semantics, of course

A map is a value, like everything in Simple:

```simp
let mut mine = map str int;
mine["x"] = 1;
let yours = mine;          // yours is its own map (copied lazily)
mine["x"] = 100;
print(yours["x"]);         // 1 — unaffected
```

Passing a map to a function, storing it in a struct, sending it down a
channel to another thread — all follow the rules you already know.
Nothing is ever shared mutable.

## Under the hood (one paragraph)

The map is an insertion-ordered entry array plus a compact index —
the same layout Python discovered makes dicts both fast and ordered.
Hashing is pure integer arithmetic (FNV-1a for strings), so a map
behaves *identically* on every architecture. The whole thing is ~370
lines of generated IR; the perf lab's `mapbench` (11.5 million mixed
operations) has Simple's map beating Go's and within 10% of Rust's —
see `perf/results/` for the honest numbers.

---

**Next:** [Modules →](15-modules.md)
