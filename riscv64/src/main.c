/*
 * The kernel proper.
 *
 * `kmain` is called from the `_start` trampoline in `boot.S`, in S-mode, with
 * the stack pointing at `__stack_top` and `.bss` already zeroed. It receives
 * the hart id that the SBI boot protocol passed in `a0`, because `a0` is the
 * first argument register and `boot.S` calls us without touching it.
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

#include "console.h"
#include "sbi.h"

/* Declared here as well because boot.S calls it by name. */
void kmain(unsigned long hartid);

void
kmain(unsigned long hartid)
{
	long version;
	int putch_ok;

	console_init();

	/*
	 * Report the hart id the firmware handed us. Printing it is how "a0 is
	 * the hart id" gets checked rather than assumed.
	 */
	console_puts("SBI hart id ");
	console_puthex((uint64_t)hartid);
	console_puts("\r\n");

	/*
	 * SBI legacy console, bypassing the UART, to prove the firmware path is
	 * alive. Harmless if the firmware dropped the extension: SBI returns a
	 * negative errno and the character simply never appears.
	 */
	putch_ok = sbi_console_putc('!');
	console_puts("\r\nSBI legacy_putc works: ");
	console_puts(putch_ok ? "yes" : "no (extension absent)");
	console_puts("\r\n");

	/* Identify the firmware we are running under. */
	version = sbi_spec_version();
	console_puts("SBI spec version ");
	console_putdec((uint64_t)(version >> 24) & 0xff);
	console_putc('.');
	console_putdec((uint64_t)version & 0xffffff);
	console_puts("\r\n");

	console_puts("SBI impl id ");
	console_puthex((uint64_t)sbi_impl_id());
	console_puts("\r\n");

	console_puts("hurd-riscv64: milestone 1 reached, halting\r\n");

	/* There is no scheduler yet. Park the hart; wfi keeps it off the host CPU. */
	for (;;) {
		__asm__ volatile ("wfi");
	}
}