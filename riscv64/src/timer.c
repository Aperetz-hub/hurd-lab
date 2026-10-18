/*
 * The S-mode timer, one set of state per hart.  See include/timer.h for the
 * rationale behind the mechanism, and include/hsm.h for why the state is
 * per-hart.
 */

#include "timer.h"

#include "console.h"
#include "csr.h"
#include "sbi.h"

/*
 * Everything the handler needs, per hart.  Indexed by hart id, which is cached
 * once at entry: `mhartid` cannot be read from S-mode.
 */
struct timer_state {
	unsigned long interval;
	unsigned long next_deadline;
	unsigned long last_tick_time;
	unsigned long first_interrupt_time;
	unsigned long tick_count;
	unsigned long tick_count_mmu;
	unsigned long sbi_error;
	int verbose;
	int armed;
	const char *armed_by;
};

static struct timer_state timers[HSM_MAX_HARTS];

unsigned long
timer_tick_count(unsigned long hartid)
{
	if (hartid >= HSM_MAX_HARTS)
		return 0;
	return timers[hartid].tick_count;
}

unsigned long
timer_tick_count_mmu(unsigned long hartid)
{
	if (hartid >= HSM_MAX_HARTS)
		return 0;
	return timers[hartid].tick_count_mmu;
}

int
timer_arm(unsigned long hartid, unsigned long interval_ticks, int how)
{
	struct timer_state *t;
	unsigned long now;
	struct sbiret r;

	if (hartid >= HSM_MAX_HARTS)
		return -1;

	t = &timers[hartid];
	t->interval = interval_ticks;
	t->verbose = (how == TIMER_ARM_BOOT);
	t->tick_count = 0;
	t->tick_count_mmu = 0;
	t->last_tick_time = 0;
	t->first_interrupt_time = 0;
	t->armed = 0;
	t->armed_by = "none";

	/*
	 * Arm the first deadline.
	 *
	 * The boot hart asks the SBI TIME extension first
	 * (sbi_timer_set_stimecmp, FID #7), so the banner records which
	 * mechanism the firmware offers; if that call fails it falls back to
	 * writing the Sstc CSR directly.
	 *
	 * A secondary hart released through the SBI HSM extension skips the
	 * ecall entirely.  Two reasons: the rearm happens in interrupt context,
	 * where an ecall into the firmware is the wrong thing to do, and a hart
	 * that has only just been released should not depend on the firmware
	 * still servicing calls from it.  The CSR write is the native mechanism
	 * on a hart that advertises the Sstc extension, and it is what the
	 * rearm path uses anyway.
	 *
	 * Whether the firmware delegated the supervisor timer interrupt to
	 * S-mode cannot be read from S-mode (mideleg is a machine-mode CSR), so
	 * it is established by the effect: the tick either arrives or it does
	 * not.
	 */
	now = csr_read_time();
	t->next_deadline = now + interval_ticks;

	if (how == TIMER_ARM_SECONDARY) {
		csr_write_stimecmp(t->next_deadline);
		t->armed_by = "stimecmp CSR";
		t->armed = 1;
		csr_enable_interrupts();
		return 0;
	}

	r = sbi_timer_set_stimecmp(t->next_deadline);
	t->sbi_error = r.error;
	if (r.error == 0) {
		t->armed_by = "SBI timer extension";
	} else {
		csr_write_stimecmp(now + interval_ticks);
		t->armed_by = "stimecmp CSR";
	}
	t->armed = 1;

	csr_enable_interrupts();
	return 0;
}

void
timer_set_verbose(unsigned long hartid, int verbose)
{
	if (hartid >= HSM_MAX_HARTS)
		return;

	timers[hartid].verbose = verbose;
}

void
timer_report(unsigned long hartid)
{
	struct timer_state *t;

	if (hartid >= HSM_MAX_HARTS)
		return;

	t = &timers[hartid];
	console_puts("timer armed by ");
	console_puts(t->armed_by);
	console_puts(" at ");
	console_putdec(t->next_deadline);
	console_puts(" (SBI sbi_timer_set_stimecmp error ");
	console_putdec_signed((int64_t)t->sbi_error);
	console_puts(")\r\n");
}

void
timer_on_tick(unsigned long hartid)
{
	struct timer_state *t;
	unsigned long now;

	if (hartid >= HSM_MAX_HARTS)
		return;

	t = &timers[hartid];
	now = csr_read_time();

	t->tick_count++;
	if (t->tick_count == 1)
		t->first_interrupt_time = now;

	/*
	 * Record whether this tick was taken with the MMU live.  A hart that
	 * armed its interrupt but never installed a page table would otherwise
	 * look exactly like one that did.
	 */
	if ((csr_read_satp() >> 60) == 8)
		t->tick_count_mmu++;

	/*
	 * Rearm without drift.
	 *
	 * Scheduling "now + interval" would make every tick late by the time
	 * the handler takes to run -- the UART writes and the rearm itself --
	 * so the measured period would be the interval plus that latency, and
	 * the error would accumulate.  Instead the deadline is advanced by
	 * exactly one interval, so the period is the interval whatever the
	 * handler costs.
	 *
	 * The only exception is when the handler took so long that the next
	 * deadline is already in the past; then the schedule is resynchronised
	 * on the current time instead of firing a burst of catch-up ticks.
	 */
	if (t->next_deadline + t->interval <= now)
		t->next_deadline = now + t->interval;
	else
		t->next_deadline += t->interval;

	csr_write_stimecmp(t->next_deadline);
	t->last_tick_time = now;

	if (!t->verbose)
		return;

	console_puts("timer tick ");
	console_putdec(t->tick_count);
	console_puts(" at ");
	console_putdec(now);
	if (t->tick_count > 1) {
		/*
		 * The deadline schedule advances by exactly one interval per
		 * tick, so it cannot drift.  The *observed* times still differ
		 * from the deadlines by the handler latency, which varies; the
		 * average period over the ticks so far is the number that shows
		 * whether the error accumulates or stays bounded.
		 */
		console_puts(" (period ");
		console_putdec((now - t->first_interrupt_time) / (t->tick_count - 1));
		console_puts(" ticks, deadline +");
		console_putdec(t->next_deadline - (t->first_interrupt_time + t->interval));
		console_puts(")");
	}
	console_puts("\r\n");
}