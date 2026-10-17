# rust/ — Safe-systems Rust for GNU/Hurd (translator-shaped components)

This workspace is the experimental track for **writing or interacting with
GNU/Hurd servers (translators, MIG stubs) in Rust**.

> Status: **working and validated** on Debian 13 with stable Rust 1.99 —
> `cargo check`, `cargo clippy`, `cargo fmt --check`, `cargo test` (32 tests) and
> `cargo run` all pass. It has *not* been built for, or run inside, the Hurd
> guest yet (see "Not done yet").

## Layout

```
rust/
├── Cargo.toml                 workspace + panic="abort" (required, see below)
├── rust-toolchain.toml        STABLE; nightly is opt-in, for -Z build-std only
├── .cargo/config.toml         target selection, overflow checks, -lc
└── crates/
    ├── hurd-mig-sys/          raw, generated FFI layer       (#![no_std])
    │   ├── build.rs           bindgen driver (see the long comment at the top)
    │   ├── include/mach_ffi.h umbrella header: real Mach headers if found,
    │   │                      self-contained fallback otherwise
    │   └── src/lib.rs         the thin hand-written layer + alloc hooks
    └── hurd-mig-example/      a minimal translator-shaped component
        ├── build.rs           exposes the build target to the binary
        ├── src/lib.rs         all the logic, always #![no_std]  <- tested here
        ├── src/message.rs     mach_msg_header_t layout + validation
        ├── src/protocol.rs    MIG-shaped message ids
        ├── src/transport.rs   Transport trait: Loopback (host) / Mach (guest)
        ├── src/server.rs      the dispatcher
        └── src/main.rs        global allocator, entry point, demo transcript
```

## Running it

```sh
cd rust
cargo test                          # 32 tests, no VM, no QEMU
cargo run --release -p hurd-mig-example
```

`cargo run` prints a transcript that walks the whole path — three well-formed
requests, two hostile ones, the loop API, and malformed bytes at the transport
boundary:

```
-- dispatch --
  -> PONG (reply, id 0x95000101) to port 101
  -> ECHO_REPLY (reply, id 0x95000102) to port 101
     body u32 = 0xdeadbeef
  refused FOREIGN: message id is from another subsystem
  -> ERROR_REPLY (reply, id 0x95000100) to port 101
     body u32 = 0x00000004
  refused UNASSIGNED: unknown message id in this subsystem
  -> ERROR_REPLY (reply, id 0x95000100) to port 101
-- malformed input at the transport boundary --
  4-byte buffer  : rejected (Invalid(TooShort))
  lying msgh_size: rejected (Invalid(SizeMismatch { declared: 4, available: 24 }))
```

## The four ideas this workspace is built around

### 1. Keep `std` out of the FFI layer, keep `alloc` available

`hurd-mig-sys` is `#![no_std]`, and the build script is written so that
bindgen never emits a `::std::` path:

| bindgen call | what it prevents |
|---|---|
| `.use_core()` | `::std::os::raw::c_int` in generated code |
| `.ctypes_prefix("::core::ffi")` | `::std::ffi::c_int` for the C typedefs |
| `#[allow(...)]` on the `raw` module | the generated items re-triggering our lints |

Forgetting `.use_core()` is the single most common reason a generated crate
fails to compile in a `#![no_std]` crate, with an error about `std::os::raw` that
looks unrelated to what you just did.

Note what is **not** in `build.rs`: `.raw_line("#![allow(..)]")`. A macro
expansion cannot inject inner attributes, so `include!`-ing that file is a hard
compile error (`an inner attribute is not permitted in this context`). The
`#[allow]` goes on the `pub mod raw { include!(..) }` instead.

