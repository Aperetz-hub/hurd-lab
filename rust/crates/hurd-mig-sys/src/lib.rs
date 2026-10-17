/*!
 * hurd-mig-sys -- raw FFI to the GNU Mach / MIG interface.
 *
 * What this crate is:
 *   - `#![no_std]` by construction. It is meant to be linked into a Hurd
 *     translator or into a freestanding component, where `std` does not exist.
 *   - A *thin* layer. The generated bindings are included verbatim so struct
 *     layouts are the compiler's business, and hand-written helpers live on top
 *     rather than around them.
 *
 * What this crate is NOT:
 *   - Not a safe Mach binding. Everything crossing the FFI boundary is `unsafe`
 *     and stays that way; the point of the safe-systems work is to grow a
 *     checked layer above this one, one call at a time.
 *   - Not linkable on a non-Mach host by default. The raw bindings only
 *     *declare* the Mach symbols, which is free, but calling them needs a Mach
 *     runtime. That is what the `runtime` feature is for: a host-only build
 *     (CI, `cargo check`, a logic test suite) keeps the types and skips the
 *     calls, so nothing drags in an undefined symbol at link time.
 *
 * Features:
 *   runtime         enable the convenience wrappers that call into Mach
 *   alloc-support   expose the C allocator declarations (see [`alloc`])
 */

#![no_std]
#![deny(missing_docs)]

/// Bindings produced by `build.rs`.
///
/// Public on purpose: a component occasionally needs an entry point the
/// hand-written layer does not wrap yet, and re-generating the crate for that is
/// pure friction.
///
/// The `#[allow]`s live here rather than in the generated file because
/// `include!` cannot inject inner attributes: bindgen's `.raw_line("#![allow(..)]")`
/// would be a hard compile error, not a style problem.
#[allow(
    non_upper_case_globals,
    non_camel_case_types,
    non_snake_case,
    dead_code,
    missing_docs,
    clippy::all
)]
pub mod raw {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

// Glob re-export: the generated items are documented upstream, in the header.
// It sits at the crate root on purpose -- wrapping it in a module would move the
// generated names out of scope, and every `raw::`-free user (`Krn`, the
// `mach_msg_header_t` alias, ...) would stop resolving.
pub use raw::*;

/* ---------------------------------------------------------------------- */
/* Constants                                                              */
/*                                                                         */
/* Defined here rather than taken from the generated bindings on purpose:   */
/* bindgen only emits simple integer macros, so relying on it would make     */
/* these names silently disappear depending on which header branch was used.   */
/* They shadow the glob import above, which Rust resolves in our favour.      */
/* ---------------------------------------------------------------------- */

/// Return type of every Mach entry point.
pub type Krn = kern_return_t;

/// Mach succeeded.
pub const KERN_SUCCESS: Krn = 0;

/// Return value is not a port name, or the name is already in use.
pub const KERN_INVALID_NAME: Krn = 15;

/// The task name supplied is not a valid task.
pub const KERN_INVALID_TASK: Krn = 16;

/// Insufficient space in the task's address space.
pub const KERN_NO_SPACE: Krn = 3;

/// The name is not a right we hold.
pub const KERN_INVALID_RIGHT: Krn = 17;

/// `true` for [`KERN_SUCCESS`].
#[inline]
pub const fn is_ok(kr: Krn) -> bool {
    kr == KERN_SUCCESS
}

/* ---------------------------------------------------------------------- */
/* Runtime wrappers (feature = "runtime")                                 */
/* ---------------------------------------------------------------------- */

#[cfg(feature = "runtime")]
mod runtime {
    use super::raw;
    use super::Krn;

    /// Human-readable text for a Mach error code.
    ///
    /// # Safety
    ///
    /// `buffer` must be valid for its whole length: `mach_error_string()`
    /// writes into it unconditionally and truncates rather than failing.
    pub unsafe fn error_string(kr: Krn, buffer: &mut [core::ffi::c_char]) -> &str {
        let max = buffer.len();
        if max == 0 {
            return "";
        }
        // SAFETY: caller guarantees the slice is valid for `max` bytes.
        unsafe { raw::mach_error_string(kr, buffer.as_mut_ptr(), max as raw::mach_msg_size_t) };
        // SAFETY: on success mach_error_string() NUL-terminates the buffer.
        unsafe { core::ffi::CStr::from_ptr(buffer.as_ptr()) }
            .to_str()
            .unwrap_or("<non-UTF-8 error string>")
    }

    /// The task port of the calling task.
    ///
    /// # Safety
    ///
    /// Must be called from a thread that actually belongs to a task, i.e. not
    /// before Mach has set up the calling thread.
    pub unsafe fn task_self() -> raw::mach_port_t {
        // SAFETY: caller guarantees a live Mach thread.
        unsafe { raw::mach_task_self() }
    }

