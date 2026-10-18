/*
 * The S-mode timer.
 *
 * The hart runs on QEMU's `virt` board, whose CLINT timebase is 10 MHz, and
 * the banner reports the `sstc` (Supervisor Timer Compare) ISA extension, so
 * `stimecmp` can be written directly from S-mode: once `time` reaches
 * `stimecmp`, the hart raises a supervisor timer interrupt.
 *
 * Two things have to be true for that interrupt to be delivered, and both are
 * the firmware's business:
 *
 *   - `menvcfg.STCE` must be set, so that S-mode may write `stimecmp`;
 *   - `mideleg.STIP` must be set, so that the supervisor timer interrupt is
 *     delegated to S-mode instead of being handled in M-mode.
 *
 * Both are checked at runtime rather than assumed, and the values are
 * printed, because a silent "the timer never fires" is exactly the kind of
 * failure that is expensive to debug from the outside.
 *
 * The SBI TIME extension is used to read the wall clock for information; the
 * interrupt itself is armed with a direct `stimecmp` write, which needs no
 * ecall and therefore no dependency on the firmware's trap handler.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Ticks of the 10 MHz timebase between two interrupts: 100000 = 10 ms. */
#define TIMER_INTERVAL_TICKS 100000UL

/* How many interrupts the milestone waits for. */
#define TIMER_TICKS_TARGET 5

/*
 * Arm the timer and enable the supervisor timer interrupt. Returns 0 on
 * success, or a negative value if the firmware has not delegated the
 * interrupt to S-mode.
 */
int timer_init(unsigned long interval_ticks);

/* Called from the trap handler on every supervisor timer interrupt. */
void timer_on_tick(void);

/* Number of timer interrupts seen so far. */
unsigned long timer_tick_count(void);

#endif /* TIMER_H */