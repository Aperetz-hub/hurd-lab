# riscv64/ — bare-metal rv64gc kernel skeleton for GNU/Hurd

Experimental target: a **64-bit RISC-V microkernel skeleton** that boots on the
QEMU `virt` board under OpenSBI. It is the smallest thing that proves the whole
bring-up chain works end to end — firmware handoff, S-mode entry, SBI console
output — so that everything above it can be built on a foundation that is known
to reach `main`.

It is written in **freestanding C and GNU assembler**, and built with the same
kind of toolchain GNU Mach itself uses: `riscv64-linux-gnu-gcc`, `gas`, `ld`
and `make`. There is no other build system involved.

> Status: **milestone 3 boots** (18/10/2026). The kernel switches the MMU on
> in Sv39 mode with per-section permissions, takes five supervisor timer
> interrupts *with the MMU live*, and reports milestone 3. `make verify` checks
> it at `-smp` 1, 2, 3 and 4, and `make selftest` checks the fault paths on
> purpose. See "Boot log" below for the captured output, and `docs/suivi.md`
> for what was measured, what was fixed and what is still unknown. See
> `docs/roadmap-riscv.md` for the toolchain setup.

## What is in here

```
riscv64/
├── Makefile                         check / build / run / verify / debug / disasm
├── include/
│   ├── console.h                    console multiplexer API
│   ├── csr.h                        supervisor CSR accessors
│   ├── linker.h                     the symbols the linker script exports
│   ├── pmm.h                        the physical page allocator
│   ├── sbi.h                        SBI ecall wrappers, struct sbiret
│   ├── timer.h                      S-mode timer
│   ├── trap.h                       struct trap_frame, the C handler
│   ├── trap_offsets.h               frame layout, shared with the assembly
│   ├── uart.h                       NS16550A driver
│   └── vm.h                         Sv39 page tables
├── linker/
│   └── riscv64-virt.ld              image layout for the QEMU virt board
└── src/
    ├── boot.S    _start trampoline: gp, .bss, call to C
    ├── trap.S    S-mode trap entry: save the frame, call C, sret
    ├── main.c    kmain(), the milestone 3 sequence
    ├── trap.c    the C trap handler
    ├── timer.c   the S-mode timer and the tick counter
    ├── pmm.c     the 4 KiB physical page allocator
    ├── vm.c      the Sv39 page tables and the MMU switch-on
    ├── console.c console multiplexer + allocation-free hex/decimal
    ├── sbi.c     ecall wrappers, base and TIME extensions
    └── uart.c    polled NS16550A driver for the virt board
```

## Build

