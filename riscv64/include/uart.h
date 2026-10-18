/*
 * Polled NS16550A UART driver for the QEMU `virt` board.
 *
 * QEMU wires the board's serial line to an emulated 16550 at 0x1000_0000,
 * which is what `-nographic` and `-serial stdio` attach to. The register
 * offsets and bit definitions below are from the PC-compatible UART
 * programming model.
 *
 * This driver is deliberately polling and byte-at-a-time: it runs before any
 * interrupt plumbing exists, and the boot sequence has no other option.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Program the baud divisor and enable the FIFOs, with interrupts off. */
void uart_init(void);

/* Block until the transmit holding register is free, then send one byte. */
void uart_putc(char byte);

#endif /* UART_H */