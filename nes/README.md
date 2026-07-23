# NES emulator, written in Simple

A Nintendo Entertainment System emulator built in [Simple](../README.md) — the
project's biggest real-world stress test, and the road toward a GBA emulator
(the GBA literally contains a Game-Boy-class CPU, so this ladder reuses itself:
6502 today → the CPU discipline scales up).

## Architecture: why it's `unsafe`

An emulator is a **shared mutable arena** — the CPU, PPU, DMA, and timers all
read and write the same memory millions of times per second. Simple's safe
subset can't express that cheaply: it has no globals, parameters are immutable,
and value semantics copy a buffer whenever you thread it through a function
(measured: **22.9s** vs **0.52s** for the raw-pointer version on 2M accesses).

So all mutable state lives in `unsafe` `malloc`'d byte buffers reached through
raw pointers — exactly like C, and exactly what `examples/plasma.simp` does for
its framebuffer. This is a deliberate, documented trade-off; the language-level
fix (uniqueness-based in-place mutation, so this could be *safe* and just as
fast) is tracked as **issue #13** in the main repo. When it lands, the core
moves back to safe Simple with no perf loss.

State is split into logical pointer buffers (`cpu`, `ram`, `prg`, `chr`,
`ppu`, …) passed to the functions that need them.

## Build order

- [x] **6502 CPU core** — fetch/decode/execute, the common opcode set,
      addressing modes, flags, stack, branches, `JSR`/`RTS`. Tested with
      hand-crafted programs (`test_cpu.simp`).
- [ ] **Complete the opcode table** — all 151 official opcodes + addressing
      matrix, then validate against `nestest.nes` (the golden-log CPU test).
- [ ] **Bus + iNES loader + mappers** — parse `.nes`, PRG/CHR-ROM, NROM (0)
      then MMC1/UxROM/CNROM/MMC3.
- [ ] **PPU (2C02)** — background + sprites, scanline/dot timing, scrolling,
      sprite-0 hit, NMI on vblank. (The hard part.)
- [ ] **Input** — the two controller shift registers (`$4016`/`$4017`).
- [ ] **APU** — 2 pulse, triangle, noise, DMC.
- [ ] **Display / audio / input frontend** — SDL2 window (like plasma), or
      headless PPM frames for deterministic testing first.

## Files

| file | what |
|------|------|
| `cpu.simp` | the Ricoh 2A03 (6502) CPU core + CPU bus |
| `test_cpu.simp` | hand-crafted 6502 programs that check the core |

## Running the CPU tests

```
simplec nes/test_cpu.simp -o /tmp/testcpu && /tmp/testcpu
# expect: 55, 63, 2, 64, 64
```

## Status

CPU core: a validated subset runs real 6502 programs correctly. Next up is
completing the opcode table and standing it against `nestest`.
