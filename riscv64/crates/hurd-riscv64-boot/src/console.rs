/*
 * Console multiplexer.
 *
 * Both sinks are always compiled in, but the order is a compile-time choice so
 * that a future milestone can drop the SBI path (and the ecall cost) without
 * touching the callers.
 *
 * Default policy: talk to the UART when it has been initialised, because QEMU
 * forwards it directly to the terminal and it is roughly an order of magnitude
 * faster than the firmware console. Fall back to the SBI legacy console when
 * the UART is not known to be alive, which is the situation very early in
 * `_start` and inside the panic handler.
 *
 * We cannot "probe" the UART: on real hardware a missing device write is
 * silently dropped, so probing tells us nothing. `UART_READY` is set by
 * [`init`] instead, and the SBI path is the safety net.
 */

use crate::sbi;
use crate::uart::Uart;

/// Set once the UART has been programmed.
static UART_READY: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);

static UART: Uart = Uart::new();

/// Initialise the primary console. Call once, from `kmain`.
pub fn init() {
    UART.init();
    UART_READY.store(true, core::sync::atomic::Ordering::Release);
    write("hurd-riscv64: console up (rv64gc, QEMU virt)\r\n");
}

/// Send one byte to the primary console.
pub fn putc(byte: u8) {
    if byte == b'\n' {
        // The board's terminal is not in ONLCR mode; do the CR ourselves so
        // output lines up in a raw `-serial stdio` capture.
        putc_raw(b'\r');
    }
    putc_raw(byte);
}

/// Send one byte, no newline translation.
fn putc_raw(byte: u8) {
    if UART_READY.load(core::sync::atomic::Ordering::Acquire) {
        UART.putc(byte);
        return;
    }
    // SAFETY: reached only after S-mode entry, i.e. from kmain or the panic
    // handler, both of which run after _start handed control over.
    unsafe {
        sbi::console_putc(byte);
    }
}

/// Send a string to the primary console.
pub fn puts(s: &str) {
    for byte in s.as_bytes() {
        putc(*byte);
    }
}

/// Render a `u64` in hexadecimal, `0x`-prefixed, no allocation.
pub fn puthex(value: u64) {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    puts("0x");
    let mut started = false;
    for shift in (0..16).rev() {
        let nibble = ((value >> (shift * 4)) & 0xf) as usize;
        if nibble != 0 || started || shift == 0 {
            started = true;
            putc(DIGITS[nibble]);
        }
    }
}