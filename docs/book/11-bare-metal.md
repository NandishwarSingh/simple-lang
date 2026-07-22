# 11. Bare Metal

*(New in v0.6.)* Everything so far has been safe. This chapter is where
Simple hands you the tools to talk to hardware, call C libraries, and
touch memory directly — the abilities that make a language a *systems*
language.

## Sized integers

`int` is 64-bit, which is right for counting things. But a network
packet, a hardware register, or a file header describes memory with
exact widths. Simple has the full set:

| signed | unsigned | bits |
|--------|----------|-----:|
| `i8`   | `u8`     | 8    |
| `i16`  | `u16`    | 16   |
| `i32`  | `u32`    | 32   |
| `i64`  | `u64`    | 64   |

(`int` is another name for `i64`.)

Each type wraps at its own width — no surprises, no undefined behavior:

```simp
let a: u8 = 200;
print(a + 100);        // 44, because u8 stops at 255
let b: i8 = 100;
print(b + b);          // -56
```

Mixing widths is a compile error, and there are no implicit conversions.
Convert explicitly by writing the type as a function:

```simp
let n = 300;
print(u8(n));          // 44
print(u32(0 - 1));     // 4294967295
```

The one convenience: **number literals adapt to their context**, so you
write `let x: u8 = 5;` and `flags & 1` without any casts. Only literals
do this — a value with a type never converts behind your back.

## Bitwise operators

```simp
print(flags & 0x30);   // and
print(flags | 0x0F);   // or
print(flags ^ 0xFF);   // xor
print(1 << 8);         // shift left
print(256 >> 4);       // shift right
print(~0);             // not (-1)
```

One deliberate difference from C: `&` binds like `*` and `|` binds like
`+`. In C, `a & b == c` secretly means `a & (b == c)` — a bug so common
it has its own compiler warning. In Simple it means what it looks like.

## Structs are hardware layouts

Fields are laid out in order, each aligned to its own size — so a struct
describes memory the way the hardware sees it:

```simp
struct Packet {
    version: u8,     // offset 0
    flags: u8,       // offset 1
    length: u16,     // offset 2
    id: u32,         // offset 4
}                    // 8 bytes total
```

## Calling C

```simp
extern fn puts(s: str) -> i32;
extern fn malloc(size: u64) -> *u8;
extern fn free(p: *u8);

fn main() {
    puts("hello from C");
}
```

`extern fn` declares something implemented outside Simple. That's the
whole feature — and it means every C library on your machine is
available today, including the operating system's own APIs.

## `unsafe` and raw pointers

Some things can't be proven safe. Reading a device register, walking
memory returned by `malloc`, implementing an allocator — these need
naked pointers. Simple allows them only inside `unsafe`:

```simp
let mut x = 1234;
unsafe {
    let p = &x;        // address of x, type *int
    print(*p);         // read through the pointer
    *p = 5678;         // write through it
}
print(x);              // 5678
```

Pointer arithmetic scales by the pointee's size, as in C:

```simp
unsafe {
    let buf = malloc(16);
    *buf = 65;
    *(buf + 1) = 66;   // next byte
    free(buf);
}
```

Two more things exist only inside `unsafe`. **Any pointer can stand in
for any other**, because C APIs traffic in `void*` and demanding a cast
would buy nothing where the compiler has already stopped vouching for
you:

```simp
let mut bytes = [u8(0); 16];
unsafe {
    let p: *u32 = &bytes[0];   // read four bytes as one number
    *p = 0xFFFF0000;
}
```

And **`null`** is the null pointer, for the many C functions that expect
one:

```simp
unsafe {
    SDL_RenderCopy(ren, tex, null, null);   // null means "the whole thing"
    let buf = malloc(64);
    if (buf == null) { return; }
}
```

Outside `unsafe`, `&` and `*` are simply not allowed:

```
error: taking an address with '&' is only allowed inside an `unsafe { }` block
```

The rule that keeps this honest: **the safe language never grows to
accommodate C.** Pointers, `null`, and free conversion between pointer
types all live behind `unsafe`, so chapters 1–10 never mention them and
a reader of ordinary Simple code never meets them.

**Why a block and not a keyword on the operation?** Because it makes the
dangerous parts of a codebase *greppable*. Search for `unsafe` and you
have the complete list of places where the compiler stopped helping. In
a well-built program that list is short, and every line in it deserves a
comment explaining why it's correct.

## Filling arrays

Writing out a large array is impractical, so:

```simp
let mut grid = [[0; 200]; 200];    // 200x200, all zeros
let mut flags = [true; 1000];
```

## Try it

1. Define a `struct Color { r: u8, g: u8, b: u8 }` and pack it into a
   `u32` with shifts and `|`, then unpack it again.
2. Use `extern fn` to call C's `abs` and `strlen`.
3. Allocate 8 bytes with `malloc`, write a pattern, read it back, `free`
   it — all inside one `unsafe` block.
4. Try `&x` outside `unsafe` and read the error.

**Next:** [Errors as Values →](12-errors-as-values.md)
