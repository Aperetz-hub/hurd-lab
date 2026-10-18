# Roadmap: RISC-V port

**Date:** 18/10/2026 (Rust sections removed 18/10/2026, see §1)
**Status:** scaffolding committed; **nothing built yet** — the RISC-V toolchain
is absent on the reference host (verified, see §2.3).

This document is the map for the `riscv64/` expansion path: a 64-bit RISC-V
(`riscv64`) microkernel / boot target skeleton.

It is **experimental**. It shares this repository with the x86_64 fork work but
does not modify it, and nothing here is allowed to affect the existing `-smp 2`
/ virtio work. Everything in this document that touches a VM is subject to the
same rules as the rest of the lab (`AGENTS.md`): headless only, disposable
overlays only, no mutation of `hurd-dev.qcow2` or `ovl-fork-master.qcow2`.

## 1. Scope and history

This file originally covered two expansion paths, `riscv64/` and a `rust/`
workspace holding a `#![no_std]` translator-shaped component (`hurd-mig-sys`,
`hurd-mig-example`). The Rust track was removed from the repository on
18/10/2026. Its scaffolding is recoverable from history: `rust/` was added in
`b108fb5bb` and made to build in `570531c15`; `git show 570531c15 -- rust`
reproduces it exactly. Nothing in `riscv64/` depended on it.

---

## 2. What exists

| path | purpose |
|---|---|
| `riscv64/README.md` | the rv64gc skeleton: boot contract, console design, milestones |
| `riscv64/Cargo.toml`, `rust-toolchain.toml`, `.cargo/config.toml` | workspace, nightly pin, `panic=abort`, linker script wiring |
| `riscv64/Makefile` | `check` / `build` / `build-std` / `run` / `debug` / `disasm` / `clean` |
| `riscv64/crates/hurd-riscv64-boot/` | the kernel skeleton itself (see below) |

Inside `riscv64/crates/hurd-riscv64-boot/src`:

- `boot.rs` — `_start` trampoline: park non-boot harts, load `gp`, set `sp`,
  zero `.bss`, call `kmain`.
- `sbi.rs` — SBI `ecall` wrappers (`a7` = extension, `a6` = function), with
  `console_putc`, `spec_version`, `impl_id`.
- `uart.rs` — polled NS16550A driver for the QEMU `virt` board (`0x1000_0000`).
- `console.rs` — sink multiplexer plus allocation-free `putc`/`puts`/`puthex`.
- `main.rs` — `kmain()`, the panic handler, and an allocation-free `putdec`.

---

## 3. Toolchain setup on a Debian host

### 3.1 RISC-V cross toolchain

```sh
sudo apt update
sudo apt install gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
                 qemu-system-misc gdb-multiarch
```

- `gcc-riscv64-linux-gnu` gives `riscv64-linux-gnu-gcc`, `readelf`, `objdump`.
  It is used to sanity-check the layout of the kernel ELF
  (`riscv64-linux-gnu-readelf -lW`) and is the toolchain to reach for if the
  skeleton ever needs a C stub (a linker script `PHDRS`-aware trampoline, an
  assembly `.S` instead of `global_asm!`, a tiny C SBI shim).
- `qemu-system-misc` provides `qemu-system-riscv64`. On Debian it may also be
  available as the standalone `qemu-system-riscv64` package — either is fine;
  `make check` verifies the binary exists and names the package to install.
- `gdb-multiarch` is required for the RISC-V debug server; the plain `gdb` on
  Debian does not have the RISC-V target.

Note that the Rust build in `riscv64/` does **not** need
`gcc-riscv64-linux-gnu`: `riscv64gc-unknown-none-elf` is a Rust target, not a
Linux target, and `rust-lld` links the image. The GCC cross toolchain is for
inspection and for C interop.

### 3.2 Current state of the reference host

Verified on 18/10/2026:

| tool | state |
|---|---|
| `rustc` / `cargo` / `rustup` | installed (stable 1.99.1, nightly 1.101.0-nightly) — needed to build the skeleton |
| `rust-src`, `rustfmt`, `clippy` | installed |
| `qemu-system-riscv64` | **absent** |
| `riscv64-linux-gnu-gcc` | **absent** |
| OS | Debian GNU/Linux 13 (trixie), x86_64 |

The **RISC-V skeleton has never been compiled**: without `qemu-system-riscv64`
and `riscv64-linux-gnu-gcc`, every "expected output" in `riscv64/README.md` is
a design intent, not an observed result. First action for whoever picks this up:
install §3.1, then `riscv64/make check` followed by `make build`.

---

## 4. RISC-V path

### 4.1 Boot chain and why each piece exists

