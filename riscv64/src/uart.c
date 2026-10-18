/*
 * Polled NS16550A UART driver for the QEMU `virt` board.
 *
 * See include/uart.h for the rationale. Register offsets are from the
 * PC-compatible UART programming model:
 *
 *   0  RBR (read) / THR (write), or DLL when LCR.DLAB is set
 *   1  IER,                 or DLM when LCR.DLAB is set
 *   2  IIR (read) / FCR (write)
 *   3  LCR  (line control, read/write)
 *   5  LSR  (line status, read-only)
 *
 * NOTE, deliberate difference from the previous implementation of this driver:
 * the divisor latch is now programmed by setting LCR.DLAB (offset 3) and then
 * writing DLL/DLM, and LCR is used for what it is.  The earlier code wrote the
 * divisor through the constant it named REG_LSR, i.e. offset 5, which is the
 * read-only Line Status Register, so DLAB was never set and the divisor never
 * reached the latch registers.  That was harmless under QEMU, whose emulated
 * 16550 and OpenSBI already leave the line in 8N1, and would not be on real
 * hardware.
 */

#include "uart.h"

/* Base address of the first 16550A on the QEMU `virt` board. */
#define UART0_BASE 0x10000000UL

#define REG_RBR 0 /* also THR, and DLL when DLAB is set */
#define REG_IER 1 /* also DLM when DLAB is set */
#define REG_FCR 2
#define REG_LCR 3
#define REG_LSR 5

/* LSR bit 5: transmit holding register empty. */
#define LSR_THRE (1u << 5)

/* LCR bit 7: divisor latch access. */
#define LCR_DLAB (1u << 7)
/* LCR bits 1..0 = 3: 8 data bits, one stop bit, no parity (8N1). */
#define LCR_8N1 0x03u

/*
 * The board's clock is 3686400 Hz / (UART_DIVISOR * 16) baud; QEMU does not
 * care about the real rate, only that we do not divide by zero.
 */
#define UART_DIVISOR 2

/* FIFO on, clear both FIFOs, 14-byte receive trigger. */
#define FCR_ENABLE_CLEAR 0xc7

static inline volatile uint8_t *
uart_reg(unsigned int offset)
{
	return (volatile uint8_t *)(UART0_BASE + offset);
}

void
uart_init(void)
{
	/* Select the divisor latches before touching DLL/DLM. */
	*uart_reg(REG_LCR) = LCR_DLAB;
	*uart_reg(REG_RBR) = (uint8_t)(UART_DIVISOR & 0xff);
	*uart_reg(REG_IER) = (uint8_t)(UART_DIVISOR >> 8);

	/* Back to normal operation, 8N1, interrupts still off. */
	*uart_reg(REG_LCR) = LCR_8N1;
	*uart_reg(REG_FCR) = FCR_ENABLE_CLEAR;
	*uart_reg(REG_IER) = 0x00;
}

void
uart_putc(char byte)
{
	while ((*uart_reg(REG_LSR) & LSR_THRE) == 0) {
		/* Poll until the holding register drains. */
	}
	*uart_reg(REG_RBR) = (uint8_t)byte;
}