    /// Allocate a fresh receive right in the calling task.
    ///
    /// On success the caller owns the name and must eventually deallocate it;
    /// see the `hurd-mig-example` `transport` module for an RAII wrapper that
    /// makes forgetting impossible.
    ///
    /// # Safety
    ///
    /// `port` must point to writable storage for a `mach_port_t`.
    pub unsafe fn allocate_receive_right(port: *mut raw::mach_port_t) -> Krn {
        // SAFETY: caller guarantees `port` is writable.
        unsafe {
            raw::mach_port_allocate(
                // mach_task_self() is documented to always succeed and never
                // transfers ownership, so taking the port by value is fine.
                raw::mach_task_self(),
                raw::MACH_MSG_TYPE_COPY_RECEIVE,
                port,
            )
        }
    }

    /// Release a receive right previously allocated by
    /// [`allocate_receive_right`].
    ///
    /// # Safety
    ///
    /// `port` must name a right this task holds, and must not be used again.
    pub unsafe fn deallocate_receive_right(port: raw::mach_port_t) -> Krn {
        // SAFETY: caller guarantees the right is held and owned here.
        unsafe { raw::mach_port_deallocate(raw::mach_task_self(), port) }
    }

    /// Allocate `count` zeroed bytes in the calling task's address space.
    ///
    /// This is the allocator Hurd translators actually use; see [`crate::alloc`].
    ///
    /// # Safety
    ///
    /// `address` must point to writable storage for a `vm_offset_t`.
    pub unsafe fn vm_allocate(address: *mut raw::vm_offset_t, count: usize) -> Krn {
        // SAFETY: caller guarantees `address` is writable; `count` fits in
        // vm_size_t on this target (both are 64-bit).
        unsafe { raw::mig_allocate(raw::mach_task_self(), count as raw::vm_size_t, address) }
    }

    /// Release memory obtained from [`vm_allocate`].
    ///
    /// # Safety
    ///
    /// `address` must be a live allocation of exactly `count` bytes from
    /// [`vm_allocate`], and must not be touched afterwards.
    pub unsafe fn vm_deallocate(address: raw::vm_offset_t, count: usize) -> Krn {
        // SAFETY: caller guarantees the allocation is live and owned here.
        unsafe { raw::mig_deallocate(raw::mach_task_self(), address, count as raw::vm_size_t) }
    }
}

/// Convenience wrappers that call into Mach.
///
/// Absent unless the `runtime` feature is enabled: see the crate-level docs for
/// why a host build must be able to skip these.
#[cfg(feature = "runtime")]
pub use runtime::{
    allocate_receive_right, deallocate_receive_right, error_string, task_self, vm_allocate,
    vm_deallocate,
};

/* ---------------------------------------------------------------------- */
/* alloc support                                                          */
/* ---------------------------------------------------------------------- */

/**
 * Allocation hooks for `#![no_std]` components that need `alloc`.
 *
 * A `#![no_std]` crate that uses `alloc` (through `Vec`, `String`, `Box`, …)
 * must supply a `#[global_allocator]`. On Hurd that is the translator's C
 * runtime, so the allocator is a thin forwarding shim rather than anything
 * self-contained:
 *
 * ```ignore
 * use core::alloc::{GlobalAlloc, Layout};
 *
 * extern "C" {
 *     fn malloc(size: usize) -> *mut core::ffi::c_void;
 *     fn free(ptr: *mut core::ffi::c_void);
 * }
 *
 * struct HurdAlloc;
 *
 * unsafe impl GlobalAlloc for HurdAlloc {
 *     unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
 *         unsafe { malloc(layout.size()).cast() }
 *     }
 *     unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
 *         unsafe { free(ptr.cast()) }
 *     }
 * }
 * ```
 *
 * Two rules that are easy to get wrong here:
 *
 *  * `alloc` must return a non-null, suitably aligned pointer, or signal
 *    failure. Rust's `handle_alloc_error` expects a null return to mean OOM, so
 *    do not wrap `malloc` in something that retries instead of propagating.
 *  * `realloc` should be forwarded to `realloc` when available; falling back to
 *    alloc/copy/dealloc is correct but turns every `Vec` growth into two copies.
 */
#[cfg(feature = "alloc-support")]
pub mod alloc {
    extern "C" {
        /// The translator's allocator.
        pub fn malloc(size: usize) -> *mut core::ffi::c_void;
        /// Free a pointer returned by [`malloc`].
        pub fn free(ptr: *mut core::ffi::c_void);
        /// Resize a pointer returned by [`malloc`].
        pub fn realloc(ptr: *mut core::ffi::c_void, size: usize) -> *mut core::ffi::c_void;
    }

    /// Alignment the C allocator is guaranteed to satisfy.
    ///
    /// Useful when sizing an inline buffer for the "no allocation at all" path,
    /// which is the right default for a boot component.
    pub const MALLOC_ALIGN: usize = core::mem::size_of::<usize>();
}
