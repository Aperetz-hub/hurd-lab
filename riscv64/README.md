# riscv64/ — bare-metal rv64gc kernel skeleton for GNU/Hurd

Experimental target: a **64-bit RISC-V microkernel skeleton** that boots on the
QEMU `virt` board under OpenSBI. It is the smallest thing that proves the whole
bring-up chain works end to end — firmware handoff, S-mode entry, SBI console
output — so that everything above it can be built on a foundation that is known
to reach `main`.

It is written in **freestanding C and GNU assembler**, and built with the same
kind of toolchain GNU Mach itself uses: `riscv64-linux-gnu-gcc`, `gas`, `ld`
and `make`. There is no other build system involved.

> Status: **milestone 4 boots** (18/10/2026). The kernel switches the MMU on
> in Sv39 mode with per-section permissions, takes a **PLIC external
> interrupt** through the UART and claims and completes it, and **releases
> every other hart** through the SBI HSM extension: each secondary installs
> the shared page table, arms its own timer and takes its own timer interrupts
> with the MMU live. `make verify` requires that at `-smp` N exactly N harts
> report running, and `make selftest` checks the fault paths and the PLIC
> interrupt on purpose. See "Boot log" below for the captured output, and
> `docs/suivi.md` for what was measured, what was fixed and what is still
> unknown. See `docs/roadmap-riscv.md` for the toolchain setup.

## What is in here

```
riscv64/
├── Makefile                         check / build / run / verify / selftest / debug / disasm
├── include/
│   ├── console.h                    console multiplexer API
│   ├── csr.h                        supervisor CSR accessors
│   ├── hsm.h                        secondary hart bring-up
│   ├── hsm_offsets.h                the stack table layout, shared with boot.S
│   ├── linker.h                     the symbols the linker script exports
│   ├── pmm.h                        the physical page allocator
│   ├── plic.h                       the PLIC and the S-mode interrupt context
│   ├── sbi.h                        SBI ecall wrappers, struct sbiret
│   ├── timer.h                      S-mode timer
│   ├── trap.h                       struct trap_frame, the C handler
│   ├── trap_offsets.h               frame layout, shared with the assembly
│   ├── uart.h                       NS16550A driver
│   └── vm.h                         Sv39 page tables
├── linker/
│   └── riscv64-virt.ld              image layout for the QEMU virt board
└── src/
    ├── boot.S    _start and secondary_start trampolines
    ├── trap.S    S-mode trap entry: save the frame, call C, sret
    ├── main.c    kmain(), the milestone 4 sequence
    ├── trap.c    the C trap handler
    ├── timer.c   the S-mode timer, one set of state per hart
    ├── pmm.c     the 4 KiB physical page allocator
    ├── vm.c      the Sv39 page tables and the MMU switch-on
    ├── hsm.c     SBI HSM bring-up of the secondary harts
    ├── plic.c    the PLIC, and the external interrupt handler
    ├── console.c console multiplexer + allocation-free hex/decimal
    ├── sbi.c     ecall wrappers: base, TIME and HSM
    └── uart.c    NS16550A driver: polled console, receive interrupt
```

The PLIC self test needs a byte to arrive in the guest's UART, which the guest
cannot produce for itself; `../vm/riscv-uart-inject.py` injects one over the
serial socket, driven by a trigger line the kernel prints.

## Build

