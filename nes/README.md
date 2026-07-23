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

- [x] **6502 CPU core** — all **151 official opcodes** with the full
      addressing-mode matrix (imm, zp, zp/X, zp/Y, abs, abs/X, abs/Y,
      `(ind,X)`, `(ind),Y`, indirect JMP with the page-boundary bug), flags,
      stack, branches, `JSR`/`RTS`/`RTI`. Self-tested by `test_cpu.simp`
      (10 checks across every mode, all passing).
- [x] **iNES loader + `nestest` harness** — `ines.simp` parses `.nes` and
      loads PRG/CHR; `nestest.simp` runs a CPU test ROM from `$C000`, traces
      each instruction in `nestest.log` format, and reports the result codes.
      Drop in `nestest.nes` to validate against the canonical log.
- [ ] **Cartridge mappers** — NROM (0) works; add MMC1/UxROM/CNROM/MMC3.
- [ ] **Real BRK/IRQ/NMI** — BRK currently halts the test harness; wire the
      interrupt sequence for PPU-driven NMI.
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
| `ines.simp` | iNES (`.nes`) cartridge loader |
| `nestest.simp` | run + trace a CPU test ROM |

## Running the tests

```
simplec nes/test_cpu.simp -o /tmp/testcpu && /tmp/testcpu
# expect: 55 63 2 64 64  171 171  2 1  119

# validate the CPU against the gold-standard ROM (download nestest.nes first):
simplec nes/nestest.simp -o /tmp/nt
/tmp/nt nestest.nes            # -> result $02 = 00 / $03 = 00  means all passed
/tmp/nt nestest.nes trace      # -> per-instruction log to diff vs nestest.log
```

## Status

**The 6502 passes `nestest`.** Running the canonical CPU test ROM, this core
matches the reference Nintendulator log **exactly for all 5003 official-opcode
instructions** — every PC, A, X, Y, P, and SP identical — and the ROM's
`$02`/`$03` result bytes both read `00` (the pass code). It stops only where
nestest moves on to *unofficial* opcodes. The CPU is instruction- and
flag-exact.

The PPU has its **tile decoder** and **background renderer**, and the CPU now
**drives the PPU**: `busrd`/`buswr` route `$2000`-`$2007` (PPUCTRL/MASK/STATUS/
SCROLL/ADDR/DATA), OAMDATA, and `$4014` OAM DMA into a single machine-memory
buffer that also holds VRAM/OAM; an `nmi` sequence is ready. `ppu_test.simp`
proves it end-to-end — a 6502 program writes tile indices through `$2006`/
`$2007`, they land in nametable memory, and the frame renders. The
nestest-validated CPU is untouched (it never accesses `$2000`-`$3FFF`).

And a **NES program now boots and draws its own screen.** `nes.simp` runs an
NMI-driven frame loop (visible period with vblank clear, then vblank with the
flag set and NMI fired); `ppu.simp` renders the background in full colour via
the attribute table, palette RAM, and the 64-colour NES master palette. A
self-contained test ROM — reset handler waits for vblank, loads a palette,
enables NMI + rendering; NMI handler draws the nametable each frame — boots and
renders an authentic NES frame (blue background, white/cyan text).

**Sprites** render too: `render_sprites` draws the 64 OAM sprites (palette,
H/V flip, back-to-front) over the background. The test ROM places four sprites
via `$2003`/`$2004` and they appear as "ABCD" over the scene.

**Input** works too: `$4016` implements the controller strobe + shift register,
and `set_buttons` lets a frontend feed key state (a test reads B pressed / A up
correctly). The core is now everything a game needs — CPU, PPU (bg + sprites),
NMI timing, and controllers.

Next: an **SDL2 live window** (real-time display + keyboard → `set_buttons`,
like `examples/plasma.simp`), cycle-accurate timing so commercial ROMs boot
cleanly, more **mappers**, and the **APU** for sound.