```sh
make check      # toolchain present?
make build      # -> build/hurd-riscv64-boot.elf
make run        # boot it, headless, serial on stdout
make verify     # boot at -smp 1..4 and require the milestone banner once
make selftest   # check the fault paths: illegal instruction, and a store to
                # the read-only data that must raise a store page fault
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

## SBI: the error and the value are in different registers

The SBI specification (v3.0, section "Binary Encoding") is explicit:

> "SBI functions must return a pair of values in a0 and a1, with a0 returning
> an error code. This is analogous to returning the C structure
> `struct sbiret { long error; union { long value; unsigned long uvalue; }; }`"

So `a0` is the error code (0 == `SBI_SUCCESS`) and `a1` carries the answer.
An earlier version of this skeleton read `a0` only, which is why it printed
"SBI spec version 0.0" and "SBI impl id 0x0": the calls were *succeeding*, and
the answers were arriving in `a1`, which the wrapper threw away.

`include/sbi.h` now returns the pair, and the boot log shows both parts so a
mismatch is visible rather than silent.

The legacy extensions are the exception: the specification says "nothing is
returned in a1" and "the value returned in a0 register is SBI legacy extension
specific", so the legacy console has its own wrapper. The earlier code
addressed the legacy console as if it were a base-extension call (`a7` = 0x10,
`a6` = 0x00), which is `sbi_get_spec_version`: the character was never sent,
and the call "succeeded" because the error code of a different function was
zero. The `!` in the boot log below is that character arriving for the first
time.

## The S-mode trap vector

`src/trap.S` is installed in `stvec` in direct mode. It saves the interrupted
context in a frame on the interrupted stack, calls `trap_handler()` with a
pointer to that frame, restores everything, and returns with `sret`.

The frame layout lives in `include/trap_offsets.h` and is used by *both* the
assembly and the C struct in `include/trap.h`, which then checks every member
offset with `_Static_assert`. A mismatch between the two sides is a build
error, not a trap that silently corrupts the stack.

Two facts about this platform make the layout work:

- the `ecall` from S-mode is **not** delegated to S-mode (`medeleg` bit 9 is
  clear), so the SBI call path traps straight to M-mode and never touches the
  S-mode `stvec`. Installing our own vector therefore does not break the SBI
  calls;
- `menvcfg` and `mideleg` are machine-mode CSRs, so they cannot be read from
  S-mode at all. Reading `menvcfg` raises an illegal instruction, which is
  how that was found. The delegation state therefore has to be established by
  its effect — does the tick arrive? — and not by reading a register.

`make selftest` builds two variants and checks both fault paths:

- **exception** — raises a deliberate illegal instruction right after the
  vector is installed; the handler must catch it and report `scause`, `sepc`
  and `stval`;
- **rodata** — stores to the read-only data once the MMU is live. The page is
  mapped `R--`, so the store must raise a **store/AMO page fault**
  (`scause = 15`). If the permissions were wrong the store would simply
  succeed and the test would fail by reaching the code after it.

The second one is the test that the Sv39 permissions are real, and not just
a table that happens to be installed.

## The S-mode timer

The hart advertises the `sstc` extension, so `stimecmp` can be written
directly from S-mode. The kernel reads `time` with `rdtime`, adds an interval
of 100000 ticks (10 ms on the 10 MHz timebase the board reports) and writes
the deadline, then enables `sie.STIE` and `sstatus.SIE`.

The SBI TIME extension is asked first (`sbi_timer_set_stimecmp`, FID #7) and
the direct `stimecmp` write is the fallback; the boot log says which one was
used and why. The rearm inside the trap handler always uses the CSR, so no
`ecall` is made in interrupt context.

The measured interval between the first two ticks is 100698 to 101739 ticks
over five runs, against a programmed 100000: the excess is the time between
the interrupt being taken and the rearm write, about 70 to 170 microseconds.

## The physical page allocator

`src/pmm.c` hands out 4 KiB pages from a fixed pool of 64 pages that the
linker script reserves between `_kernel_end` and `_pmm_pool_end`. The pool is
static on purpose: discovering memory at boot needs the device tree, and a
fixed pool is enough for the page tables of the first milestones.

Every page is handed out **4 KiB aligned** and **zeroed**. The zeroing matters
for a page table in particular: a stale entry in a fresh table would be a
mapping the kernel never intended to make.

The pool size is fixed in two places — `PMM_POOL_PAGES` in `include/pmm.h` and
`PMM_POOL_PAGES` in the linker script — and the kernel prints the size the
script actually reserved, so a mismatch is visible at boot.

## Sv39 and the page permissions

`src/vm.c` builds a three-level Sv39 page table and switches the MMU on with
`satp = (8 << 60) | root_ppn` followed by `sfence.vma`. The mapping is an
identity mapping, because the kernel keeps running at its physical address.
What the tables buy is *permissions*, which a flat mapping cannot express:

| region | permissions |
|---|---|
| `[__text_start, __text_end)` | `R-X` |
| `[__rodata_start, __rodata_end)` | `R--` |
| `[__data_start, _kernel_end)` — `.data`, `.bss`, boot stack | `RW-` |
| `[_kernel_end, _pmm_pool_end)` — the page pool | `RW-` |
| `[0x1000_0000, 0x1000_1000)` — the NS16550A | `RW-` |

The section boundaries in the linker script are padded to 4 KiB page
boundaries, which is what makes those permissions possible: without the
padding, the read-only data would share a page with the code and the page
would have to be executable, or the code would have to be writable.

`.data` and `.bss` are deliberately *not* separated by a page gap. They hold
the small-data objects that the RISC-V `gp` convention addresses relative to
`__global_pointer$`, and keeping them adjacent preserves that; the assembler
resolves the distance and refuses to relax an access that does not fit, so a
mistake here is a link error rather than a wrong address.

The UART is mapped explicitly. It is MMIO and is not part of the kernel
image, and an access to an unmapped address is a page fault rather than a
silent no-op.

## The timer does not drift

Scheduling the next interrupt as `rdtime() + interval` makes every tick late
by however long the handler takes — the UART writes and the rearm itself —
and the error accumulates. Instead the deadline is advanced by exactly one
interval:

```c
if (next_deadline + interval <= now)
        next_deadline = now + interval;   /* fell too far behind */
else
        next_deadline += interval;
