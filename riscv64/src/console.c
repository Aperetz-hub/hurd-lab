/*
 * Console multiplexer. See include/console.h for the policy.
 */

#include "console.h"

#include "sbi.h"
#include "uart.h"

static int uart_ready;

void
console_init(void)
{
	uart_init();
	uart_ready = 1;
	console_puts("hurd-riscv64: console up (rv64gc, QEMU virt)\r\n");
}

void
console_putc(char byte)
{
	if (byte == '\n') {
		/*
		 * The board's terminal is not in ONLCR mode; do the CR ourselves
		 * so output lines up in a raw `-serial stdio` capture.
		 */
		if (uart_ready)
			uart_putc('\r');
		else
			(void)sbi_legacy_console_putc('\r');
	}

	if (uart_ready)
		uart_putc(byte);
	else
		(void)sbi_legacy_console_putc(byte);
}

void
console_puts(const char *s)
{
	while (*s != '\0')
		console_putc(*s++);
}

void
console_putdec(uint64_t value)
{
	char digits[20];
	int n = 0;

	if (value == 0) {
		console_putc('0');
		return;
	}

	while (value > 0) {
		digits[n++] = (char)('0' + (value % 10));
		value /= 10;
	}

	while (n > 0)
		console_putc(digits[--n]);
}

void
console_putdec_signed(int64_t value)
{
	if (value < 0) {
		console_putc('-');
		/* Negate in unsigned space so that INT64_MIN does not overflow. */
		console_putdec((uint64_t)0 - (uint64_t)value);
		return;
	}
	console_putdec((uint64_t)value);
}

void
console_puthex(uint64_t value)
{
	static const char hexdigits[16] = "0123456789abcdef";
	int started = 0;
	int shift;

	console_puts("0x");

	for (shift = 60; shift >= 0; shift -= 4) {
		unsigned int nibble = (unsigned int)((value >> shift) & 0xf);

		if (nibble != 0 || started || shift == 0) {
			started = 1;
			console_putc(hexdigits[nibble]);
		}
	}
}