```sh
make check      # toolchain present?
make build      # -> build/hurd-riscv64-boot.elf
make run        # boot it, headless, serial on stdout
make verify     # boot at -smp 1..4 and require exactly N harts running
make selftest   # check, on purpose: an illegal instruction, a store to the
                # read-only data that must raise a store page fault, and a
                # PLIC external interrupt from a host-injected byte
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
| `_start` | `src/boot.S` | publishes the hart id in `tp`, loads `gp`, sets `sp = __stack_top`, zeroes `.bss` |
| `kmain` | `src/main.c` | console, SBI, trap vector, timer, Sv39 MMU, PLIC, then the secondary harts |
| `secondary_start` | `src/boot.S` | entered by the firmware through `sbi_hart_start`: takes a stack page from the pool, then `hsm_secondary_main` |

`_start` is a separate `.S` file because it must run before any C code
exists: it is the only place where the ABI is set up by hand.

The hart id comes from `a0`, which is what the SBI boot protocol passes at the
S-mode entry point. `kmain` takes it as its first argument, so it is captured
before any C code can clobber it. Do **not** read `mhartid` for this: it is a
machine-mode CSR, and reading it from S-mode raises an illegal instruction
exception, so the hart traps before it can say anything.

The firmware hands control to exactly one hart: with `-smp` 1, 2, 3 and 4,
exactly one hart enters `_start`, and the others are held by the firmware at
their S-mode entry point. The boot hart is **not necessarily hart 0** — the
firmware picks whichever hart wins the race to the reset vector, and on the same
host the banner reported `Boot HART ID: 0`, `1` and `2` on different runs. There
is therefore no "park the other harts" loop in the boot path; a loop of the form
`bnez a0, 1b` would park the boot hart itself whenever the firmware did not pick
hart 0, and the kernel would never start. `make verify` boots each hart count
several times for exactly that reason.

Since milestone 4 the kernel does not leave them held: it releases them itself,
through the SBI HSM extension, and they run the same code on the same page
table. See "Every hart runs the kernel" below.

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
| `[_kernel_end, _pmm_pool_end)` — the page pool, and the secondary stacks | `RW-` |
| `[0x1000_0000, 0x1000_1000)` — the NS16550A | `RW-` |
| `[0x0c00_0000, 0x0c00_2000)` — PLIC priorities and pending bits | `RW-` |
| `[0x0c00_2000, 0x0c00_3000)` — PLIC enable words, contexts 0..15 | `RW-` |
| `[0x0c20_0000, 0x0c20_4000)` — PLIC thresholds and claim/complete | `RW-` |

The PLIC windows are the largest non-kernel mapping, four pages, and they are
sized in `include/plic.h` rather than written out here: the mapping and the
driver have to agree on them, and two copies of that geometry is one more thing
to keep in step.

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

## Every hart runs the kernel

The firmware starts one hart and holds the rest. `sbi_hart_start` (HSM, function
0) is how a payload releases them, and it is how this kernel runs its own code
on every hart of the board. The boot hart gives each secondary a 4 KiB stack page
from the physical pool, publishes the `satp` value of the page table it installed,
and releases the harts one at a time; each secondary then installs the trap
vector, arms its own timer, writes the shared `satp` and enables interrupts.

Three things about that path are not obvious and are each the result of a bug
that had to be found first.

### The status numbering is measured, not assumed

`sbi_hart_get_status` returns a state code, and the specification has not always
numbered the states the same way: SBI v0.2 listed `STOPPED` first and `STARTED`
second, SBI v3.0 lists `STARTED` first and `STOPPED` second. Guessing would either
start nothing or start every hart twice.

So the encoding is measured. The boot hart is, by definition, the one hart that
is definitely `STARTED`, so whatever number it reports for itself is the
firmware's `STARTED` value, and the other one is `STOPPED`. OpenSBI v1.6 reports
`0`, so this firmware uses the v3.0 encoding, and the boot log says so:

```
hsm: status of the boot hart 0 is 0, so the firmware uses the SBI v3.0 encoding (STARTED=0, STOPPED=1)
```

### The hart id lives in `tp`, not in a global

There is no per-CPU storage yet, and a plain global cannot stand in for it: every
hart on the board writes the same word, so a hart that asks "which hart am I" is
answered with whichever hart wrote last.

That was not a theoretical concern. With the hart id cached in a static, `-smp 3`
reported hart 1 at **0 ticks** while hart 2 reported **202**, which is the boot
hart's own count: each hart was counting its interrupts into the wrong slot. The
hart id is now published in `tp` (`x4`) by both entry trampolines, and
`src/trap.S` passes it to the C handler as a second argument. `tp` is preserved
across calls by the calling convention and is already saved and restored by the
trap frame, so it costs nothing and is genuinely per-hart.

### "Running" is not "working"

A secondary that has written `satp` and set a flag has proved very little. What
matters is a hart that has *taken a timer interrupt with the MMU live*: it has
trapped through the shared page tables and returned. `timer_on_tick()` records
the `satp` mode field on every tick and counts the ones that found it set to 8,
and the boot hart waits for each secondary to reach at least one before it
reports. That is the line

```
hsm: hart 2 took a timer interrupt with the MMU live
```

in the boot log below, and it is what `make verify` requires N of at `-smp` N.

### Only the boot hart prints

A second hart writing to the same UART interleaves mid-line, and the log stops
being evidence of anything. Every secondary therefore records its state and says
nothing; the boot hart reads the slots and prints them.

## The PLIC, and the first external interrupt

The PLIC sits at `0x0c00_0000` and delivers interrupts from devices to harts. Its
registers are grouped by offset and indexed by "context" — one interrupt context
per hart per privilege mode:

| what | address |
|---|---|
| the priority of a source | `PLIC + 0x0 + 4 * source` |
| the pending bits | `PLIC + 0x1000 + 4 * (source / 32)` |
| a context's enable words | `PLIC + 0x2000 + 0x80 * context + 4 * (source / 32)` |
| a context's threshold | `PLIC + 0x200000 + 0x1000 * context` |
| a context's claim / complete | the same, plus 4 |

UART0 is source 10 on this board. Enabling `IER.RDI` on the 16550 makes it assert
that line when a byte arrives in the receive register; the PLIC raises the
supervisor external interrupt when the source is pending, enabled in the context,
and of priority above the threshold; the handler reads the byte, writes the
claim/complete word, and the interrupt is done.

### Which context is "S-mode"

This is the part that cannot be guessed. Get it wrong and the interrupt is
delivered to the wrong privilege mode: to M-mode, where the firmware swallows it.

The answer comes from the device tree, which is what both sides actually agree
on. QEMU writes the PLIC node's `interrupts-extended` as one pair of cells per
(hart, mode), M-mode then S-mode, and OpenSBI derives an entry's context number
as `cell_index / 2`. Dumping the device tree QEMU 10.0.13 builds and walking it
the same way gives:

| `-smp` | context numbers |
|---|---|
| 1 | hart 0 M → 0, hart 0 S → **1** |
| 2 | hart 0 M → 0, hart 0 S → **1**; hart 1 M → 2, hart 1 S → **3** |
| 4 | harts 0..3 M → 0, 2, 4, 6; harts 0..3 S → **1, 3, 5, 7** |

so `PLIC.h` defines `PLIC_S_CONTEXT(hartid)` as `2 * hartid + 1`. The whole
derivation, and the `dtb`-walking method, is in the header comment of
`include/plic.h`.

Note the consequence for the page tables: a context is one 4 KiB page of
threshold and claim registers, and a hart's context is *not* its hart id. A
two-page context window covers harts 0 and 1 only. That was the first version,
and it booted at `-smp` 1, 2 and 3 every time while failing at `-smp` 4 — but
only on the runs where the firmware happened to pick hart 2 or 3 as the boot
hart, with a store page fault (`cause 15`) at `0x0c205000`, which is context 5.
The boot hart is chosen by a race, so the failure looked intermittent and was
really deterministic. The windows are now defined once in `include/plic.h` and
mapped from those definitions, so the mapping cannot fall out of step with the
driver.

### The context is verified by effect, and the obvious way to do it is wrong

The self test does not take `PLIC.h`'s word for it. But the obvious check —
enable the source in one candidate context at a time and read that context's
claim word — **cannot work**, and the reason is worth recording:

> A claim word answers with a source when *that context's enable bits* allow it,
> whatever context the firmware actually routes the interrupt to.

So enabling the source in a wrong context makes that wrong context hand it
straight back, and the search measures its own writes. Measured on 18/10/2026 at
`-smp` 1, it reported **context 0** while the interrupt-driven path had just
delivered the same source correctly through context 1.

The check that does work asks the question the hardware answers: enable the
source in one candidate at a time and wait to see whether the supervisor external
interrupt *arrives at all*. Only the context wired to this hart's S-mode external
interrupt can raise the trap, so a wrong candidate is identified by its silence.

Two details of that test were themselves bugs first:

- the interrupt count has to be read **before** the byte arrives. The trap can
  land at any moment after it does — including inside the loop that watches for
  it — so reading the count afterwards counts an interrupt that already happened
  and the trial then waits for a second one that never comes;
- there must be **no** "has the byte landed yet" wait at all. A correctly wired
  context raises the trap so promptly that the handler has already drained the
  receive register by the time the pending bit is polled, so that wait timed out
  on precisely the context that worked and the search reported finding nothing.

Context 0 is not tried, because on this board it is the M-mode context of hart 0
and enabling a source there provokes the firmware, which is not the kernel's
call to make.

### Where the byte comes from

The guest writes to the serial port and never reads from it, so it cannot produce
the byte that makes the UART assert its interrupt. `../vm/riscv-uart-inject.py`
does it from the host: QEMU is given a unix socket as its serial chardev and the
injector connects as the client. The byte is not sent on a timer — the kernel
prints a trigger line and *that* releases the byte, so the test cannot race the
injector and a guest that dies before asking gets a clean failure rather than a
hang. Each `--send` pair fires once per occurrence of its trigger, which is what
lets the search ask for one byte per trial when it cannot know how many trials it
needs.

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
$ timeout 40 qemu-system-riscv64 -machine virt -m 256M -nographic -no-reboot \
      -smp 4 -bios default -kernel build/hurd-riscv64-boot.elf
```