```

The boot log prints the average period over the ticks so far and the offset
of the deadline schedule, which is the number that shows whether the error
accumulates or stays bounded. Measured over three runs, with an interval of
100000 ticks:

| scheme | average period over 5 ticks |
|---|---|
| `now + interval` (drifting) | 100989, 100824, 101046 — always *above* the target |
| `next_deadline += interval` | 99214, 99226, 99730 — at or below, converging to it |

The deadline offsets printed in the log advance by exactly 100000 every tick
(`+96360`, `+196360`, `+296360`, `+396360`): the constant offset is the
lateness of the first tick, and the steps between the offsets are the
interval, exactly.

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
!
SBI legacy_putc works: yes
SBI spec version 2.0 (error 0, value 0x2000000)
SBI impl id 0x1 (error 0)
SBI impl version 0x10006 (error 0)
SBI extension probe base eid 0x10 -> 1 (error 0)
SBI extension probe legacy console eid 0x1 -> 1 (error 0)
SBI extension probe time eid 0x54494d45 -> 1 (error 0)
SBI wall clock 0 (error 0)
hurd-riscv64: milestone 1 reached
pmm: pool 64 pages of 4 KiB
pmm: allocated page at 0x80214000, aligned=0x1, free now 63
firmware stvec was 0x80200000
installed stvec 0x80201d20
hurd-riscv64: milestone 2 reached
timer armed by stimecmp CSR at 911911 (SBI sbi_timer_set_stimecmp error -2)
vm: root table ppn 0x80215, 85 pages mapped, 5 pool pages used
vm: Sv39 active, satp 0x8000000000080215
timer tick 1 at 915551
timer tick 2 at 1012955 (period 97404 ticks, deadline +96360)
timer tick 3 at 1112692 (period 98570 ticks, deadline +196360)
timer tick 4 at 1215265 (period 99904 ticks, deadline +296360)
timer tick 5 at 1312438 (period 99221 ticks, deadline +396360)
hurd-riscv64: milestone 3 reached, halting
```
The `!` on its own line is the character the legacy console extension
produced, which the earlier implementation never actually sent.

The firmware's own `stvec` was `0x80200000`, which is `_start`: a trap taken
before our vector is installed would have jumped back to the entry point.

`SBI hart id` is the SBI boot-protocol hart id read out of `a0`. It was
added in this pass precisely so that the "a0 is the hart id" claim is
measured rather than assumed. The value is not always `0x0`: it is the id of
whichever hart the firmware booted, so the capture above is one run among
several. `make verify` automates the check that the milestone banner appears
exactly once per boot, at every hart count, over several runs.

## Known limitations, not fixed here

- **Only the boot hart runs the kernel.** The other harts are held by the
  firmware and never reach `_start`; nothing in this skeleton starts them.
  The timer therefore ticks on one hart only. "The second CPU works" is still
  an unproven claim, exactly as on the x86_64 side.
- **The firmware does not implement `sbi_timer_set_stimecmp`.** The call
  returns `SBI_ERR_NOT_SUPPORTED` (-2), so the first deadline is armed by
  writing the `stimecmp` CSR directly. The SBI TIME extension *is* present
  and *is* used, for the wall clock and for the extension probe.
- **The firmware does not implement `sbi_timer_timebase`.** Also -2. The
  10 MHz timebase is therefore taken from the board's own description
  (`aclint-mtimer @ 10000000Hz`) rather than from the firmware, and the
  measured interval is printed so the assumption can be checked.
- **The delegation state cannot be read from S-mode.** `menvcfg` and `mideleg`
  are machine-mode CSRs; reading `menvcfg` traps. The timer therefore relies
  on the tick arriving, which is the only observation available from S-mode.
- **Any exception is fatal.** The handler reports `scause`, `sepc` and `stval`
  and stops the hart. There is no process to kill and no other hart to report
  to, so there is nothing else to do — but a kernel that can recover is the
  obvious next step.
- **`stimecmp` requires the firmware to have set `menvcfg.STCE`.** If it had
  not, the write would raise an illegal instruction; the handler would report
  it, so the failure would be visible rather than a silent hang.
- **The trap frame is allocated on the interrupted stack.** That is enough for
  one hart and one trap at a time, and it keeps the design small, but a
  per-hart trap stack (`sscratch`) would be the right thing once more harts
  exist.
- **The MMU maps only what the kernel uses.** The identity mapping covers the
  kernel image, the page pool and the UART page. The rest of DRAM is not
  mapped at all, so an access to it is a page fault rather than a surprise.
  There is no user space, and no attempt is made to reserve anything.
- **The physical pool is a fixed 64 pages.** That is 256 KiB, enough for the
  five page tables the current mapping needs with a wide margin, and not
  enough for anything real. Growing it means either a bigger static pool or
  a way to discover the memory the firmware had, which needs the device tree.
- **There is no `sfence.vma` with operands.** The whole address space is
  flushed on the MMU switch-on, which is correct but coarse. A per-address
  flush will be needed once mappings change while the MMU is on.