On the consumer side, `#![no_std]` does **not** mean "no heap":
`extern crate alloc;` plus a `#[global_allocator]` that forwards to the C
runtime is all it takes, and that is exactly what `hurd-mig-example` does. On
Hurd the allocator belongs to the translator's C runtime, so forwarding is the
correct design rather than a compromise — see the `alloc` module in
`hurd-mig-sys/src/lib.rs` for the two rules that are easy to get wrong (`alloc`
must return null on OOM, `realloc` should be forwarded rather than emulated).

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
HURD_INCLUDE=/usr/include/i386-gnu-hurd cargo build -vv
# Or from a checkout:
HURD_SRC=/path/to/hurd cargo build -vv
```

The build prints a `cargo:warning` naming the path taken; *"building against the
self-contained fallback"* means the struct layouts are a stand-in and are
unverified. **The committed results were built against the fallback**, because
`libhurd-dev` is not installed on the reference host.

### 3. Validate before the kernel sees the buffer

`message.rs` is where the safety actually lives, and it is the reason this
component is worth having. `Message::from_bytes()` is the boundary where
untrusted data enters, and it refuses:

- a buffer shorter than `mach_msg_header_t`;
- a `msgh_size` that disagrees with the buffer actually received (clamping or
  trusting the smaller of the two is how translators end up reading past a
  buffer);
- a `msgh_size_rounded` that is not `msgh_size` rounded to a word;
- a send with no `msgh_remote_port`;
- a body of the wrong width when a scalar is read (`body_u32` returns `None`
  rather than truncating).

The header layout is asserted against `size_of` at compile time, because a
silent layout drift would corrupt every message rather than fail.

The dispatcher is *total*: every input produces either a reply or an error
reply, and a request that is itself a reply is refused rather than answered —
otherwise two peers would trade replies forever.

### 4. The transport is a trait, so the component is testable

`Transport` has two implementations. `Loopback` runs on the host and is
directional — `send` fills an outbox, `recv` drains an inbox, and `deliver()`
plays the part of the kernel moving one to the other. That is not cosmetic: a
single-queue model lets a server loop eat its own replies and never terminate,
which is exactly what the first version of this file did before the tests
caught it.

`Mach` (feature `mach`) is the real thing, and owns its receive right in an RAII
wrapper so a port cannot leak on an early return.

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
   about (e.g. `hurd/hurd/libihelper/`). The output tree then contains
   `mach/server.h` and the `fooUser.h`/`fooServer.h` pair.
2. **Point the build script at it.** `MIG_DEFS=<that tree>` adds it as an
   include root and defines `HAVE_MIG_HEADERS`; `include/mach_ffi.h` already
   includes `<mach/server.h>` and `<mach/mach_port.h>` under that guard. Extend
   the umbrella header for a specific subsystem rather than widening the bindgen
   header list, so there is exactly one place to look.
3. **Do not try to parse `.defs` directly.** If you need the *semantics*
   (message ids, argument descriptors), read them from the generated C: it is
   already the compiler's input and is guaranteed consistent with it. The
   numeric ids in the generated `foo.defs.c` are the ground truth.
4. **Pin the layouts.** bindgen emits `#[test]` layout assertions; run
   `cargo test` on a host that has the real headers before trusting an offset.
   Keep the C-side ABI flags and `rust/.cargo/config.toml` in the same commit.

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

## Toolchain notes

| thing | why |
|---|---|
| **stable**, not nightly | `core` and `alloc` are prebuilt for `x86_64-unknown-linux-gnu`. Pinning nightly would force a ~150 MB download per contributor for nothing. |
| `-Z build-std` is opt-in | Do **not** put `[unstable] build-std` in `.cargo/config.toml`: it turns standard-library rebuilding on by default, and cargo then builds its own `core` on top of the prebuilt one — `error[E0152]: duplicate lang item in crate core: sized`. Use `cargo +nightly build -Z build-std=…` explicitly, which also needs `rustup component add rust-src`. |
| `panic = "abort"` in the workspace profile | Required, not stylistic: a `#![no_std] #![no_main]` crate defines its own `#[panic_handler]` and links no unwinder. Cargo applies the setting to the binary but keeps unwinding for test targets, which is exactly what is needed. |
| `-C link-arg=-lc` | A `no_std` crate is still a *hosted* program here, and rustc links freestanding crates with `-nodefaultlibs`. On Hurd the equivalent is linking `libhurd-dev`. |
| `rust_eh_personality` stub | Prebuilt `alloc` references it; with `panic = "abort"` nothing can unwind but the symbol must still exist. The alternative is `-Z build-std`, i.e. nightly. |
| **no `+soft-float`** | Meaningless on x86_64, and it applies to build scripts too — with it in `.cargo/config.toml`, *every* build script in the workspace segfaulted during `cargo check`. |
| `bindgen = "0.72"` | 0.73 widens its `prettyplease` requirement to `<0.4`, and the 0.3 line was observed segfaulting in its build script on this host. |

## Not done yet

- **Not built for the guest.** Everything above is the host build. Cross-
  compilation for Hurd needs a Hurd sysroot and the translator startup files;
  see `docs/roadmap-riscv-rust.md` §5.2.
- **No real MIG subsystem.** The component speaks its own small protocol in the
  MIG id layout. Wiring a real one is the next milestone.
- **No safe abstraction boundary yet.** Everything crossing the FFI boundary is
  `unsafe`; the generated layer stays untouched and safety is added above it.
  The planned order is: owned Mach rights with `Drop`, a checked `Message` type
  (done, in `message.rs`), then subsystem calls.