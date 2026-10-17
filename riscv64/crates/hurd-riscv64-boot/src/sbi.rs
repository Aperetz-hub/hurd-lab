/*
 * Thin wrappers over the RISC-V Supervisor Binary Interface.
 *
 * The calling convention is fixed by the SBI specification and is not the same
 * as the RISC-V standard calling convention, which is why these are written as
 * raw `ecall`s instead of normal Rust functions:
 *
 *   a7 = extension ID
 *   a6 = function ID
 *   a0..a5 = arguments, a0/a1 = return value
 *
 * Every wrapper returns `(a0, a1)` so callers can distinguish the SBI error
 * codes defined by the specification (negative errno values) from a real result.
 * `a1` carries the extension-level code when an extension is not supported.
 *
 * Only `ecall` is used. The time/dbase/IPI extensions are deliberately not
 * wired yet: they need the device tree to find the timer, which is the first
 * real milestone after this skeleton (see README.md).
 */

/// Supervisor call extension: the base `legacy` extension set.
pub const SBI_SYSCALL: usize = 0x10;

/// `sbi_get_spec_version` — spec version encoded as (major << 24) | minor.
pub const FN_GET_SPEC_VERSION: usize = 0;

/// `sbi_get_impl_id` — firmware implementation identifier.
pub const FN_GET_IMPL_ID: usize = 0x01;

/// `SBI_EXT_LEGACY_CONSOLE` — the pre-0.2 console extension.
pub const EXT_LEGACY_CONSOLE: usize = 0x01;

/// `legacy_putch`: `a0` = character to emit.
pub const FN_LEGACY_PUTCH: usize = 0x00;

/// Issue one SBI `ecall`.
///
/// # Safety
///
/// This traps into M-mode firmware. It is only valid once OpenSBI has handed
/// control to S-mode, i.e. never before `_start` has run.
#[inline(always)]
pub unsafe fn ecall(eid: usize, fid: usize, arg0: usize, arg1: usize) -> (isize, isize) {
    let ret0: usize;
    let ret1: usize;
    core::arch::asm!(
        "ecall",
        inlateout("a0") arg0 => ret0,
        inlateout("a1") arg1 => ret1,
        in("a6") fid,
        in("a7") eid,
        lateout("a2") _,
        lateout("a3") _,
        lateout("a4") _,
        lateout("a5") _,
        options(nostack, preserves_flags),
    );
    // a1 is an SBI extension code (-1 when the extension is absent), a0 is the
    // result or a negative errno.
    (ret0 as isize, ret1 as isize)
}

/// Write one byte to the firmware console.
///
/// Returns `true` when the firmware accepted the call. This is the only output
/// path that works before any device tree parsing, which is why it is the
/// fallback of last resort in [`crate::console`].
///
/// # Safety
///
/// Must only be called after S-mode entry.
#[inline(always)]
pub unsafe fn console_putc(byte: u8) -> bool {
    let (ret, _) = ecall(SBI_SYSCALL, EXT_LEGACY_CONSOLE, byte as usize, FN_LEGACY_PUTCH);
    // SBI_ERR_NO_ERROR is 0; anything else is -EIO / -ENOSYS / ... per spec.
    ret == 0
}

/// Read the SBI specification version, encoded as (major << 24) | minor.
///
/// # Safety
///
/// Must only be called after S-mode entry.
#[inline(always)]
pub unsafe fn spec_version() -> isize {
    ecall(SBI_SYSCALL, FN_GET_SPEC_VERSION, 0, 0).0
}

/// Read the firmware implementation identifier (OpenSBI returns a fixed vendor
/// value; the legacy SBI does not define a stable meaning, so treat it as
/// informational).
///
/// # Safety
///
/// Must only be called after S-mode entry.
#[inline(always)]
pub unsafe fn impl_id() -> isize {
    ecall(SBI_SYSCALL, FN_GET_IMPL_ID, 0, 0).0
}