Captured at `-smp 4`, so the last part of the log is the interesting part: three
harts the kernel released itself, each on the shared page table. Everything above
them is OpenSBI's own banner.

```
hurd-riscv64: console up (rv64gc, QEMU virt)
SBI hart id 0x1
!
SBI legacy_putc works: yes
SBI spec version 2.0 (error 0, value 0x2000000)
SBI impl id 0x1 (error 0)
SBI impl version 0x10006 (error 0)
SBI extension probe base eid 0x10 -> 1 (error 0)
SBI extension probe legacy console eid 0x1 -> 1 (error 0)
SBI extension probe time eid 0x54494d45 -> 1 (error 0)
SBI extension probe hsm eid 0x48534d -> 1 (error 0)
SBI wall clock 0 (error 0)
hurd-riscv64: milestone 1 reached
pmm: pool 64 pages of 4 KiB
pmm: allocated page at 0x80215000, aligned=0x1, free now 63
firmware stvec was 0x80200000
installed stvec 0x80202980
hurd-riscv64: milestone 2 reached
timer armed by stimecmp CSR at 1157023 (SBI sbi_timer_set_stimecmp error -2)
vm: root table ppn 0x80216, 105 pages mapped, 7 pool pages used
vm: Sv39 active, satp 0x8000000000080216
timer tick 1 at 1160444
timer tick 2 at 1258559 (period 98115 ticks, deadline +96579)
timer tick 3 at 1359209 (period 99382 ticks, deadline +196579)
timer tick 4 at 1458438 (period 99331 ticks, deadline +296579)
timer tick 5 at 1558655 (period 99552 ticks, deadline +396579)
hurd-riscv64: milestone 3 reached
hsm: extension probe 0x0x48534d -> available
hsm: status of the boot hart 1 is 0, so the firmware uses the SBI v3.0 encoding (STARTED=0, STOPPED=1)
hsm: hart 0 status 1 (stopped)
hsm: hart 1 status 0 (started, this is the boot hart)
hsm: hart 2 status 1 (stopped)
hsm: hart 3 status 1 (stopped)
hsm: hart 4 does not exist (error -3), so the board has 4 harts
hsm: published satp 0x8000000000080216, shared by every hart
hsm: released hart 0 at 0x80200038, stack top 0x8021e000
hsm: released hart 2 at 0x80200038, stack top 0x8021f000
hsm: released hart 3 at 0x80200038, stack top 0x80220000
hsm: released 3 secondary harts
timer tick 6 at 1657947 (period 99500 ticks, deadline +496579)
timer tick 7 at 1757584 (period 99523 ticks, deadline +596579)
hsm: 3 of 3 secondary harts reported running
timer tick 8 at 1857828 (period 99626 ticks, deadline +696579)
hsm: hart 0 took a timer interrupt with the MMU live
hsm: hart 2 took a timer interrupt with the MMU live
hsm: hart 3 took a timer interrupt with the MMU live
hsm: hart 0 running, 1 ticks (1 with the MMU live), satp 0x8000000000080216, stack top 0x8021e000
hsm: hart 1 running, 8 ticks (8 with the MMU live), satp 0x8000000000080216, stack top 0x80215000
hsm: hart 2 running, 2 ticks (2 with the MMU live), satp 0x8000000000080216, stack top 0x8021f000
hsm: hart 3 running, 2 ticks (2 with the MMU live), satp 0x8000000000080216, stack top 0x80220000
hurd-riscv64: milestone 4 reached, halting
```

