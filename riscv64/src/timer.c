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

	r = sbi_timer_set_stimecmp(now + interval);
	if (r.error == 0) {
		armed_by = "SBI timer extension";
	} else {
		csr_write_stimecmp(now + interval);
		armed_by = "stimecmp CSR";
	}
	console_puts("timer armed by ");
	console_puts(armed_by);
	console_puts(" at ");
	console_putdec(now + interval);
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

	/* Rearm for the next one. */
	csr_write_stimecmp(now + interval);
	last_tick_time = now;

	console_puts("timer tick ");
	console_putdec(tick_count);
	console_puts(" at ");
	console_putdec(now);
	if (tick_count == 2) {
		console_puts(" (interval ");
		console_putdec(delta);
		console_puts(" ticks)");
	}
	console_puts("\r\n");
}
