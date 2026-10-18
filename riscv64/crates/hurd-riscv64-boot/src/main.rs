/*
 * The kernel proper.
 *
 * `kmain` is called from the `_start` trampoline in `boot.rs`, in S-mode, with
 * the stack pointing at `__stack_top` and `.bss` already zeroed.
 *
 * Milestone 1 does exactly three things, in this order, so that every failure
 * mode stays observable:
 *   1. bring up the console;
 *   2. prove the SBI ecall path works (and that the firmware console is alive);
 *   3. read the SBI specification version, which is the cheapest way to learn
 *      *which* firmware and *which* spec revision we are running under.
 *
 * See README.md for what comes next.
 */

#![no_std]
#![no_main]

use core::panic::PanicInfo;

mod boot;
mod console;
mod sbi;
mod uart;

/// Render a decimal number without `alloc` (no `ToString` in `core`).
fn putdec(mut value: u64) {
    let mut digits = [0u8; 20];
    let mut n = 0;
    if value == 0 {
        console::putc(b'0');
        return;
    }
    while value > 0 {
        digits[n] = b'0' + (value % 10) as u8;
        value /= 10;
        n += 1;
    }
    while n > 0 {
        n -= 1;
        console::putc(digits[n]);
    }
}

/// Kernel entry point, called from `boot.rs`.
///
/// Diverges: there is nothing to return to.
#[no_mangle]
pub extern "C" fn kmain() -> ! {
    // `_start` reaches us by `call kmain`, which leaves `a0` untouched, and
    // `a0` is the hart id the SBI boot protocol passes at S-mode entry (see
    // boot.rs). It has to be captured *first*: any function call, including
    // `console::init()` below, is free to clobber `a0`.
    let hartid: u64;
    // SAFETY: `mv` into a fresh local register; no memory, flags or stack effect.
    unsafe {
        core::arch::asm!("mv {}, a0", out(reg) hartid, options(nomem, nostack, preserves_flags));
    }

    console::init();

    // Printing it is how "a0 is the hart id" is checked rather than assumed.
    console::puts("SBI hart id ");
    console::puthex(hartid);
    console::puts("\r\n");

    // SBI legacy console, bypassing the UART, to prove the firmware path is
    // alive. Harmless if the firmware dropped the extension: SBI returns a
    // negative errno and the character simply never appears.
    // SAFETY: S-mode entry already happened.
    let putch_ok = unsafe { sbi::console_putc(b'!') };
    console::puts("\r\nSBI legacy_putc works: ");
    console::puts(if putch_ok { "yes" } else { "no (extension absent)" });
    console::puts("\r\n");

    // Identify the firmware we are running under.
    // SAFETY: S-mode entry already happened.
    let version = unsafe { sbi::spec_version() } as u64;
    console::puts("SBI spec version ");
    putdec((version >> 24) & 0xff);
    console::putc(b'.');
    putdec(version & 0xff_ffff);
    console::puts("\r\n");

    // SAFETY: S-mode entry already happened.
    console::puts("SBI impl id ");
    console::puthex(unsafe { sbi::impl_id() } as u64);
    console::puts("\r\n");

    console::puts("hurd-riscv64: milestone 1 reached, halting\r\n");

    // There is no scheduler yet. Park the hart; wfi keeps it off the host CPU.
    loop {
        // SAFETY: `wfi` is unprivileged in S-mode.
        unsafe { core::arch::asm!("wfi") };
    }
}

/// Panic handler: the only diagnostics available before a real console exists.
#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    console::puts("\r\n*** panic: ");
    console::puts(info.message().as_str().unwrap_or("<no message>"));
    if let Some(loc) = info.location() {
        console::puts(" at ");
        console::puts(loc.file());
        console::putc(b':');
        putdec(loc.line() as u64);
    }
    console::puts(" ***\r\n");
    loop {
        // SAFETY: `wfi` is unprivileged in S-mode.
        unsafe { core::arch::asm!("wfi") };
    }
}