# hurd-lab

Kernel, driver and architecture bring-up work for **GNU/Hurd**: `x86_64` SMP
bring-up in GNU Mach and a VirtIO-network stack (`rumpnet` / `vioif`) built on
the NetBSD rump kernel, plus an experimental freestanding `riscv64` (`rv64gc`)
S-mode microkernel prototype written in pure C11 and GNU assembler.

Everything claimed below was measured on a QEMU host; the numbers are the ones
in `docs/suivi.md`, which records each measurement, what it does and does not
prove, and where the boundary between observation and inference sits.

---

## Key technical results

### 1. GNU Mach `x86_64` SMP bring-up — the `-smp 2` boot freeze

**Root cause.** `start_other_cpus()` in `i386/i386/mp_desc.c` wrapped the
application-processor bring-up in `lapic_disable()` / `lapic_enable()`. On the
boot processor that is not merely redundant, it is destructive. `splhigh()` is
already in effect for the whole window, so masking costs nothing — but clearing
`LAPIC_ENABLE` makes the BSP unable to accept *any* IOAPIC interrupt, so the
100 Hz PIT edges that arrive while the APs come up are consumed without ever
being delivered.

Because `clock_interrupt()` advances `uptime`, the timeout wheel and
`last_hpc_read` **only on the master CPU**, the system clock then stops
completely: no time, no timeout, no preemption. `rumpdisk` spins forever in its
AHCI attach. The measured signature is a guest that is otherwise alive with
`uptime` stuck at **0.120 s** and `elapsed_ticks` at **12**.

**Fix.** One hunk in `mp_desc.c`, `smp: do not disable the BSP local APIC while
starting the APs` — commit **`4207ebda`** on `fork/x86_64`.

**Verification.** On QEMU q35 with KVM, `-m 4G`, the `noide` boot argument, and
a disposable overlay:

| kernel | `-smp 2` | `-smp 1` |
|---|---|---|
| `origin/master` `79e98e8b`, unpatched | **0/3 BOOT** (3 × WEDGE) | 2/2 BOOT, `nproc=1` |
| `origin/master` + the extracted 3-patch series | **3/3 BOOT**, `nproc=2` | 2/2 BOOT, `nproc=1` |
| fork branch, fix applied | **12/12 BOOT**, `nproc=2` | 3/3 BOOT |

The 12/12 figure is the fork-branch run recorded in
`upstream/gnumach-smp-ap-startup/COVER-NOTE.md`; the 0/3 and 3/3 figures are the
clean-`master` before/after pair in `docs/suivi.md`.

Every BOOT round reads `nproc` and `md5sum /boot/gnumach-1.8-amd64-up.gz` over
SSH *inside* the guest, and the checksum matched the uploaded kernel each time,
so the kernel that booted is the one that was built from the patched tree.

**Master clock.** Over a 60 s window: **6115 ticks in 61.140 s of host time =
100.017 ticks/s** against a nominal 100, with the kernel's own `uptime`
tracking the host wall clock to 0.017 %.

**The AP runs its own periodic timer.** Measured, not assumed: the per-CPU
`machine_slot[cpu].cpu_ticks[]` counters, which `clock_interrupt()` increments
*outside* the master-only guard and which nothing else writes.

| QEMU irqchip | master (CPU 0) | application processor (CPU 1) |
|---|---|---|
| `kernel-irqchip=on` (default) | 99.98 Hz | **85.83 Hz** |
| `kernel-irqchip=split` | 200.01 Hz | **163.50 Hz** |

The AP's total rate is unchanged between idle and saturated (163.50 vs
163.48 Hz), and under full load it charged itself **136.9 Hz of user-mode
time**, which is how the kernel accounts that CPU at all.

**Concurrent MP stress.** A 60 s window at `-smp 2` with `vioif0` up, running a
CPU-bound worker on each processor set (one pinned through `/sbin/smp`),
continuous `rumpdisk` write+fsync+read and continuous ICMP: **0 % packet loss**,
~11 G iterations on both workers, clock never frozen.

**Known limit, stated rather than glossed:** that the Hurd *userland* is
SMP-correct is **not** shown. Two CPUs run concurrent work, both take clock
interrupts and the clock holds, but the safety of the timeslice accounting, run
queues and AST path with two processors in user space at once remains open.

### 2. VirtIO network — `vioif0` on NetBSD rump

`librumpdev_pci_virtio` and `librumpdev_pci_if_vioif` are absent from the Debian
Hurd packages; this repository builds and installs them.

- **`curlwp` / `%gs` crash fixed.** rump's `<machine/cpu.h>` was resolving
  `curlwp` to the `%gs` access `x86_curlwp()` instead of the Hurd hypercall
  `rump_curlwp_fast()`, crashing on `mov %gs:0xac0,%rax`. Fixed by interposing
  the architecture header and defining `-D_RUMPKERNEL` so `curlwp` stays
  `x86_curlwp()`, plus putting the rump includes first — see `patches/build/`.
  This is a defect of the build *recipe*, not an upstream patch, and the
  repository says so rather than dressing it up as one.
