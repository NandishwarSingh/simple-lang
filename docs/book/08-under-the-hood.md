# 8. Under the Hood

You don't need this chapter to *use* Simple — but you're learning a systems
language, and systems programmers know what their tools do. Let's watch a
program become machine code.

## The journey of a .simp file

```
count.simp
   │  lexer        — text → tokens
   │  parser       — tokens → syntax tree (AST)
   │  type checker — proves the tree makes sense; catches ch.7's errors
   │  codegen      — tree → QBE intermediate code     (count.ssa)
   │  QBE          — intermediate code → assembly      (count.s)
   │  cc           — assembly → linked executable      (count)
   ▼
./count
```

The first four stages *are* the Simple compiler (`simplec`). QBE is a small
open-source backend that handles the truly gnarly parts — register
allocation, instruction selection — and `cc` does the final assembly and
linking against the system.

## Seeing each stage

Take this program:

```simp
fn main() {
    let mut i = 0;
    while (i < 3) {
        i = i + 1;
    }
    print(i);
}
```

**Stage 1 — tokens.** `./simplec count.simp --tokens` shows the source
chopped into words:

```
2: 'let'
2: 'mut'
2: identifier "i"
2: '='
2: integer literal 0
2: ';'
...
```

**Stage 2–4 — intermediate code.** `./simplec count.simp --emit-ssa` prints
the QBE IR your compiler generated:

```
export function w $main() {
@start
	%i_1 =l alloc8 8            # make stack space for i
	storel 0, %i_1              # i = 0
	jmp @while_cond_1
@while_cond_1
	%t1 =l loadl %i_1           # read i
	%t2 =w csltl %t1, 3         # t2 = (i < 3)
	jnz %t2, @while_body_2, @while_end_3
@while_body_2
	%t3 =l loadl %i_1
	%t4 =l add %t3, 1           # t4 = i + 1
	storel %t4, %i_1            # i = t4
	jmp @while_cond_1           # back to the condition
@while_end_3
	%t5 =l loadl %i_1
	call $printf(l $fmt_int, ..., l %t5)
	ret 0
}
```

Look how literal it is: your `while` loop became three labeled blocks and
some jumps. **All loops, in every language, are secretly jumps.** Now you've
seen it.

**Stage 5 — assembly.** After compiling, open `count.s`: that's ARM64 (or
x86) assembly, the actual instructions your CPU executes, with registers
instead of named variables.

## Why this design is fast enough

You might notice the IR reads and writes `i` from memory constantly. That's
deliberate — it keeps our compiler simple — and QBE's optimizer promotes
those memory slots into CPU registers automatically. We write clear code,
QBE makes it fast.

And since v0.5, Simple has its own optimizer *above* QBE: it inlines small
functions into their callers, turns division by powers of two into bit
shifts, folds constants, and deletes dead code — the things that require
knowing the *language*, not the machine. It's always on; compile with
`--no-opt` to see the naive translation and compare. (Try both on a
program with `n % 2` in a loop and diff the `.ssa` files — the division
vanishes.)

Want the full story — how the parser works, what the AST looks like, why
variables get stack slots? The **[internals book](../internals/README.md)**
documents the whole compiler, file by file.

**Next:** [Structs, Arrays, and Strings →](09-structs-arrays-strings.md)
