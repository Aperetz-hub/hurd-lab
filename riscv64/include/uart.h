/*
 * NS16550A UART driver for the QEMU `virt` board.
 *
 * QEMU wires the board's serial line to an emulated 16550 at 0x1000_0000,
 * which is what `-nographic` and `-serial stdio` attach to. The register
 * offsets and bit definitions below are from the PC-compatible UART
 * programming model.
 *
 * The console path is polled and byte-at-a-time: it runs before any interrupt
 * plumbing exists, and the boot sequence has no other option.  The receive
 * half is different -- it is there for the PLIC, which needs the device to
 * assert its interrupt line when a byte lands in the receive register.  Note
 * that a byte can only arrive from the host: the guest writes to this device,
 * it never reads from it, so the self test has the host inject one over the
 * serial socket.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Program the baud divisor and enable the FIFOs, with interrupts off. */
void uart_init(void);

/* Block until the transmit holding register is free, then send one byte. */
void uart_putc(char byte);

/* Enable the "received data available" interrupt in IER, which is what makes the
 * device assert its PLIC line. */
void uart_enable_rx_irq(void);

/* Turn it back off, and clear anything already pending in the receive register. */
void uart_disable_rx_irq(void);

/* True while the receive register holds a byte (LSR.DR). */
int uart_rx_ready(void);

/* Read the byte out of the receive register. Reading it is what drops the
 * device's interrupt line. */
unsigned char uart_read_rbr(void);

#endif /* UART_H */