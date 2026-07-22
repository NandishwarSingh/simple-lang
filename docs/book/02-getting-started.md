# 2. Getting Started

## Installing

You need three things (macOS shown; Linux is the same idea):

1. A C++ compiler and `make` — on macOS these come with the Xcode command
   line tools (`xcode-select --install`).
2. **QBE**, the compiler backend Simple uses: `brew install qbe`
3. The Simple compiler itself. From the repository root:

```
make
```

That produces `./simplec`, the Simple compiler. Check it works:

```
./simplec
```

You should see the usage message.

## Your first program

Create a file called `hello.simp`:

```simp
// Your first Simple program.
fn main() {
    print("hello, world");
}
```

Two things to notice:

- Every program starts at `fn main()` — the compiler refuses to build a
  program without one.
- `//` starts a comment; the compiler ignores the rest of the line.
  (`/* ... */` comments spanning multiple lines work too.)

## Compile and run

```
$ ./simplec run hello.simp
hello, world
```

`run` compiles your program and immediately executes it. To just compile:

```
$ ./simplec hello.simp
hello
$ ./hello
hello, world
```

The compiler created **three files**:

| file        | what it is                                             |
|-------------|--------------------------------------------------------|
| `hello`     | your native executable — real machine code             |
| `hello.ssa` | the intermediate code Simple generated (QBE IR)        |
| `hello.s`   | the assembly QBE generated from that                   |

The `.ssa` and `.s` files are kept on purpose. You'll never *need* them, but
peeking inside is the best way to learn what a compiler actually does —
chapter 8 walks through them.

## The compiler commands

```
./simplec program.simp             compile → ./program
./simplec program.simp -o name     compile → ./name
./simplec run program.simp         compile and run now
./simplec program.simp --emit-ssa  print the intermediate code, build nothing
./simplec program.simp --tokens    print what the lexer sees (debug)
```

## When things go wrong

Delete the `;` in hello.simp and recompile:

```
hello.simp:4: error: expected ';', found '}'
```

Every error names the file, the line, and what the compiler expected. Simple's
errors are designed to be read, not decoded — chapter 7 is a guided tour of
them.

**Next:** [Variables and Types →](03-variables-and-types.md)
