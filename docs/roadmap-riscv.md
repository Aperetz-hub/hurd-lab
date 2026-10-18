# Roadmap: RISC-V port

**Date:** 18/10/2026
**Status:** milestone 3 **built and booted** — the skeleton is freestanding C
and GNU assembler, built with `riscv64-linux-gnu-gcc`, `gas`, `ld` and `make`.
It installs an S-mode trap vector, switches the MMU on in Sv39 mode with
per-section permissions, and takes supervisor timer interrupts with the MMU
live. See `riscv64/README.md` for the boot log and `docs/suivi.md` for the
measurements.

This document is the map for the `riscv64/` expansion path: a 64-bit RISC-V
(`riscv64`) microkernel / boot target skeleton.

It is **experimental**. It shares this repository with the x86_64 fork work but
does not modify it, and nothing here is allowed to affect the existing `-smp 2`
/ virtio work. Everything in this document that touches a VM is subject to the
same rules as the rest of the lab (`AGENTS.md`): headless only, disposable
overlays only, no mutation of `hurd-dev.qcow2` or `ovl-fork-master.qcow2`.

---

## 1. Scope and history

This file originally covered two expansion paths: `riscv64/`, and a separate
workspace holding a translator-shaped component written in a memory-safe
systems language, with bindings generated from the Mach headers. That second
path was removed from the repository on 18/10/2026 because it had never spoken
to Mach and was not worth carrying; the analysis that led to that decision is
recorded in `docs/suivi.md`, section "Removal of the experimental component
workspace".

Its scaffolding is recoverable from history: the tree was added in `b108fb5bb`
and last built in `570531c15`; `git show 570531c15` reproduces it exactly.
Nothing in `riscv64/` depended on it.

On 18/10/2026 `riscv64/` itself was rewritten in freestanding C and GNU
assembler, so that the whole repository uses one toolchain family — the same
one GNU Mach itself is built with.

---

## 2. What exists

| path | purpose |
|---|---|
| `riscv64/README.md` | the rv64gc skeleton: boot contract, console design, milestones, known defects |
| `riscv64/Makefile` | `check` / `build` / `run` / `verify` / `selftest` / `debug` / `disasm` / `clean` |
| `riscv64/linker/riscv64-virt.ld` | image layout for the QEMU `virt` board |
| `riscv64/include/` | `console.h`, `csr.h`, `linker.h`, `pmm.h`, `sbi.h`, `timer.h`, `trap.h`, `trap_offsets.h`, `uart.h`, `vm.h` |
| `riscv64/src/` | the kernel skeleton itself (see below) |

Inside `riscv64/src`:

- `boot.S` — `_start` trampoline: load `gp`, set `sp`, zero `.bss`, call
  `kmain`.
- `trap.S` — the S-mode trap entry: save the frame, call the C handler,
  restore, `sret`.
- `trap.c` — the C trap handler, and `trap_offsets.h` / `trap.h`, which share
  the frame layout and check it with `_Static_assert`.
- `timer.c` — the S-mode timer: `rdtime` plus an interval, written to
  `stimecmp`, plus the tick counter and the drift-free rearm.
- `pmm.c` — the 4 KiB physical page allocator over a fixed pool above the
  kernel image.
- `vm.c` — the Sv39 page tables and the MMU switch-on.
- `sbi.c` — SBI `ecall` wrappers returning the `struct sbiret` pair, for the
  base and TIME extensions and for the legacy console.
- `uart.c` — polled NS16550A driver for the QEMU `virt` board (`0x1000_0000`).
- `console.c` — sink multiplexer plus allocation-free `putc`/`puts`/`puthex`.
- `main.c` — `kmain()`, and an allocation-free `putdec`.

Build output goes to `riscv64/build/` and is ignored by Git.

---

## 3. Toolchain setup on a Debian host

### 3.1 RISC-V cross toolchain

```sh
sudo apt update
sudo apt install gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
                 qemu-system-misc gdb-multiarch
```

- `gcc-riscv64-linux-gnu` gives `riscv64-linux-gnu-gcc`, `readelf`, `objdump`.
  It is the toolchain the skeleton is built with: `gcc` drives `gas` for the
  `.S` trampoline, `gcc -nostdlib -T linker/riscv64-virt.ld` drives `ld` for
  the image. The same binaries are used for inspection
  (`riscv64-linux-gnu-readelf -lW`).
- `qemu-system-misc` provides `qemu-system-riscv64`. On Debian it may also be
  available as the standalone `qemu-system-riscv64` package — either is fine;
  `make check` verifies the binary exists and names the package to install.
  The OpenSBI firmware ships with QEMU at
  `/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin` and is what
  `-bios default` loads.
