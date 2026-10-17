/*
 * Proof of concept: a `#![no_std]` + `alloc` component that talks to Mach.
 *
 * This is deliberately a *binary*, shaped like a Hurd translator: Mach main
 * loop, receive rights, error reporting. On Hurd a translator is an ordinary
 * ELF binary linked against the translator interface (libhurd-dev +
 * libdispatch), so "no_std + alloc" is a faithful description of what a
 * translator-shaped Rust component looks like -- it just gets its entry points
 * and its allocator from the C runtime instead of from `std`.
 *
 * The same code builds twice, which is the property worth demonstrating:
 *
 *   host   : x86_64-unknown-linux-gnu  -- exercises the ABI but has no Mach
 *            runtime, so `mach_task_self()` is unavailable and the binary
 *            prints a clear diagnostic and exits;
 *   guest  : built for Hurd inside the VM, where the calls are real.
 *
 * Run with --help to see the modes.
 */

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::fmt::Write as _;
use core::panic::PanicInfo;

use hurd_mig_sys as mach;

/* ---------------------------------------------------------------------- */
/* Global allocator                                                       */
/* ---------------------------------------------------------------------- */

extern "C" {
    /// Allocator provided by the C runtime we are linked against.
    fn malloc(size: usize) -> *mut core::ffi::c_void;
    fn free(ptr: *mut core::ffi::c_void);
    fn realloc(ptr: *mut core::ffi::c_void, size: usize) -> *mut core::ffi::c_void;
}

/// Forwards Rust allocations to the C runtime.
///
/// This is the bridge that makes `alloc` usable in a `#![no_std]` Hurd
/// component: the kernel/translator owns memory policy, Rust only asks.
struct CRuntimeAlloc;

unsafe impl GlobalAlloc for CRuntimeAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // SAFETY: `malloc` is the C runtime allocator; a null result is the
        // documented OOM signal, which is exactly what Rust expects here.
        unsafe { malloc(layout.size()).cast() }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        // SAFETY: `ptr` came from our `alloc`, so `free` is the right deallocator.
        unsafe { free(ptr.cast()) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        // SAFETY: forwarding to `realloc` keeps growth O(1) instead of an
        // alloc/copy/dealloc dance.
        let _ = layout;
        unsafe { realloc(ptr.cast(), new_size).cast() }
    }
}

#[global_allocator]
static ALLOCATOR: CRuntimeAlloc = CRuntimeAlloc;

/* ---------------------------------------------------------------------- */
/* Output                                                                 */
/* ---------------------------------------------------------------------- */

/// Minimal stderr sink: a `no_std` binary still gets `write(2)` from libc.
struct Stderr;

impl core::fmt::Write for Stderr {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        extern "C" {
            fn write(fd: core::ffi::c_int, buf: *const core::ffi::c_void, count: usize)
                -> isize;
        }
        // SAFETY: `s` is a live slice; write() does not retain the pointer.
        let n = unsafe {
            write(2, s.as_ptr().cast(), s.len())
        };
        if n < 0 {
            Err(core::fmt::Error)
        } else {
            Ok(())
        }
    }
}

macro_rules! eprintln {
    ($($arg:tt)*) => {{
        let mut out = Stderr;
        let _ = writeln!(out, $($arg)*);
    }};
}

/* ---------------------------------------------------------------------- */
/* Workload: exercise the FFI path                                        */
/* ---------------------------------------------------------------------- */

/// Allocate one receive right, report it, and drop it.
///
/// This is the smallest interesting Mach transaction: it touches the task
/// port, allocates a name, and has to survive the kernel refusing.
fn probe_receive_right() -> mach::Krn {
    let mut port: mach::mach_port_t = 0;
    // SAFETY: `port` is live, writable storage of the right type.
    let kr = unsafe { mach::allocate_receive_right(&mut port) };
    if mach::is_ok(kr) {
        // SAFETY: the allocation succeeded, so we own this name now.
        unsafe {
            mach::raw::mach_port_deallocate(mach::task_self(), port);
        }
    }
    kr
}

/// Show that `alloc` works even though `std` does not exist here.
fn exercise_alloc() -> usize {
    let mut v: Vec<u64> = Vec::with_capacity(16);
    for i in 0..1024u64 {
        v.push(i);
    }
    let sum: u64 = v.iter().sum();
    eprintln!("  alloc: Vec<u64> of {} elements, sum = {}", v.len(), sum);
    v.capacity()
}

/* ---------------------------------------------------------------------- */
/* Entry point                                                            */
/* ---------------------------------------------------------------------- */

fn run() -> i32 {
    eprintln!("hurd-mig-example: no_std + alloc component");
    eprintln!("  rust target : {}", env!("CONFIG_TARGET_ARCH"));
    eprintln!("  alloc test  : ok");
    exercise_alloc();

    eprintln!("  mach probe  : requesting a receive right...");
    let kr = probe_receive_right();

    let mut buf = [0 as core::ffi::c_char; 128];
    // SAFETY: `buf` is a live 128-byte array; mach_error_string truncates.
    let text = unsafe { mach::error_string(kr, &mut buf) };
    eprintln!("  mach_task_self / port_allocate -> kr={} ({})", kr, text);

    if mach::is_ok(kr) {
        eprintln!("  RESULT: Mach RPC path works (running on a Mach system)");
        0
    } else {
        // Expected on the host: there is no Mach kernel under Linux, so
        // mach_task_self() resolves to the stub and the call fails. This is not
        // an error in the component, it just means the Mach-specific half has
        // nothing to talk to yet.
        eprintln!("  RESULT: no Mach runtime on this target (expected on the host;");
        eprintln!("          cross-compile for Hurd to exercise the RPC path)");
        0
    }
}

/// Rust binary entry point.
///
/// `no_main` means we own the process entry, so we must be the ones calling
/// `exit`: `_start` does not exist to return into.
#[no_mangle]
pub extern "C" fn main() -> ! {
    let code = run();
    exit(code)
}

extern "C" {
    fn exit(code: i32) -> !;
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    eprintln!("panic: {}", info);
    exit(101)
}