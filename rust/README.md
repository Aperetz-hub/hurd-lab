# rust/ — Safe-systems Rust for GNU/Hurd (proof-of-concept scaffolding)

This workspace is the experimental track for **writing or interacting with
GNU/Hurd servers (translators, MIG stubs) in Rust**. It is a skeleton: the
structure, the FFI plumbing and the conventions are in place; the interesting
work starts with the first real MIG subsystem.

> Status: **blueprint**. Nothing here has been compiled yet on the reference
> host (see `docs/roadmap-riscv-rust.md` for the toolchain that is missing).

## Layout

```
rust/
├── Cargo.toml                 workspace root
├── rust-toolchain.toml        nightly + rust-src (needed for -Z build-std)
├── .cargo/config.toml         target selection and C-ABI-consistent rustflags
└── crates/
    ├── hurd-mig-sys/          raw, generated FFI layer  (#![no_std])
    │   ├── build.rs           bindgen driver (see the long comment at the top)
    │   ├── include/mach_ffi.h umbrella header: real Mach headers if found,
    │   │                      self-contained fallback otherwise
    │   └── src/lib.rs         the thin hand-written layer + alloc hooks
    └── hurd-mig-example/      proof of concept: a translator-shaped binary
        └── src/main.rs        #![no_std] + alloc + a Mach receive-right probe
```

## The two ideas this workspace is built around

### 1. Keep `std` out of the FFI layer, keep `alloc` available

`hurd-mig-sys` is `#![no_std]`, and the build script is written so that
bindgen never emits a `::std::` path:

| bindgen call | what it prevents |
|---|---|
| `.use_core()` | `::std::os::raw::c_int` in generated code |
| `.ctypes_prefix("::core::ffi")` | `::std::ffi::c_int` for the C typedefs |
| `.raw_line("#![allow(...)]")` | generated module re-triggering our lints |

On the consumer side, `#![no_std]` does **not** mean "no heap": `extern crate
alloc;` plus a `#[global_allocator]` that forwards to the C runtime is all it
takes, and that is exactly what `hurd-mig-example` does. On Hurd the allocator
belongs to the translator's C runtime, so forwarding is the correct design
rather than a compromise — see the `alloc` module in `hurd-mig-sys/src/lib.rs`
for the two rules that are easy to get wrong (`alloc` must return null on OOM,
`realloc` should be forwarded rather than emulated).

### 2. Bindings must come from the real headers, and must always build

`include/mach_ffi.h` is a single stable entry point for bindgen. It includes
the real GNU Mach headers when they are available and falls back to a small,
layout-compatible subset when they are not, so `cargo check` works on a bare
machine and in CI while still generating authoritative layouts on a build host
with `libhurd-dev` installed.

Header discovery order (first hit wins):

1. `$HURD_INCLUDE` — colon-separated list of include roots
2. `$HURD_SRC/hurd/gnumach` and `$HURD_SRC/hurd` — a source checkout
3. `/usr/include/i386-gnu/hurd` — Debian cross headers
4. `/usr/local/include/i386-gnu/hurd`

```sh
# Generate against the real headers:
HURD_INCLUDE=/usr/include/i386-gnu/hurd cargo build -vv
# Or from a checkout:
HURD_SRC=/path/to/hurd cargo build -vv
```

The build prints a `cargo:warning` saying which of the two paths was taken; if
you see *"building against the self-contained fallback"*, your bindings are a
stand-in and the struct layouts are unverified.

## Parsing MIG `.defs` files

MIG subsystem definitions are **not** C and not headers: `foo.defs` is a
`migcom` input language. bindgen cannot read it directly, and running `migcom`
by hand does not scale. The supported pipeline is:

```
   foo.defs
       │  migcom (part of the Hurd build; not installed standalone)
       ▼
   foo.defs.c / foo.defs.h  +  <mach>/server.h  +  foo{User,Server}.h
       │  clang
       ▼
   bindgen  ──►  bindings.rs
```

Practical recipe, in order:

1. **Generate the C first.** Run the Hurd build for the subsystem you care
   about (`make` inside `hurd/hurd/libihelper/`, or the top-level
   `hurd/hurd` build). The output tree then contains `mach/server.h` and
   `fooUser.h`/`fooServer.h`. Point `MIG_DEFS` at that tree — the build script
   adds it as an include root and defines `HAVE_MIG_HEADERS`.
2. **Let the umbrella header pull them in.** It already includes
   `<mach/server.h>` and `<mach/mach_port.h>` when `HAVE_MIG_HEADERS` is set;
   extend it for the specific subsystem header you want rather than widening
   the bindgen header list, so there is exactly one place to look.
3. **Do not try to parse `.defs` directly.** If you need the *semantics*
   (message ids, argument descriptors), read them from the generated C: it is
   already the compiler's input and is guaranteed consistent with it. The
   numeric ids in the generated `foo.defs.c` are the ground truth.
4. **Pin the layouts.** bindgen emits `#[test]` layout assertions; run
   `cargo test` on a host that has the real headers before trusting an offset.
   Keep the C-side ABI flags (`-m64`, packing pragmas) and
   `rust/.cargo/config.toml` in the same commit.

For the interactive loop — regenerating bindings while you edit a header — use
the standalone CLI rather than the build script:

```sh
sudo apt install clang libclang-dev
cargo install bindgen-cli

bindgen \
  rust/crates/hurd-mig-sys/include/mach_ffi.h \
  -I /usr/include/i386-gnu/hurd \
  --use-core --ctypes-prefix '::core::ffi' \
  --no-layout-tests --no-doc-comments \
  -o /tmp/bindings.rs
```

`bindgen-cli` is deliberately **not** a build dependency: using the library in
`build.rs` means the build cannot break because a globally installed CLI is a
different version.

## What this workspace does not do yet

- No actual MIG subsystem is wired up. `hurd-mig-example` only probes
  `mach_task_self()` / `mach_port_allocate()`, which is enough to prove the ABI
  plumbing but nothing more.
- No safe abstraction layer. Everything crossing the boundary is `unsafe`; the
  plan is to grow a checked layer above it one call at a time, keeping the
  generated layer untouched.
- Not built for the guest yet. The POC targets the host; see
  `docs/roadmap-riscv-rust.md` for the `-Z build-std` and cross-compilation
  integration with the existing build system.