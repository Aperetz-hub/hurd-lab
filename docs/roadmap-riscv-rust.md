# Roadmap: RISC-V port and Rust (safe-systems) component track

**Date:** 18/10/2026
**Status:** scaffolding committed; nothing built yet (toolchain absent on the
reference host — verified, see §2.3).

This document is the map for the two expansion paths opened in `hurd-lab`:

1. **`riscv64/`** — a 64-bit RISC-V (`riscv64`) microkernel / boot target
   skeleton.
2. **`rust/`** — a safe-systems approach using Rust (`#![no_std]`) for writing
   or interacting with GNU/Hurd servers (translators, MIG stubs).

Both are **experimental**. They share this repository with the x86_64 fork work
but they do not modify it, and nothing here is allowed to affect the existing
`-smp 2` / virtio work. Everything in this document that touches a VM is
subject to the same rules as the rest of the lab (`AGENTS.md`): headless only,
disposable overlays only, no mutation of `hurd-dev.qcow2` or
`ovl-fork-master.qcow2`.

---

## 1. What was created

| path | purpose |
|---|---|
| `riscv64/README.md` | the rv64gc skeleton: boot contract, console design, milestones |
| `riscv64/Cargo.toml`, `rust-toolchain.toml`, `.cargo/config.toml` | workspace, nightly pin, `panic=abort`, linker script wiring |
| `riscv64/Makefile` | `check` / `build` / `build-std` / `run` / `debug` / `disasm` / `clean` |
| `riscv64/crates/hurd-riscv64-boot/` | the kernel skeleton itself (see below) |
| `rust/README.md` | workspace layout, the two design ideas, the MIG `.defs` pipeline |
| `rust/Cargo.toml`, `rust-toolchain.toml`, `.cargo/config.toml` | workspace, nightly pin, C-ABI-consistent rustflags |
| `rust/crates/hurd-mig-sys/` | `#![no_std]` FFI layer + the `bindgen` build script |
| `rust/crates/hurd-mig-example/` | proof of concept: translator-shaped, `no_std` + `alloc` |

Inside `riscv64/crates/hurd-riscv64-boot/src`:

- `boot.rs` — `_start` trampoline: park non-boot harts, load `gp`, set `sp`,
  zero `.bss`, call `kmain`.
- `sbi.rs` — SBI `ecall` wrappers (`a7` = extension, `a6` = function), with
  `console_putc`, `spec_version`, `impl_id`.
- `uart.rs` — polled NS16550A driver for the QEMU `virt` board (`0x1000_0000`).
- `console.rs` — sink multiplexer plus allocation-free `putc`/`puts`/`puthex`.
- `main.rs` — `kmain()`, the panic handler, and an allocation-free `putdec`.

---

## 2. Toolchain setup on a Debian host

### 2.1 RISC-V cross toolchain

```sh
sudo apt update
sudo apt install gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
                 qemu-system-misc gdb-multiarch
```

