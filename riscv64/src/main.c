/*
 * The kernel proper.
 *
 * `kmain` is called from the `_start` trampoline in `boot.S`, in S-mode, with
 * the stack pointing at `__stack_top` and `.bss` already zeroed. It receives
 * the hart id that the SBI boot protocol passed in `a0`, because `a0` is the
 * first argument register and `boot.S` calls us without touching it.
 *
 * Milestone 2 does, in this order:
 *   1. bring up the console;
 *   2. exercise the SBI base extension and print what the firmware says
 *      about itself (spec version, implementation id and version, and which
 *      extensions are available);
 *   3. install the S-mode trap vector;
 *   4. arm the S-mode timer and wait for five supervisor timer interrupts;
 *   5. report milestone 2 and park.
 *
 * Every step prints what it observed, so that a failure says where it
 * happened rather than only that it happened.
 *
 * See README.md for what comes next.
 */

#include "console.h"
#include "csr.h"
#include "sbi.h"
#include "timer.h"
#include "trap.h"

/* Declared here as well because boot.S calls it by name. */
void kmain(unsigned long hartid);

/* The SBI specification version, as major.minor. */
static void
print_spec_version(struct sbiret r)
{
	unsigned long v;

	/*
	 * The SBI base extension returns the pair (a0 = error, a1 = value).
	 * A firmware that answers in a0 instead is not conformant, so the value
	 * is taken from a1 and the error code is printed alongside it.
	 */
	v = (unsigned long)r.value;

	console_puts("SBI spec version ");
	console_putdec((v >> 24) & 0x7f);
	console_putc('.');
	console_putdec(v & 0xffffff);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(", value ");
	console_puthex(v);
	console_puts(")\r\n");
}

static void
print_impl_id(const char *what, struct sbiret r)
{
	console_puts(what);
	console_puts(" ");
	console_puthex((unsigned long)r.value);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");
}

static void
print_probe(const char *what, unsigned long eid)
{
	struct sbiret r = sbi_probe_extension(eid);

	console_puts(what);
	console_puts(" eid ");
	console_puthex(eid);
	console_puts(" -> ");
	console_putdec((unsigned long)r.value);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");
}

void
kmain(unsigned long hartid)
{
	struct sbiret r;

	console_init();

	/*
	 * Report the hart id the firmware handed us. Printing it is how "a0 is
	 * the hart id" gets checked rather than assumed.
	 */
	console_puts("SBI hart id ");
	console_puthex((uint64_t)hartid);
	console_puts("\r\n");

	/*
	 * The legacy console extension, to prove the firmware path is alive.
	 * The legacy extensions return their result in a0, not in the a0/a1
	 * pair, so this call does not go through sbi_ecall().
	 */
	if (sbi_legacy_console_putc('!'))
		console_puts("\r\nSBI legacy_putc works: yes\r\n");
	else
		console_puts("\r\nSBI legacy_putc works: no (extension absent)\r\n");

	/* Identify the firmware we are running under. */
	print_spec_version(sbi_get_spec_version());
	print_impl_id("SBI impl id", sbi_get_impl_id());
	print_impl_id("SBI impl version", sbi_get_impl_version());

	/* Which extensions are actually there. */
	print_probe("SBI extension probe base", SBI_EID_BASE);
	print_probe("SBI extension probe legacy console", SBI_EID_LEGACY_CONSOLE);
	print_probe("SBI extension probe time", SBI_EID_TIME);

	/* The SBI TIME extension reads the wall clock; the timebase tells us
	 * how fast it runs. */
	r = sbi_timer_time();
	console_puts("SBI wall clock ");
	console_putdec((unsigned long)r.value);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");

	r = sbi_timer_timebase();
	console_puts("SBI timebase ");
	console_putdec((unsigned long)r.value);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");

	console_puts("hurd-riscv64: milestone 1 reached\r\n");

	/*
	 * Install the S-mode trap vector. The ecall from S-mode is not
	 * delegated to S-mode by the firmware, so this does not disturb the SBI
	 * path: the ecall traps straight to M-mode.
	 */
	console_puts("firmware stvec was ");
	console_puthex(csr_read_stvec());
	console_puts("\r\n");
	csr_write_stvec((unsigned long)trap_entry);
	console_puts("installed stvec ");
	console_puthex(csr_read_stvec());
	console_puts("\r\n");

#ifdef SELFTEST_ILLEGAL_INSTRUCTION
	/*
	 * Self-test: raise a deliberate illegal instruction and check that the
	 * trap handler catches it and reports the context.  The handler treats
	 * an exception as fatal, so the kernel stops here on purpose; the point
	 * is the report that precedes it.
	 */
	console_puts("self-test: raising an illegal instruction\r\n");
	__asm__ volatile (".word 0x00000000");
#endif

	/* Arm the timer and wait for the interrupts to arrive. */
	if (timer_init(TIMER_INTERVAL_TICKS) != 0) {
		console_puts("hurd-riscv64: milestone 2 NOT reached, no timer\r\n");
		for (;;) {
			__asm__ volatile ("wfi");
		}
	}

	while (timer_tick_count() < TIMER_TICKS_TARGET) {
		__asm__ volatile ("wfi");
	}

	console_puts("hurd-riscv64: milestone 2 reached, halting\r\n");

	/* Park the hart; the timer is disarmed by the handler's halt path. */
	csr_write_sie(csr_read_sie() & ~CSR_SIE_STIE);
	csr_write_sstatus(csr_read_sstatus() & ~CSR_SSTATUS_SIE);
	for (;;) {
		__asm__ volatile ("wfi");
	}
}