## Known limitations, not fixed here

- **The second harts run, and nothing schedules on them.** Every hart of the
  board is released, shares the page table and takes its own timer interrupts —
  `make verify` requires exactly N of them at `-smp` N. But each one ends up
  parked in a `wfi` loop, so what is proven is that a second hart can run kernel
  code and take interrupts, not that anything is running on it. There is no
  scheduler and no work queue; "the second CPU works" remains an unproven claim
  about useful work, exactly as on the x86_64 side.
- **The harts do not stop each other.** The only synchronisation is the
  per-hart slots in `hsm_harts[]`, written with `__atomic_store_n` and read with
  `__atomic_load_n`. That is enough for one boot sequence and not enough for a
  kernel: there is no lock, no memory barrier beyond the acquire/release pairs
  already used, and no protection against a second hart and the boot hart writing
  the same slot for different purposes.
- **The PLIC is only wired for one hart.** `plic_init` programs the boot hart's
  context and enables `sie.SEIE` there only. The secondaries take no external
  interrupts, which is deliberate — the UART is one device whose interrupt
  belongs to one context, and a second hart claiming the same source would just
  find it already claimed — but it means external interrupts on a secondary are
  untested.
- **The context discovery walks 16 contexts with a 300 ms budget each.** In the
  passing case that is one trial, because the expected context is tried first.
  A board whose context numbering is wrong costs up to 16 injected bytes and
  about five seconds.
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
  one hart and one trap at a time, and it keeps the design small. With four
  harts running this is now four stacks in play, which is still fine, but a
  per-hart trap stack in `sscratch` is the right thing before nesting anything.
