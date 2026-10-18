# riscv64/ — bare-metal rv64gc kernel skeleton for GNU/Hurd

Experimental target: a **64-bit RISC-V microkernel skeleton** that boots on the
QEMU `virt` board under OpenSBI. It is the smallest thing that proves the whole
bring-up chain works end to end — firmware handoff, S-mode entry, SBI console
output — so that everything above it can be built on a foundation that is known
to reach `main`.

It is written in **freestanding C and GNU assembler**, and built with the same
kind of toolchain GNU Mach itself uses: `riscv64-linux-gnu-gcc`, `gas`, `ld`
and `make`. There is no other build system involved.

> Status: **milestone 1 boots** (18/10/2026). `make build` then
> `make run` produces the kernel's first console line on QEMU `virt` under
> OpenSBI v1.6, and `make verify` checks it at `-smp` 1, 2, 3 and 4. See
> "Boot log" below for the captured output, and `docs/suivi.md` for what was
> measured, what was fixed and what is still unknown. See
> `docs/roadmap-riscv.md` for the toolchain setup.

## What is in here

```
riscv64/
├── Makefile                         check / build / run / verify / debug / disasm
├── include/
│   ├── console.h                    console multiplexer API
│   ├── sbi.h                        SBI ecall wrappers
│   └── uart.h                       NS16550A driver
├── linker/
│   └── riscv64-virt.ld              image layout for the QEMU virt board
└── src/
    ├── boot.S    _start trampoline: hart parking, gp, .bss, call to C
    ├── main.c    kmain(), the milestone 1 sequence
    ├── console.c console multiplexer + allocation-free hex/decimal
    ├── sbi.c     ecall wrappers, console_putc, spec_version, impl_id
    └── uart.c    polled NS16550A driver for the virt board
```

## Build

```sh
make check      # toolchain present?
make build      # -> build/hurd-riscv64-boot.elf
make run        # boot it, headless, serial on stdout
make verify     # boot at -smp 1..4 and require the milestone banner once
make disasm     # program headers, entry point, first instructions
```

The kernel is linked freestanding: `-ffreestanding -nostdlib -nostartfiles`,
no PIC (the image lives at a fixed address), no unwind tables, and our own
linker script. `-mcmodel=medany` is required because the image is at
`0x8020_0000`, above the signed 2 GiB reach of the `medlow` default.

## Boot contract

| stage | who | what happens |
|---|---|---|
| reset | QEMU + OpenSBI (`-bios default`) | M-mode firmware, sets up a stack, enters S-mode at `0x80200000` |
| `_start` | `src/boot.S` | parks non-zero harts, loads `gp`, sets `sp = __stack_top`, zeroes `.bss` |
| `kmain` | `src/main.c` | initialises the console, exercises SBI, reports the spec version, parks the hart |

`_start` is a separate `.S` file because it must run before any C code
exists: it is the only place where the ABI is set up by hand.

The hart id comes from `a0`, which is what the SBI boot protocol passes at the
S-mode entry point. `kmain` takes it as its first argument, so it is captured
before any C code can clobber it. Do **not** read `mhartid` for this: it is a
machine-mode CSR, and reading it from S-mode raises an illegal instruction
exception, so the hart traps before it can say anything.

Only the boot hart ever reaches `_start`: with `-smp` 1, 2, 3 and 4, exactly
one hart enters the kernel, and the others are held by the firmware. The boot
hart is **not necessarily hart 0** — the firmware picks whichever hart wins the
race to the reset vector, and on the same host the banner reported
`Boot HART ID: 0`, `1` and `2` on different runs. There is therefore no "park
the other harts" loop in the boot path; a loop of the form `bnez a0, 1b` would
park the boot hart itself whenever the firmware did not pick hart 0, and the
kernel would never start. `make verify` boots each hart count several times
for exactly that reason.

## Console: two sinks, one API

`console_putc` talks to whichever sink is known to be alive:

1. **UART** (NS16550A at `0x1000_0000`) after `console_init()` — this is what
   QEMU forwards to `-serial stdio` / `-nographic`, and it is roughly an order
   of magnitude faster than the firmware console;
2. **SBI legacy console** (`legacy_putch`, extension `0x01`) as the fallback,
   including inside any diagnostic path.

We deliberately do **not** "probe" the UART: on real hardware a write to a
missing device is silently dropped, so a probe tells you nothing. `uart_ready`
is set by `console_init()` and SBI is the safety net.

OpenSBI marks `legacy_putch` deprecated in favour of the debug-console (`DBCN`)
extension, which needs device-tree parsing. That is a milestone, not a blocker.

## Boot log (captured 18/10/2026)

```
$ cd riscv64 && make build
$ timeout 30 qemu-system-riscv64 -machine virt -m 256M -nographic -no-reboot \
      -bios default -kernel build/hurd-riscv64-boot.elf
```

Last lines of the captured console (everything above them is OpenSBI's own
banner, which ends with `Domain0 Next Address: 0x0000000080200000`,
`Domain0 Next Mode: S-mode`):

```
hurd-riscv64: console up (rv64gc, QEMU virt)
SBI hart id 0x0

SBI legacy_putc works: yes
SBI spec version 0.0
SBI impl id 0x0
hurd-riscv64: milestone 1 reached, halting
```

`SBI hart id` is the SBI boot-protocol hart id read out of `a0`. It was
added in this pass precisely so that the "a0 is the hart id" claim is
measured rather than assumed. The value is not always `0x0`: it is the id of
whichever hart the firmware booted, so the capture above is one run among
several. `make verify` automates the check that the milestone banner appears
exactly once per boot, at every hart count, over several runs.

## Known defects, not fixed here

Found while bringing this up; both are recorded rather than silently repaired,
because the bring-up brief was to fix only what blocked the first boot.

- **The SBI base-extension calls return 0.** `sbi_get_spec_version()` returns
  0 although OpenSBI reports `Runtime SBI Version : 2.0`, and
  `sbi_get_impl_id()` returns 0; `legacy_putch` also reports success while the
  `!` it was asked to emit does not appear anywhere in the captured console.
  The disassembly shows spec-conformant calls (`li a7,16` with `li a6,0` or
  `li a6,1` and a single `ecall`), so the calling convention is right and the
  cause is not established. Consequently the skeleton's own string "SBI
  legacy_putc works: yes" is a claim the capture does not corroborate.
- **There is no trap handler.** `stvec` is never programmed, so an exception
  in S-mode is fatal and silent. A future milestone needs one; until then, a
  trap looks exactly like a hang.
- **Only the boot hart runs the kernel.** The other harts are held by the
  firmware and never reach `_start`; nothing in this skeleton starts them.
  That is deliberate for milestone 1, but it means "the second CPU works" is
  still an unproven claim, exactly as on the x86_64 side.