- **`bpf_hdr` 64-bit layout corrected.** rumpnet's local copy of `struct
  bpf_hdr` had `usec` before `sec`, so on `x86_64` every reader of the timestamp
  got microseconds where seconds belong. The offsets the translator relies on
  did not move, which is why it went unnoticed.
- **Integrated at boot** via `scripts/start-rumpnet.sh` (translator + devnode,
  called as `pre-up`) and `scripts/interfaces.vioif`; `eth0`/netdde kept as a
  non-`auto` fallback.
- **`-m 4G` is mandatory**: with 2 GB the driver does not attach
  (`virtio0: virtqueue 0 for rx0 allocation failed, error code 1`).

**Sustained heavy TCP, 100 MiB per transfer**, throughput counted by the host
sink that receives the bytes rather than by the guest's clock:

| QEMU irqchip | idle | under the full MP load |
|---|---|---|
| `kernel-irqchip=on` | **3.059 MiB/s** | **3.058 MiB/s** (−0.04 %) |
| `kernel-irqchip=split` | **5.538 MiB/s** | **4.395 MiB/s** (−20.6 %) |

All four transfers delivered exactly 104857600 bytes with **zero stalls** and
**0 % ICMP loss**. `scp` of the same volume over the same device reaches
4.396 MiB/s — within 0.03 % of raw TCP — so the cipher is not the limiting
factor, the device is.

**A configuration difference worth knowing about.** The two QEMU irqchip models
are not interchangeable: with `kernel-irqchip=split` the master clock runs at
200 Hz against a kernel built for `hz = 100`, so the guest's time of day advances
at **2× real time** and every guest-measured duration is inflated by half again.
That is why every throughput figure above is host-measured.

**Known limit:** TCP retransmit counts are **not measured**. The guest has no
`/proc/net`, and for a connection the guest opens, TCP is terminated inside
QEMU's user-mode stack where the host cannot see it.

### 3. `riscv64/` — freestanding `rv64gc` S-mode kernel prototype

100 % freestanding C11 and GNU assembler on the QEMU `virt` board, built with
`riscv64-linux-gnu-gcc`, `gas`, `ld` and `make` — the same toolchain family GNU
Mach itself uses, and no other. `-ffreestanding -nostdlib -nostartfiles`, own
linker script, no PIC, no unwind tables.

At **milestone 4**:

| capability | how it was established |
|---|---|
| OpenSBI v2.0 SBI handshake | `struct sbiret` error/value pair read from `a0`/`a1`, not `a0` alone |
| S-mode trap vector | `stvec` in direct mode; the frame layout is defined once and shared by the assembly and the C struct, with a `_Static_assert` per member, so drift is a build error |
| drift-free `stimecmp` timer | deadline advanced by exactly one interval, so the period cannot drift with handler latency |
| 4 KiB physical allocator | pages handed out aligned and zeroed from a pool the linker script reserves |
| three-level Sv39 MMU | `satp` + `sfence.vma`, with hardware-enforced `R-X` / `R--` / `RW-` per section — the `R--` is a real fault, not a convention |
| multi-hart SMP bring-up | the firmware's parked harts released through the SBI HSM extension (`0x48534D`); each installs the **shared** page table and takes its own timer interrupts |
| PLIC external interrupt | UART receive interrupt, supervisor external interrupt claimed and completed; the S-mode context number `2N+1` derived from the device tree, then **verified by effect** |

`make verify` requires **exactly N harts running at `-smp` N** — 6/6 runs each at
`-smp` 1, 2, 3 and 4 — because the second harts are the deliverable and a boot
hart reporting success says nothing about them. Each hart must additionally have
taken a timer interrupt *with the MMU live*, which is a trap through the shared
page tables and not merely a flag being set.

`make selftest` exercises three failure paths on purpose: a deliberate illegal
instruction, a store to the read-only data that must raise a store page fault
(cause 15), and the PLIC interrupt, for which the host injects a byte into the
guest's UART over a serial socket.

---

## Repository layout

| path | purpose |
|---|---|
| `riscv64/` | the freestanding `rv64gc` S-mode kernel prototype; its own `README.md` has the design rationale and the boot log |
| `docs/` | `suivi.md` — the chronological engineering log with every measurement; `roadmap-riscv.md` — scope and bring-up path for the RISC-V target |
| `upstream/` | patches **prepared** for the GNU/Hurd project, one directory per independent submission, each with a cover note giving the problem, the cause, the exact test commands and results, and the known limits. **Nothing here has been sent anywhere.** |
| `patches/` | consolidated fork patches and the out-of-tree rump build tooling for the virtio network stack |
| `vm/` | QEMU launch scripts, the SMP trial harness, the guest-side stress and throughput harnesses, and the kernel probes |
| `scripts/` | `vioif` boot integration: `start-rumpnet.sh`, `interfaces.vioif`, `net-switch-vioif.sh` |

The `gnumach`, `hurd` and `mig` checkouts this work is done in are **not** part
of this repository; they live outside it, with work on `fork/x86_64` branches.

### The measurement tools in `vm/`

| tool | what it does |
|---|---|
| `clock-probe.py` | reads the kernel's timekeeping globals out of guest memory through the QEMU monitor, stamped against the host wall clock |
| `ap-timer-probe.py` | measures each processor's clock-interrupt rate, which is how the AP's own timer was established |
| `memread-check.py` | self-checks for those probes; see the trap it guards below |
| `vga-console.py` | decodes the guest's VGA text console out of guest memory, because the Hurd console is not the serial line |
| `kprobe.py` | the original kernel thermometer, kept |
| `vioif-throughput.sh`, `vioif-send.py` | the sustained-throughput experiment over `vioif0` |
| `riscv-uart-inject.py` | feeds a byte to a guest waiting for one on its serial line, released by a trigger the guest prints rather than by a host timer |

**A measurement trap these guard against.** QEMU's HMP `xp/Nx` reads **bytes**,
not words: the size suffix is only honoured when it is one of `b`/`h`/`w`/`l`/`q`.
A reader written for the word form silently reads bytes, picks up the low byte
of each 32-bit word, and returns numbers that look like counters rather than the
counters. Separately, the kernel is linked at `0xffffffff80000000` but loaded at
1 MiB, so a link-time address needs translating for a physical read and is
already correct for a virtual one. Both mistakes return plausible numbers
instead of errors, which is why `memread-check.py` exists: a self-referential
pointer for the translation, `uptime` against `elapsed_ticks` for the width, and
the per-CPU slots for index and stride.

---

## Reproducing and verification

### `riscv64/`

Needs `gcc-riscv64-linux-gnu`, `binutils-riscv64-linux-gnu`, `qemu-system-misc`
and `python3`.

```sh
cd riscv64

