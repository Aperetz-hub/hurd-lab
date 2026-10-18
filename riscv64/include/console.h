/*
 * Console multiplexer: one API, two sinks.
 *
 * Both sinks are always compiled in, but the choice between them is a
 * compile-time decision so that a future milestone can drop the SBI path
 * (and the ecall cost) without touching the callers.
 *
 * Default policy: talk to the UART once it has been initialised, because QEMU
 * forwards it directly to the terminal and it is roughly an order of magnitude
 * faster than the firmware console. Fall back to the SBI legacy console when
 * the UART is not known to be alive, which is the situation very early in
 * `_start` and inside any diagnostic path.
 *
 * We cannot "probe" the UART: on real hardware a write to a missing device is
 * silently dropped, so probing tells us nothing. `uart_ready` is set by
 * `console_init()`, and the SBI path is the safety net.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

/* Initialise the primary console. Call once, from kmain. */
void console_init(void);

/* Send one byte to the primary console ('\n' is translated to CR LF). */
void console_putc(char byte);

/* Send a NUL-terminated string. */
void console_puts(const char *s);

/* Render a 64-bit value in decimal, no allocation. */
void console_putdec(uint64_t value);

/* Render a 64-bit value in signed decimal, no allocation. */
void console_putdec_signed(int64_t value);

/* Render a 64-bit value in hexadecimal, 0x-prefixed, no leading zeros. */
void console_puthex(uint64_t value);

#endif /* CONSOLE_H */