- `gdb-multiarch` is required for the RISC-V debug server; the plain `gdb` on
  Debian does not have the RISC-V target.

No other toolchain is involved: the kernel is plain C11 plus one assembly
file, and links against nothing but `libgcc`.

### 3.2 Current state of the reference host

Verified on 18/10/2026:

| tool | state |
|---|---|
| `riscv64-linux-gnu-gcc` | **installed** (Debian 14.2.0) |
| `riscv64-linux-gnu-ld` | **installed** (GNU Binutils for Debian) 2.44 |
| `qemu-system-riscv64` | **installed** (QEMU 10.0.13, Debian) |
| OpenSBI firmware | **installed** (`/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin`) |
| OS | Debian GNU/Linux 13 (trixie), x86_64 |

The skeleton has been built and booted; see `docs/suivi.md`, section "RISC-V
skeleton: first boot", and the boot log in `riscv64/README.md`. Nothing in
`riscv64/README.md` is a design intent any more as far as milestone 1 goes;
milestones 2 to 5 are still unwritten and their "expected output" text is
still intent.

---

## 4. RISC-V path

### 4.1 Boot chain and why each piece exists

```
QEMU (-machine virt)
  └─ OpenSBI (-bios default)            M-mode; sets up a stack, enters S-mode
       └─ our ELF at 0x80200000        first instruction = _start
            ├─ (only the boot hart gets here; the others are held by the firmware)
            ├─ la gp, __global_pointer$ must precede any gp-relative access
            ├─ la sp, __stack_top
            ├─ zero .bss                __bss_start .. __bss_end
            └─ call kmain
                 ├─ console_init()      NS16550A @ 0x1000_0000
                 ├─ SBI base extension  spec version, impl id/version, probes
                 ├─ legacy console      extension 0x01, proves the ecall path
                 ├─ stvec = trap_entry  the S-mode trap vector
                 ├─ stimecmp = time+Δ   arm the S-mode timer
                 ├─ satp = Sv39|root    switch the MMU on
                 └─ wfi                 wait for the timer interrupts
                      └─ trap.S → trap.c  five ticks, then milestone 3
```

Three decisions worth keeping in mind when this is extended:

- **The UART is not probed.** A write to an absent device is silently dropped on
  real hardware, so `uart_ready` is set by `console_init()` and the SBI console
  is the fallback. This is why `console_putc` exists even though the UART
  works.
- **The boot trampoline is a separate `.S` file.** It has to run before any C
  code exists, and it is the only place where the ABI is set up by hand. The
  hart id is read from `a0`, which is what the SBI boot protocol passes at the
  S-mode entry point; `mhartid` must **not** be used, because it is a
  machine-mode CSR and reading it from S-mode traps immediately.
- **No "park the other harts" loop.** Only the boot hart ever reaches the
  kernel; the firmware holds the others. The boot hart is not necessarily
  hart 0, so a loop keyed on the hart id would park the boot hart itself.
  Starting further harts is a later milestone, and the parking belongs in the
  path that releases them.
- **No unwinder, so no unwind tables.** The image is linked `-nostdlib` with
  `/DISCARD/ .eh_frame`; emitting unwind tables would produce sections
  nothing can consume.

### 4.2 Milestones

| # | milestone | why it is the next thing |
|---|---|---|
| 1 | console up | **done** — firmware handoff, `gp`/`sp`/`.bss`, SBI `ecall` |
| 2 | trap vector + timer | **done** — `stvec` installed, five supervisor timer interrupts taken and counted |
| 2b | physical memory + MMU | **done** — 4 KiB page allocator, Sv39 page tables with per-section permissions, MMU live while the ticks are taken |
| 3 | device tree | parse the DTB OpenSBI passes in `a1`; find UART + CLINT. Needed for anything but the Sstc timer |
| 4 | interrupts | PLIC, UART interrupts, per-hart interrupt stacks, IPL equivalent |
| 5 | physical memory | page tables, first allocator |
| 6 | Mach-shaped core | threads, ports, IPC |

**Milestone 3 is where the x86-64 experience must be applied deliberately.**
The `-smp 2` investigation in `docs/suivi.md` found that GNU Mach advances
time, the timeout list and `last_hpc_read` **only on the master CPU**
(`clock_interrupt()`, `if (my_cpu == master_cpu)`). That design is correct and
must be reproduced *with* the correct interrupt-routing behaviour, because the
hang there came from the bring-up sequence stealing the master's tick, not from
the design itself. Two transferable lessons:

1. **Only the boot hart may configure global interrupt state.** `lapic_disable()`
   on the BSP around AP startup silently cost the master its 100 Hz tick. The
   RISC-V equivalent (a hart-mask or a PLIC source-enable write) must not be
   touched once the boot hart is serving the clock.
2. **Prove the clock, do not infer it.** A "hang" with a live guest and a frozen
   `uptime` is a routing bug, not a deadlock. `vm/kprobe.py` exists for the x86
   side; the equivalent for RISC-V should be in place at milestone 3, from the
   start, because it is what turned an "I/O hang" into a measured root cause.

### 4.3 Validation without a Hurd userland

Every milestone is validated against the host, not against a userland:

- `riscv64/make run` — expected console output is in `riscv64/README.md`.
- `riscv64/make verify` — boots at `-smp` 1 to 4, several runs each, and
  requires the milestone banner exactly once per run. The repetition is not
  paranoia: the firmware's choice of boot hart varies between runs, and a
  single run per hart count does not exercise that.
- `riscv64/make selftest` — builds two variants and checks both fault paths:
  one raises a deliberate illegal instruction, the other stores to the
  read-only data and requires a store/AMO page fault (`scause = 15`), which
  is what proves the Sv39 permissions are real rather than merely
  installed.
- `riscv64/make disasm` — check `_start` really landed at the load address and
  that `.bss` symbols exist; this catches linker-script mistakes that would
  otherwise appear as a silent hang.
- `riscv64/make debug` + `gdb-multiarch` — the machine starts stopped, so the
  first instructions can be single-stepped with the symbols present.

---

## 5. Integration with the existing build system

### 5.1 Today

The skeleton is **deliberately not wired into the Hurd build**. The existing
system is autotools-based and lives outside this repository (`src/gnumach`,
`src/hurd`, `src/mig` — see `README.md`). The skeleton here is driven by its
own `Makefile`:

```sh
cd riscv64 && make check && make build && make run
```

That separation is intentional: it keeps a broken experimental target from
failing the Hurd build.

### 5.2 Cross-compilation

| target | built by | notes |
|---|---|---|
| `riscv64-linux-gnu` (freestanding) | `riscv64-linux-gnu-gcc` | `-ffreestanding -nostdlib -nostartfiles`, own linker script, `-mcmodel=medany` |

That is the only target the skeleton needs. It is a *freestanding* build of a
Linux-target compiler, not a Linux build: no libc headers, no start files, no
dynamic loader, and only `libgcc` is linked. `-mcmodel=medany` is required
because the image lives at `0x8020_0000`, above the signed 2 GiB reach of the
`medlow` default.

The `riscv64-linux-gnu-` prefixed `readelf`/`objdump` are used for
inspection (`make disasm`).

### 5.3 Rules this workspace must not break

- **English only** (`AGENTS.md` §1) — including comments and commit messages.
- **No disk mutation.** The RISC-V path boots its own image under QEMU and never
  touches `hurd-dev.qcow2`, `ovl-fork-master.qcow2` or any overlay. If a future
  milestone needs a Hurd userland on RISC-V, it goes through
  `vm/run-hurd64-smp.sh`-style disposable overlays and a **separate** machine
  type — not through the x86_64 launchers.
- **Headless only** (`AGENTS.md` §2) — `-nographic` in every QEMU invocation.
- **Documentation edits must not alter functional code** (`AGENTS.md` §4).
- Before pushing: `git pull --rebase` (`AGENTS.md` §4) — other agents push to
  this repository concurrently.

---

## 6. Open questions

1. **Guest bootstrap for RISC-V.** Milestone 5 needs a Mach userland on
   `riscv64`. Either port `hurd` and the translators wholesale, or start with a
   Mach-shaped core plus a single small server and accept that most of the
   system is missing. The second is far cheaper and is the recommended default.
2. **Toolchain availability.** `qemu-system-riscv64` and
   `riscv64-linux-gnu-gcc` are present on the reference host, so
   `riscv64/make check` passes; milestones 2 to 5 remain unwritten.
3. **How to start the other harts.** The firmware holds them; starting them
   is an SBI HSM operation, and the kernel needs a per-hart trap stack
   (`sscratch`) before it can do that safely, since the current trap frame
   lives on the interrupted hart's stack.
4. **Whether the device tree is needed at all for the timer.** The `sstc`
   extension is enough to arm `stimecmp` without parsing the DTB, so the
   device tree milestone could be deferred past the PLIC work if a simpler
   path exists to the UART interrupts.
