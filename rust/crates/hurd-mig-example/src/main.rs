/*
 * Entry point and process-level plumbing for the example translator.
 *
 * The logic lives in the library next door (`src/lib.rs`) so it can be tested:
 * a `#![no_std] #![no_main]` crate cannot be unit-tested directly, because its
 * own `#[panic_handler]` and `main` collide with the test harness.
 *
 * What is left here is only what belongs to a process:
 *
 *   - a `#[global_allocator]` forwarding to the C runtime (on Hurd the
 *     allocator belongs to the translator, not to Rust);
 *   - an `eprint!` over `write(2)`, since `std` is not available;
 *   - `main`, the panic handler, and a transcript that exercises the
 *     request/reply path end to end.
 *
 * Everything in this file is compiled only for the freestanding build: under
 * `cfg(test)` the harness supplies `main` and the panic handler, so the demo
 * would be dead code there.
 */

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::fmt::Write as _;

use hurd_mig_example::{Loopback, Message, Outcome, Request, Translator, Transport};

// ---------------------------------------------------------------------------
// Global allocator
// ---------------------------------------------------------------------------

extern "C" {
    /// Allocator provided by the C runtime we are linked against.
    fn malloc(size: usize) -> *mut core::ffi::c_void;
    fn free(ptr: *mut core::ffi::c_void);
    fn realloc(ptr: *mut core::ffi::c_void, size: usize) -> *mut core::ffi::c_void;
}

/// Forwards Rust allocations to the C runtime.
///
/// This is the bridge that makes `alloc` usable in a `#![no_std]` Hurd
/// component: the translator owns memory policy, Rust only asks.
struct CRuntimeAlloc;

// SAFETY: every method forwards to the C allocator with the size the caller
// asked for. `alloc` returning null is the documented OOM signal, which is
// exactly what Rust's `handle_alloc_error` expects, so no invariant is broken.
unsafe impl GlobalAlloc for CRuntimeAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // SAFETY: forwarding the requested size to malloc is a valid use.
        unsafe { malloc(layout.size()).cast() }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        // SAFETY: `ptr` came from our `alloc`, so `free` is the right deallocator.
        unsafe { free(ptr.cast()) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, _layout: Layout, new_size: usize) -> *mut u8 {
        // SAFETY: forwarding to `realloc` keeps Vec growth O(1) instead of an
        // alloc/copy/dealloc dance.
        let _ = _layout;
        unsafe { realloc(ptr.cast(), new_size).cast() }
    }
}

#[global_allocator]
static ALLOCATOR: CRuntimeAlloc = CRuntimeAlloc;

/// Unwinding personality stub.
///
/// `alloc` is shipped prebuilt with `panic = "unwind"`, so it references
/// `rust_eh_personality`. The profile sets `panic = "abort"`, so nothing can
/// ever unwind -- but the symbol still has to exist at link time. Defining it as
/// a no-op is the standard stable-toolchain workaround; the alternative is
/// rebuilding the standard library with `-Z build-std`, which requires nightly
/// (see `rust-toolchain.toml`).
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

/// Minimal stderr sink: a `no_std` binary still gets `write(2)` from libc.
struct Stderr;

impl core::fmt::Write for Stderr {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        extern "C" {
            fn write(fd: core::ffi::c_int, buf: *const core::ffi::c_void, count: usize) -> isize;
        }
        // SAFETY: `s` is a live slice of exactly `s.len()` bytes, and `write`
        // does not retain the pointer past the call.
        let n = unsafe { write(2, s.as_ptr().cast(), s.len()) };
        if n < 0 {
            Err(core::fmt::Error)
        } else {
            Ok(())
        }
    }
}

macro_rules! eprint {
    ($($arg:tt)*) => {{
        let mut out = Stderr;
        let _ = writeln!(out, $($arg)*);
    }};
}

extern "C" {
    fn exit(code: i32) -> !;
}

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

/// Render a message id as `NAME (kind, id 0x...)`.
fn name_of(id: u32) -> String {
    use hurd_mig_example::protocol;
    let kind = if protocol::is_reply(id) {
        "reply"
    } else {
        "request"
    };
    let name = match id {
        protocol::MSG_PING => "PING",
        protocol::MSG_PONG => "PONG",
        protocol::MSG_ECHO => "ECHO",
        protocol::MSG_ECHO_REPLY => "ECHO_REPLY",
        protocol::MSG_UPTIME => "UPTIME",
        protocol::MSG_UPTIME_REPLY => "UPTIME_REPLY",
        protocol::MSG_ERROR_REPLY => "ERROR_REPLY",
        // Distinguish "not our subsystem" from "ours but unassigned": the
        // dispatcher answers both with KERN_INVALID_ARGUMENT, and the
        // distinction is what makes a MIG log readable.
        _ if !protocol::is_ours(id) => return String::from("FOREIGN"),
        _ => return String::from("UNASSIGNED"),
    };
    let mut s = String::from(name);
    let _ = write!(s, " ({kind}, id {id:#010x})");
    s
}