- `gcc-riscv64-linux-gnu` gives `riscv64-linux-gnu-gcc`, `readelf`, `objdump`.
  It is used to sanity-check the layout of the kernel ELF
  (`riscv64-linux-gnu-readelf -lW`) and is the toolchain to reach for if the
  Rust path ever needs a C stub (a linker script `PHDRS`-aware trampoline, an
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

### 2.2 Rust

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"

rustup toolchain install nightly
rustup component add rust-src rustfmt clippy --toolchain nightly
rustup target add riscv64gc-unknown-none-elf

# bindgen needs libclang; the CLI is optional (see rust/README.md)
sudo apt install clang libclang-dev
cargo install bindgen-cli      # interactive workflow only
```

- **`rust-src` is not optional** for the `-Z build-std` workflow: `-Z` needs
  the `core` / `alloc` / `compiler_builtins` sources to rebuild them.
- **`bindgen-cli` is deliberately not a build dependency.** `build.rs` uses
  the `bindgen` *library*, so the build cannot break because a globally
  installed CLI is a different version. Install the CLI only for the
  edit-regenerate loop.
- Both `rust-toolchain.toml` files pin nightly because each workspace declares
  an `[unstable] build-std` section. Everything in both workspaces also builds
  on a recent **stable** toolchain (`core` is prebuilt for
  `riscv64gc-unknown-none-elf`; `core` and `alloc` are prebuilt for
  `x86_64-unknown-linux-gnu`), so a stable-only CI box is fine for
  `cargo check`.

### 2.3 Current state of the reference host

Verified on 18/10/2026 — this is why the scaffolding ships unbuilt:

```
ABSENT rustc, cargo, rustup, bindgen, clang, llvm-config
ABSENT qemu-system-riscv64, riscv64-linux-gnu-gcc
Debian GNU/Linux 13 (trixie), x86_64
```

Every "expected output" in `riscv64/README.md` and every claim about generated
bindings is therefore a **design intent**, not a verified result. First action
for whoever picks this up: install §2.1 + §2.2, then run `riscv64/make check`
followed by `make build`.

---

## 3. RISC-V path

### 3.1 Boot chain and why each piece exists

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

### 3.2 Milestones

| # | milestone | why it is the next thing |
|---|---|---|
| 1 | console up | **done in the skeleton** — firmware handoff, `gp`/`sp`/`.bss`, SBI `ecall`, panic path |
| 2 | device tree | parse the DTB OpenSBI passes in `a0`/`a1`; find UART + CLINT. No timer before this |
| 3 | timer + traps | CLINT `mtime`, trap handler, interrupt vector table, per-CPU interrupt stacks, IPL equivalent |
| 4 | physical memory | PLIC, UART interrupts, page tables, first allocator (this is where `-Z build-std` becomes necessary) |
| 5 | Mach-shaped core | threads, ports, IPC — at which point `hurd-mig-sys` from `rust/` becomes useful |

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

### 3.3 Validation without a Hurd userland

Every milestone is validated against the host, not against a userland:

- `riscv64/make run` — expected console output is in `riscv64/README.md`.
- `riscv64/make disasm` — check `_start` really landed at the load address and
  that `.bss` symbols exist; this catches linker-script mistakes that would
  otherwise appear as a silent hang.
- `riscv64/make debug` + `gdb-multiarch` — the machine starts stopped, so the
  first instructions can be single-stepped with the symbols present.

---

## 4. Rust path

### 4.1 The two invariants

**(a) The FFI layer never pulls in `std`.** `hurd-mig-sys` is `#![no_std]`, and
the build script forces bindgen to agree:

| bindgen call | prevents |
|---|---|
| `.use_core()` | `::std::os::raw::c_int` in generated code |
| `.ctypes_prefix("::core::ffi")` | `::std::ffi::c_int` for C typedefs |
| `.raw_line("#![allow(...)]")` | the generated module re-triggering our lints |

Forgetting `.use_core()` is the most common way a generated crate fails to
compile in a `#![no_std]` crate, and the resulting error mentions `std::os::raw`
rather than anything you just changed.

**(b) Bindings always build, but are honest about their provenance.**
`include/mach_ffi.h` includes the real GNU Mach headers when they are found and
falls back to a small layout-compatible subset otherwise. `cargo check` therefore
works on a bare machine and in CI, while a build host with `libhurd-dev` gets
authoritative layouts. The build prints a `cargo:warning` naming the path taken;
*"building against the self-contained fallback"* means the offsets are
unverified.

### 4.2 `no_std` + `alloc` on Hurd

`#![no_std]` does not mean "no heap". `extern crate alloc;` plus a
`#[global_allocator]` is enough, and on Hurd that is the *correct* design
rather than a compromise: the allocator belongs to the translator's C runtime,
so the component forwards to it.

```rust
extern "C" { fn malloc(usize) -> *mut core::ffi::c_void; fn free(*mut core::ffi::c_void); }

struct CRuntimeAlloc;
unsafe impl core::alloc::GlobalAlloc for CRuntimeAlloc {
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        unsafe { malloc(layout.size()).cast() }   // null == OOM, which is what Rust expects
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _l: core::alloc::Layout) {
        unsafe { free(ptr.cast()) }
    }
}
#[global_allocator] static A: CRuntimeAlloc = CRuntimeAlloc;
```

Two rules that are easy to get wrong, and that the `alloc` module in
`hurd-mig-sys/src/lib.rs` spells out:

- `alloc` must return a null pointer **only** on OOM. Wrappers that "always
  succeed" by allocating zero bytes and retrying will loop forever instead of
  hitting `handle_alloc_error`.
- `realloc` should be forwarded to `realloc`. The alloc/copy/dealloc fallback is
  correct but turns every `Vec` growth into two copies — which matters when the
  component sits on the IPC hot path.

### 4.3 MIG `.defs` pipeline

`foo.defs` is a `migcom` input language, not C, so bindgen cannot read it. The
supported chain is:

```
foo.defs ──migcom──> foo.defs.c/.h + <mach>/server.h + fooUser.h
                        │
                     clang
                        │
                     bindgen ──> bindings.rs
```

Recipe:

1. **Generate the C first.** Build the subsystem in a Hurd checkout (for
   example `hurd/hurd/libihelper/`). The output tree then contains
   `mach/server.h` and the `fooUser.h` / `fooServer.h` pair.
2. **Point the build script at it.** `MIG_DEFS=<that tree>` adds it as an
   include root and defines `HAVE_MIG_HEADERS`; `include/mach_ffi.h` already
   includes `<mach/server.h>` and `<mach/mach_port.h>` under that guard. Extend
   the umbrella header for a specific subsystem rather than widening the
   bindgen header list, so there is exactly one place to look.
3. **Read semantics from the generated C, not from the `.defs`.** Message ids
   and argument descriptors are already the compiler's input there, so they
   cannot drift from it.
4. **Pin the layouts.** bindgen emits layout assertions; run `cargo test` on a
   host with the real headers before trusting an offset, and keep the C-side ABI
   flags and `rust/.cargo/config.toml` in the same commit.

### 4.4 Where the safe surface should grow

The generated layer stays untouched. Safety is added *above* it, in
dependency order, because each step only needs the previous one to be correct:

1. `error_string()` and `is_ok()` — already done. Removes the single most
   common source of misuse (comparing `kern_return_t` against a bare integer).
2. An owned right wrapper around `mach_port_t` with `Drop` deallocating, so a
   right cannot leak on an early return.
3. A checked `Message` type over `mach_msg_header_t` with a fixed inline buffer,
   so message sizes are validated before the kernel sees them.
4. Only then, subsystem-specific calls.

Each layer is where a `#![forbid(unsafe_code)]` boundary can be drawn.

---

## 5. Integration with the existing build system

### 5.1 Today

The two tracks are **deliberately not wired into the Hurd build**. The existing
system is autotools-based and lives outside this repository
(`src/gnumach`, `src/hurd`, `src/mig` — see `README.md`). The scaffolding here
is a Cargo workspace plus a `Makefile`, driven independently:

```sh
cd riscv64 && make check && make build && make run
cd rust    && cargo check
```

That separation is intentional. It keeps a broken experimental workspace from
failing the Hurd build, and it keeps this repository free of a Rust toolchain
requirement for anyone who only wants the x86_64 work.

### 5.2 Cross-compilation

| target | built by | notes |
|---|---|---|
| `riscv64gc-unknown-none-elf` | `cargo +nightly build` | no libc; `.cargo/config.toml` sets `panic=abort` and the linker script |
| `x86_64-unknown-linux-gnu` (host) | `cargo build` | default for `rust/`, so `cargo check` works anywhere |
| `x86_64-unknown-hurd` (guest) | `cargo +nightly build --target …` | see below |

Guest builds need three things that are easy to miss:

- a **Hurd sysroot**: the cross headers and the `libhurd-dev` startup objects.
  Point `RUSTFLAGS`/the linker at it rather than at a Linux sysroot — mixing the
  two produces link errors that look like missing symbols in libc.
- **`-C link-arg` for the Hurd startup files** (`crt1.o`, `crti.o`, `crtn.o`)
  that the Hurd package installs; Rust does not add them by itself for a custom
  target.
- **translator linkage**: a component that uses Mach at runtime links against
  `libhurd-dev`, so the Mach symbols resolve through the translator, not
  through a kernel library.

### 5.3 Standard library rebuilding (`-Z build-std`)

Both workspaces declare:

```toml
[unstable]
build-std = ["core", "alloc", "compiler_builtins"]
build-std-features = ["compiler-builtins-mem"]
```

| when | command | why |
|---|---|---|
| host POC | not needed | `core` + `alloc` are prebuilt |
| `riscv64` boot | not needed | `core` is prebuilt for `riscv64gc-unknown-none-elf` |
| `riscv64` with an allocator (milestone 4) | `-Z build-std=core,alloc,compiler_builtins` | no prebuilt `alloc` for a `none` target |
| hacking on `core` itself | same as above | obviously |

`compiler-builtins-mem` is needed because the RISC-V target does not get the
memory intrinsics from the prebuilt `compiler_builtins` in the same way the
Linux targets do; without it, `memcpy`/`memset` symbols go missing at link
time. The RISC-V `Makefile` exposes both paths as `make build` and
`make build-std` so the choice is visible rather than hidden in a flag.

Cost to keep in mind: `-Z build-std` recompiles `core` (and `alloc`) on every
clean, which is minutes, not seconds. Prefer the prebuilt `core` until a
milestone actually needs `alloc`.

### 5.4 Rules these workspaces must not break

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
   Mach-shaped core plus a single Rust server and accept that most of the system
   is missing. The second is far cheaper and is the recommended default.
2. **Rust for existing translators.** The interesting question is not "can Rust
   call Mach" but "is a `#![no_std]` translator-compatible component cheaper to
   make memory-safe than to audit". That is a measurement, not a slogan; a
   realistic target is one real subsystem (file system or pager) ported and
   compared.
3. **Whether `-Z build-std` becomes a hard dependency.** If milestone 4 needs it
   for `alloc`, nightly becomes mandatory for the RISC-V target, and the
   `rust-toolchain.toml` pin is load-bearing rather than optional.
4. **Toolchain availability.** Nothing has been compiled. `riscv64/make check`
   is the gate; until it passes on a host with §2.1 + §2.2 installed, the
   expected outputs in `riscv64/README.md` remain unverified.