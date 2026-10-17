/*
 * NS16550A UART driver for the QEMU `virt` board.
 *
 * QEMU wires the board's serial line to an emulated 16550 at 0x1000_0000, which
 * is what `-nographic` and `-serial stdio` attach to. The register offsets and
 * bit definitions below are from the PC-compatible UART programming model.
 *
 * This driver is deliberately polling and byte-at-a-time: it runs before any
 * interrupt plumbing exists, and the boot sequence has no other option.
 */

use core::ptr::{read_volatile, write_volatile};

/// Base address of the first 16550A on the QEMU `virt` board.
const UART0_BASE: usize = 0x1000_0000;

/// Transmit Holding Register (write) / Receive Buffer Register (read).
const REG_THR: usize = 0;
/// Divisor Latch Low.
const REG_DLL: usize = 0;
/// Interrupt Enable Register.
const REG_IER: usize = 1;
/// Divisor Latch High.
const REG_DLM: usize = 1;
/// FIFO Control Register.
const REG_FCR: usize = 2;
/// Line Status Register.
const REG_LSR: usize = 5;

/// `LSR` bit 5: transmit holding register empty.
const LSR_THRE: u8 = 1 << 5;

/// The board's clock is 3686400 Hz / (`UART_DIVISOR * 16`) baud; QEMU does not
/// care about the real rate, only that we are not dividing by zero.
const UART_DIVISOR: u16 = 2;

/// A polled 16550A on the `virt` board.
pub struct Uart {
    base: usize,
}

impl Uart {
    /// Build a handle on UART 0 without touching the hardware yet.
    pub const fn new() -> Self {
        Self { base: UART0_BASE }
    }

    /// Read one register.
    ///
    /// # Safety
    ///
    /// `offset` must be a valid 16550A register offset below 8.
    unsafe fn read(&self, offset: usize) -> u8 {
        read_volatile((self.base + offset) as *const u8)
    }

    /// Write one register.
    ///
    /// # Safety
    ///
    /// `offset` must be a valid 16550A register offset below 8.
    unsafe fn write(&self, offset: usize, value: u8) {
        write_volatile((self.base + offset) as *mut u8, value);
    }

    /// Program the baud divisor and enable the FIFOs, with interrupts off.
    ///
    /// Safe to call more than once; the second call is a no-op in practice.
    pub fn init(&self) {
        unsafe {
            self.write(REG_IER, 0x00); // no interrupts yet: no IRQ plumbing
            self.write(REG_LSR, 0x00); // DLAB = 0, select THR
            self.write(REG_DLL, (UART_DIVISOR & 0xff) as u8);
            self.write(REG_DLM, (UART_DIVISOR >> 8) as u8);
            self.write(REG_LSR, 0x80); // LCR_DLAB = 1, select the divisor latches
            self.write(REG_DLL, (UART_DIVISOR & 0xff) as u8);
            self.write(REG_DLM, (UART_DIVISOR >> 8) as u8);
            self.write(REG_LSR, 0x03); // LCR = 8N1
            self.write(REG_FCR, 0xc7); // FIFO on, clear both, 14-byte trigger
            self.write(REG_IER, 0x00);
        }
    }

    /// Block until the transmit holding register is free, then send one byte.
    pub fn putc(&self, byte: u8) {
        unsafe {
            while self.read(REG_LSR) & LSR_THRE == 0 {
                core::hint::spin_loop();
            }
            self.write(REG_THR, byte);
        }
    }

    /// Send a NUL-terminated string.
    pub fn puts(&self, s: &str) {
        for byte in s.as_bytes() {
            if *byte != 0 {
                self.putc(*byte);
            }
        }
    }
}

impl Default for Uart {
    fn default() -> Self {
        Self::new()
    }
}