- **The secondary harts write nothing to the console, including on failure.**
  They record an error code in their slot and the boot hart prints it. If a
  secondary fails before the boot hart has printed the slot — or if the boot hart
  itself is the one that fails — nothing is said at all.
- **The MMU maps only what the kernel uses.** The identity mapping covers the
  kernel image, the page pool, the UART and the PLIC windows. The rest of DRAM is
  not mapped at all, so an access to it is a page fault rather than a surprise.
  There is no user space, and no attempt is made to reserve anything.
- **The page tables are built once, before the secondaries start, and never
  change.** Every hart shares that one table, which is what makes the shared
  stacks and the physical pool work; it also means there is no `sfence.vma` with
  operands to get wrong yet, and no path for adding a mapping later.
- **The physical pool is a fixed 64 pages.** That is 256 KiB. The current
  mapping spends 7 pages on page tables and one more per secondary hart, so
  there is a wide margin at `-smp` 4 and no margin at all for anything real. Growing it means either a bigger static pool or
  a way to discover the memory the firmware had, which needs the device tree.
- **There is no `sfence.vma` with operands.** The whole address space is
  flushed on the MMU switch-on and again in each secondary after it installs the
  shared `satp`, which is correct but coarse. A per-address flush will be needed
  once mappings change while the MMU is on.
- **`pmm_free_page()` does not put a page back on the free list.** It decrements
  the used count and ignores everything else, so the pool is a bump allocator
  with a counter. That is enough for the boot sequence, which never reuses a
  released page, and it will not be enough for anything that does.