make check       # toolchain present?
make build       # -> build/hurd-riscv64-boot.elf
make verify      # boot at -smp 1..4; requires exactly N harts running
make selftest    # illegal instruction, .rodata store fault, PLIC interrupt
make disasm      # program headers, entry point, first instructions
make run         # single boot on stdio
```

`make verify` takes three rounds per hart count by default; `make verify
ROUNDS=6` is what produced the 6/6 figures above.

Prefix with `LC_ALL=C` if your shell's locale is not English — the Makefile
exports `LC_ALL=C` for the recipes it runs, but `make`'s own `-C` and
"nothing to be done" messages are printed by `make` itself, before the Makefile
is read, and use the invoking environment's locale.

### `x86_64`

Needs a Debian host with `qemu-system-x86`, `qemu-utils` and the official
Debian Hurd image. Everything runs headless — text console and SSH only; this
project never runs Xorg, `xdm` or XFCE.

```sh
# The base image is NEVER modified in place:
# https://cdimage.debian.org/cdimage/ports/latest/hurd-amd64/
qemu-img convert -O qcow2 debian-hurd-amd64-*.img hurd-dev.qcow2

# Single-CPU dev VM on the autonomous working disk
./vm/run-hurd64.sh

# -smp 2 on a disposable overlay, recreated at every launch
./vm/run-hurd64-smp.sh
./vm/run-hurd64-smp.sh /home/ben/hurd-fork/vm/hurd-dev.qcow2   # FRESH=0 reuses it

# The SMP + vioif0 MP stress window (boots, verifies, loads, samples, collects)
OVL=/path/to/disposable.qcow2 SMP=2 ./vm/smp-stress-run.sh

# The AP timer and the guest clock rate, from the host
python3 vm/ap-timer-probe.py /path/to/monitor.sock 25
python3 vm/clock-probe.py    /path/to/monitor.sock 20

# Sustained 100 MiB TCP over vioif0, idle and under load
./vm/vioif-throughput.sh --mib 100 --dur 60
```

Every VM run wants `-m 4G`. See `AGENTS.md` for the operating rules this
workspace works under — headless only, disposable overlays only, never a
mutation of `hurd-dev.qcow2` or `ovl-fork-master.qcow2`.

---

## Safety rules for disk images

- Never boot a base `.img` in write mode.
- Never re-extract or overwrite an image that is in use as a *backing file*.
- Use a disposable overlay (`hurd-*-test.qcow2`, recreated at each launch) or a
  full copy for anything risky. The SMP and throughput experiments use a full
  copy, so the dev disk is never even opened for writing.
- `vm/net-switch-vioif.sh` is **locked**: bringing `/dev/eth0` down with rumpnet
  active freezes the guest on an unacknowledged IRQ 11 and needs a reboot.

## Safety rules for this repository

- `riscv64/` is 100 % freestanding C and GNU assembler. No other language and no
  other build system appears anywhere in the tree.
- All documentation, comments and commit messages are in English.
- Claims are labelled **FACT**, **PLAUSIBLE HYPOTHESIS** or **SPECULATION** in
  `docs/suivi.md`. A measurement that was not taken is recorded as not measured,
  including where that is inconvenient.