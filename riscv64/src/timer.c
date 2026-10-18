/*
 * The S-mode timer. See include/timer.h for the rationale.
 */

#include "timer.h"

#include "console.h"
#include "csr.h"
#include "sbi.h"

static unsigned long tick_count;
static unsigned long interval;
static unsigned long first_tick_time;
static unsigned long last_tick_time;
static unsigned long next_deadline;
/* The time of the first interrupt, used to measure the average period. */
static unsigned long first_interrupt_time;

static const char *armed_by;

unsigned long
timer_tick_count(void)
{
	return tick_count;
}

int
timer_init(unsigned long interval_ticks)
{
	unsigned long now;
	struct sbiret r;

	interval = interval_ticks;

	/*
	 * Arm the first deadline.  The SBI TIME extension is asked first
	 * (sbi_timer_set_stimecmp, FID #7); if the firmware does not implement
	 * that function, the Sstc CSR is written directly, which is the native
	 * mechanism on a hart that advertises the extension.
	 *
	 * Whether the firmware delegated the supervisor timer interrupt to
	 * S-mode cannot be read from S-mode (mideleg is a machine-mode CSR), so
	 * it is established by the effect: the tick either arrives or it does
	 * not.
	 */
	now = csr_read_time();
	first_tick_time = now;
	next_deadline = now + interval;

	r = sbi_timer_set_stimecmp(next_deadline);
	if (r.error == 0) {
		armed_by = "SBI timer extension";
	} else {
		csr_write_stimecmp(now + interval);
		armed_by = "stimecmp CSR";
	}
	console_puts("timer armed by ");
	console_puts(armed_by);
	console_puts(" at ");
	console_putdec(next_deadline);
	console_puts(" (SBI sbi_timer_set_stimecmp error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");

	csr_enable_interrupts();
	return 0;
}

void
timer_on_tick(void)
{
	unsigned long now = csr_read_time();
	/* Measured before last_tick_time is updated below. */
	unsigned long delta = now - last_tick_time;

	tick_count++;
	if (tick_count == 1)
		first_interrupt_time = now;

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
	if (next_deadline + interval <= now)
		next_deadline = now + interval;
	else
		next_deadline += interval;

	csr_write_stimecmp(next_deadline);
	last_tick_time = now;

	console_puts("timer tick ");
	console_putdec(tick_count);
	console_puts(" at ");
	console_putdec(now);
	if (tick_count > 1) {
		/*
		 * The deadline schedule advances by exactly one interval per
		 * tick, so it cannot drift.  The *observed* times still differ
		 * from the deadlines by the handler latency, which varies; the
		 * average period over the ticks so far is the number that shows
		 * whether the error accumulates or stays bounded.
		 */
		console_puts(" (period ");
		console_putdec((now - first_interrupt_time) / (tick_count - 1));
		console_puts(" ticks, deadline +");
		console_putdec(next_deadline - (first_interrupt_time + interval));
		console_puts(")");
	}
	console_puts("\r\n");
}
