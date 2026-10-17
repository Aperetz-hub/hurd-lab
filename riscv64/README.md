# riscv64/ — bare-metal rv64gc kernel skeleton for GNU/Hurd

Experimental target: a **64-bit RISC-V microkernel skeleton**, written in Rust,
that boots on the QEMU `virt` board under OpenSBI. It is the smallest thing
that proves the whole bring-up chain works end to end — firmware handoff,
S-mode entry, SBI console output — so that everything above it can be built on
a foundation that is known to reach `main`.

> Status: **blueprint**. It has not been compiled on the reference host; the
> toolchain it needs is not installed there yet. See
> `docs/roadmap-riscv-rust.md` for the exact setup, and treat "it builds" as a
> to-do rather than a claim.

## What is in here

```
riscv64/
├── Cargo.toml                       workspace + release profile (opt-level=s)
├── rust-toolchain.toml              nightly, rust-src, rv64gc target
├── .cargo/config.toml               default target, panic=abort, linker script
├── Makefile                         check / build / build-std / run / debug / disasm
└── crates/hurd-riscv64-boot/
    ├── Cargo.toml
    ├── linker/riscv64-virt.ld       image layout for the QEMU virt board
    └── src/
        ├── main.rs    #![no_std] #![no_main]: kmain(), panic handler
        ├── boot.rs    _start trampoline (global_asm): hart parking, gp, .bss
        ├── sbi.rs     ecall wrappers, console_putc, spec_version, impl_id
        ├── uart.rs    polled NS16550A driver for the virt board
        └── console.rs console multiplexer + allocation-free hex/putc
```

## Boot contract

| stage | who | what happens |
|---|---|---|
| reset | QEMU + OpenSBI (`-bios default`) | M-mode firmware, sets up a stack, enters S-mode at `0x80200000` |
| `_start` | `src/boot.rs` | parks non-zero harts, loads `gp`, sets `sp = __stack_top`, zeroes `.bss` |
| `kmain` | `src/main.rs` | initialises the console, exercises SBI, reports the spec version, parks the hart |

`_start` is `global_asm!` rather than a separate `.S` file so the whole boot
path is one unit that `cargo` tracks. If that ever stops being possible, the
drop-in replacement is `src/boot/boot.S` plus `#[link_section]`.

## Console: two sinks, one API

`console::putc` talks to whichever sink is known to be alive:

1. **UART** (NS16550A at `0x1000_0000`) after `console::init()` — this is what
   QEMU forwards to `-serial stdio` / `-nographic`, and it is roughly an order
   of magnitude faster than the firmware console;
2. **SBI legacy console** (`legacy_putch`, extension `0x01`) as the fallback,
   including inside the panic handler.

We deliberately do **not** "probe" the UART: on real hardware a write to a
missing device is silently dropped, so a probe tells you nothing. `UART_READY`
is set by `init()` and SBI is the safety net.

OpenSBI marks `legacy_putch` deprecated in favour of the debug-console (`DBCN`)
extension, which needs device-tree parsing. That is a milestone, not a blocker:
the legacy extension is still supported everywhere it matters today. `sbi.rs`
keeps the raw `ecall` available so the DBCN path can be added without
reinventing the calling convention.

## Build and run

```sh
# 0. sanity: is everything installed? (fails readably)
make check

# 1. build the ELF
make build          # -> target/riscv64gc-unknown-none-elf/release/hurd-riscv64-boot

# 2. boot it, headless, serial on stdio
make run
```

Expected output:

```
hurd-riscv64: console up (rv64gc, QEMU virt)
SBI legacy_putc works: yes
SBI spec version 2.0
SBI impl id 0x...
hurd-riscv64: milestone 1 reached, halting
```

`-nographic` is not a stylistic choice: `AGENTS.md` rule 2 forbids GUI stacks
in this lab, and the RISC-V target has no VGA text buffer to read either, so
the UART/SBI path is the only output channel there is.

For debugging, `make debug` starts the machine **stopped**; OpenSBI exposes the
RISC-V debug spec on TCP `:3333`:

```sh
gdb-multiarch -ex 'target remote :3333' -ex 'info registers' \
              target/riscv64gc-unknown-none-elf/release/hurd-riscv64-boot
```

`make disasm` dumps the program headers, the entry point and the first
instructions, which is the fastest way to check that the linker script actually
placed `_start` at the address QEMU loaded.

## Toolchain notes

The `rust-toolchain.toml` pins **nightly** because `.cargo/config.toml`
declares `[unstable] build-std`, and `-Z build-std` is unstable. In practice
everything here also builds on a recent **stable** toolchain, because `core`
is shipped prebuilt for `riscv64gc-unknown-none-elf`:

```sh
cargo +stable build --release        # works, uses the prebuilt core
cargo +nightly build -Z build-std=core,compiler_builtins --release   # rebuilds core
```

Use the second form when hacking on `core` itself, or on a toolchain that has no
prebuilt `core` for the target.

## Milestones

Deliberately ordered so that each one is independently checkable:

1. **Console up** — what is implemented. Proves firmware handoff, `gp`/`sp`/
   `.bss` setup, SBI `ecall`, and a panic path.
2. **Device tree** — parse the OpenSBI-provided DTB passed in `a0`/`a1` to find
   the UART and CLINT. Until this exists there is no timer, and therefore no
   `hardclock` equivalent.
3. **Timer + trap handling** — CLINT `mtime` via the SBI timer extension or the
   `stimecmp` CSR; install a trap handler. This is where the x86-64 experience
   transfers directly: interrupt vector table, per-CPU interrupt stacks,
   spl/interrupt-priority equivalent, and the "only the boot CPU advances the
   clock" rule that caused the `-smp 2` hang in `docs/suivi.md`.
4. **Physical memory** — PLIC, UART interrupt driven, page tables, and the
   first allocator. This is the point where `-Z build-std=core,alloc` becomes
   necessary rather than optional.
5. **Mach-shaped core** — threads, ports, the IPC path. At this point
   `hurd-mig-sys` from `rust/` becomes useful, and the two tracks meet.

Milestones 3–5 are where the hard parts live, and where the lessons from the
x86-64 SMP investigation should be applied deliberately rather than
rediscovered.