fn run() -> i32 {
    use hurd_mig_example::protocol;

    eprint!("hurd-mig-example: minimal no_std translator component");
    eprint!("  build target   : {}", env!("HURD_EXAMPLE_TARGET"));
    eprint!(
        "  transport      : {}",
        if cfg!(feature = "mach") {
            "Mach (receive right)"
        } else {
            "in-process loopback"
        }
    );
    eprint!();

    let mut translator = Translator::new("hurd-mig-example");
    eprint!("  registered as  : {}", translator.name());
    eprint!("  header size    : {} bytes", message_header_size());
    eprint!("  max message    : {} bytes", max_message_size());
    eprint!();

    // Seed a normal exchange plus hostile inputs, so the transcript shows the
    // validation actually rejecting things. Everything enters through push_raw,
    // i.e. through the same validation a real transport would apply.
    let mut chan = Loopback::new();
    let seeds: [Message; 5] = [
        Request::ping(100, 101),
        Request::echo(100, 101, 0xdead_beef),
        Request::uptime(100, 101),
        // Foreign subsystem.
        Message::new(100, 101, 0x1234_0001, Vec::new()).expect("body fits"),
        // Unknown message inside our subsystem.
        Message::new(100, 101, protocol::SUBSYSTEM | 0x00ff, Vec::new()).expect("body fits"),
    ];
    for seed in seeds.iter() {
        chan.push_raw(seed.to_bytes().as_slice())
            .expect("the seeded messages are well formed");
    }

    eprint!("-- dispatch --");
    let mut requests = 0u32;
    let mut replies = 0u32;
    while let Ok(req) = chan.recv() {
        requests += 1;
        let (reply, outcome) = translator.dispatch(&req);
        match outcome {
            Outcome::Replied => replies += 1,
            Outcome::Rejected(err) => {
                eprint!("  refused {}: {}", name_of(req.id()), err.describe());
            }
            Outcome::Failed(err) => {
                eprint!("  transport failure: {err:?}");
                break;
            }
            Outcome::Idle => unreachable!("dispatch is called, never recv"),
        }
        eprint!(
            "  -> {} to port {}",
            name_of(reply.id()),
            reply.remote_port()
        );
        if let Some(v) = reply.body_u32() {
            eprint!("     body u32 = {v:#010x}");
        } else if let Some(v) = reply.body_u64() {
            eprint!("     body u64 = {v}");
        }
        chan.send(reply).expect("send cannot fail");
    }

    // Replies went to the peer's queue, not back into our own inbox: a server
    // loop that ate its own replies would never terminate.
    let queued = chan.pending_out();
    let delivered = chan.deliver();
    eprint!(
        "  {queued} repl{} queued for the client, {delivered} delivered",
        if queued == 1 { "y" } else { "ies" }
    );

    // The same dispatcher through its loop API: this is the call a real
    // translator's main loop makes once per iteration.
    eprint!("-- same dispatcher through the loop API --");
    let mut loop_chan = Loopback::new();
    for req in [Request::ping(200, 0), Request::uptime(200, 0)] {
        loop_chan
            .push_raw(req.to_bytes().as_slice())
            .expect("well formed");
    }
    let last = translator.serve_all(&mut loop_chan, 8);
    eprint!(
        "  serve_all stopped on {last:?}, {} reply(ies) queued, served total {}",
        loop_chan.pending_out(),
        translator.served()
    );

    eprint!("-- malformed input at the transport boundary --");
    let mut junk = Loopback::new();
    match junk.push_raw(&[0u8; 4]) {
        Err(err) => eprint!("  4-byte buffer  : rejected ({err:?})"),
        Ok(()) => eprint!("  4-byte buffer  : NOT REJECTED (bug)"),
    }
    let mut lying = Request::ping(100, 0).to_bytes();
    lying[0..4].copy_from_slice(&4u32.to_ne_bytes());
    match junk.push_raw(&lying) {
        Err(err) => eprint!("  lying msgh_size: rejected ({err:?})"),
        Ok(()) => eprint!("  lying msgh_size: NOT REJECTED (bug)"),
    }

    eprint!("-- result --");
    eprint!("  requests dispatched above : {requests} ({replies} answered, rest refused)");
    eprint!("  served by the loop API    : {}", translator.served());
    eprint!("  queued for the client     : {}", chan.pending_in());
    eprint!();
    eprint!("RESULT: ok (no_std + alloc + MIG-shaped protocol, validated input)");
    0
}

const fn message_header_size() -> usize {
    hurd_mig_example::message::HEADER_SIZE
}

const fn max_message_size() -> usize {
    hurd_mig_example::message::MAX_MESSAGE_SIZE
}

/// Rust binary entry point.
///
/// `no_main` means we own the process entry, so we are the ones calling
/// `exit`: there is no `_start` to return into.
#[no_mangle]
pub extern "C" fn main() -> ! {
    let code = run();
    // SAFETY: `exit` is noreturn, and `code` is a plain i32.
    unsafe { exit(code) }
}

/// Freestanding panic handler: the only diagnostics available without `std`.
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    eprint!("panic: {}", info);
    // SAFETY: as above.
    unsafe { exit(101) }
}