```
QEMU (-machine virt)
  └─ OpenSBI (-bios default)            M-mode; sets up a stack, enters S-mode
       └─ our ELF at 0x80200000        first instruction = _start
            ├─ park non-boot harts      csrr mhartid
            ├─ la gp, __global_pointer$ must precede any gp-relative access
            ├─ la sp, __stack_top
            ├─ zero .bss                __bss_start .. __bss_end
            └─ call kmain
                 ├─ console::init()     NS16550A @ 0x1000_0000
                 ├─ SBI console_putc    extension 0x01, proves the ecall path
                 ├─ SBI spec version    (major << 24) | minor
                 └─ wfi                 park (no scheduler yet)
```

Three decisions worth keeping in mind when this is extended:

- **The UART is not probed.** A write to an absent device is silently dropped on
  real hardware, so `UART_READY` is set by `init()` and the SBI console is the
  fallback. This is why `console_putc` exists even though the UART works.
- **`global_asm!` over a `.S` file.** The whole boot path stays one unit that
  cargo tracks. If it has to move, the drop-in is `src/boot/boot.S` plus
  `#[link_section]`.
- **`panic = "abort"` and `/DISCARD/ .eh_frame`.** There is no unwinder and no
  personality routine; emitting unwind tables produces an image that carries
  sections nothing can consume.

### 4.2 Milestones

| # | milestone | why it is the next thing |
|---|---|---|
| 1 | console up | **done in the skeleton** — firmware handoff, `gp`/`sp`/`.bss`, SBI `ecall`, panic path |
| 2 | device tree | parse the DTB OpenSBI passes in `a0`/`a1`; find UART + CLINT. No timer before this |
| 3 | timer + traps | CLINT `mtime`, trap handler, interrupt vector table, per-CPU interrupt stacks, IPL equivalent |
| 4 | physical memory | PLIC, UART interrupts, page tables, first allocator (this is where `-Z build-std` becomes necessary) |
| 5 | Mach-shaped core | threads, ports, IPC |

**Milestone 3 is where the x86-64 experience must be applied deliberately.**
The `-smp 2` investigation in `docs/suivi.md` found that GNU Mach advances
time, the timeout list and `last_hpc_read` **only on the master CPU**
(`clock_interrupt()`, `if (my_cpu == master_cpu)`). That design is correct and
must be reproduced *with* the correct interrupt-routing behaviour, because the
hang there came from the bring-up sequence stealing the master's tick, not from
the design itself. Two transferable lessons:

1. **Only the boot CPU may configure global interrupt state.** `lapic_disable()`
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
`src/hurd`, `src/mig` — see `README.md`). The scaffolding here is a Cargo
workspace driven by its own `Makefile`:

```sh
cd riscv64 && make check && make build && make run
```

That separation is intentional: it keeps a broken experimental workspace from
failing the Hurd build.

### 5.2 Cross-compilation

| target | built by | notes |
|---|---|---|
| `riscv64gc-unknown-none-elf` | `cargo build` | no libc; `.cargo/config.toml` sets `panic=abort` and the linker script |

That is the only target the skeleton needs. It is a Rust target, not a Linux
target, so `rust-lld` links the image; `riscv64-linux-gnu-gcc` is for inspection
and for any future C interop, not for linking.

### 5.3 Standard library rebuilding (`-Z build-std`)

**The skeleton does not declare `[unstable] build-std`**, and that is
deliberate: listing it in `.cargo/config.toml` turns standard-library
rebuilding on *by default* for every build, and cargo then builds its own
`core` on top of the prebuilt one:

```
error[E0152]: duplicate lang item in crate `core` (which `std` depends on): `sized`
```

Rebuilding is opt-in and explicit instead:

```sh
cargo +nightly build -Z build-std=core,compiler_builtins \
     -Z build-std-features=compiler-builtins-mem
```

| when | needed? |
|---|---|
| boot skeleton (milestone 1) | no — `core` is prebuilt for `riscv64gc-unknown-none-elf` |
| adding an allocator (milestone 4) | yes — no prebuilt `alloc` exists for a `none` target |
| hacking on `core` itself | yes |

`compiler-builtins-mem` is needed because the RISC-V target does not get the
memory intrinsics from the prebuilt `compiler_builtins` the way Linux targets
do; without it, `memcpy`/`memset` go missing at link time. The Makefile exposes
both paths as `make build` and `make build-std` so the choice is visible.

Cost: `-Z build-std` recompiles `core` on every clean, which is minutes, not
seconds. Prefer the prebuilt `core` until a milestone needs `alloc`.

### 5.4 Rules this workspace must not break

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
2. **Whether `-Z build-std` becomes a hard dependency.** If milestone 4 needs it
   for `alloc`, nightly becomes mandatory for the RISC-V target, and the
   `rust-toolchain.toml` pin is load-bearing rather than optional.
3. **Toolchain availability.** `qemu-system-riscv64` and
   `riscv64-linux-gnu-gcc` are absent, so `riscv64/make check` is the gate and
   the expected outputs in `riscv64/README.md` remain unverified.