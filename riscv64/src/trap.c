/*
 * The S-mode trap handler.
 *
 * Called by src/trap.S with a pointer to the interrupted context.  It returns
 * normally for the cases it handles, and the assembly then restores the
 * saved state and executes `sret`.
 *
 * A trap the kernel does not know how to deal with is fatal: there is no
 * process to kill and no other hart to report to, so the only honest thing
 * left is to say what happened and stop.
 */

#include "trap.h"

#include "console.h"
#include "csr.h"
#include "hsm.h"
#include "plic.h"
#include "timer.h"

static void
print_trap_context(const char *what, struct trap_frame *frame, unsigned long code)
{
	console_puts("trap: ");
	console_puts(what);
	console_puts(" cause=");
	console_puthex(frame->scause);
	console_puts(" code=");
	console_puthex(code);
	console_puts(" sepc=");
	console_puthex(frame->sepc);
	console_puts(" stval=");
	console_puthex(frame->stval);
	console_puts("\r\n");
}

unsigned long trap_expected_cause;

/*
 * Stop taking interrupts, then park.
 *
 * Every hart calls the same path; only the boot hart ever reaches it, because
 * a secondary that hits a problem records an error in its slot and keeps
 * running, leaving the reporting to the hart that owns the console.
 */
static void
halt(void)
{
	/* Stop taking interrupts, then park. */
	csr_write_sie(csr_read_sie() & ~(CSR_SIE_STIE | CSR_SIE_SEIE));
	csr_write_sstatus(csr_read_sstatus() & ~CSR_SSTATUS_SIE);

	for (;;) {
		__asm__ volatile ("wfi");
	}
}

void
trap_handler(struct trap_frame *frame, unsigned long hartid)
{
	unsigned long cause = frame->scause;
	unsigned long is_interrupt = cause >> 63;
	unsigned long code = cause & ((1UL << 63) - 1);

	if (is_interrupt) {
		if (code == SUPERVISOR_TIMER_INTERRUPT) {
			timer_on_tick(hartid);
			return;
		}

		if (code == SUPERVISOR_EXTERNAL_INTERRUPT) {
			plic_handle_external(hartid);
			return;
		}

		print_trap_context("unexpected interrupt", frame, code);
		halt();
		return;
	}

	/*
	 * An exception.  The kernel has no way to make progress past one, so
	 * report it and stop; the sepc and stval are what make the report
	 * actionable.
	 *
	 * The one exception is a self test that asked for a specific cause: a
	 * page fault on a read-only page, for instance, is the expected result
	 * and not a failure.
	 */
	if (trap_expected_cause != 0 && code == trap_expected_cause) {
		console_puts("selftest: fault as expected, cause ");
		console_putdec(code);
		console_puts(", sepc ");
		console_puthex(frame->sepc);
		console_puts("\r\n");
		console_puts("selftest: PASS\r\n");
		halt();
	}

	print_trap_context("exception", frame, code);
	halt();
}