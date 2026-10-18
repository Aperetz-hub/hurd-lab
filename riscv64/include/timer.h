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

#include "hsm.h"

/* Ticks of the 10 MHz timebase between two interrupts: 100000 = 10 ms. */
#define TIMER_INTERVAL_TICKS 100000UL

/* How many interrupts the milestone waits for. */
#define TIMER_TICKS_TARGET 5

/* How much ceremony a hart's timer needs. */
#define TIMER_ARM_BOOT 0	/* the boot hart: ask the SBI TIME extension
				 * first, so the banner records what the firmware
				 * offers, then report and print every tick */
#define TIMER_ARM_SECONDARY 1	/* a hart the kernel released itself: write
				 * the Sstc CSR directly and say nothing, so two
				 * harts never interleave on the console */

/*
 * Arm this hart's timer and enable the supervisor timer interrupt.
 *
 * `hartid` selects the per-hart slot: every hart runs its own copy of this
 * state, and a secondary hart started through the SBI HSM extension arms its
 * own tick exactly the way the boot hart does.
 *
 * Returns 0 on success, or a negative value if the firmware has not delegated
 * the interrupt to S-mode.
 */
int timer_arm(unsigned long hartid, unsigned long interval_ticks, int how);

/* Report how the timer was armed, for the console. The boot hart calls this. */
void timer_report(unsigned long hartid);

/*
 * Turn the per-tick console report on or off for this hart.
 *
 * The report is what makes a boot log readable, and what makes a test that waits
 * through hundreds of idle ticks unreadable.  The PLIC self test turns it off for
 * the duration of its searches and back on afterwards.
 */
void timer_set_verbose(unsigned long hartid, int verbose);

/* Called from the trap handler on every supervisor timer interrupt. */
void timer_on_tick(unsigned long hartid);

/* Number of timer interrupts this hart has taken. */
unsigned long timer_tick_count(unsigned long hartid);

/*
 * How many of those arrived with the MMU live.  A hart that armed its tick and
 * enabled its interrupts but never actually switched its `satp` on would
 * otherwise report the same tick count as one that did.
 */
unsigned long timer_tick_count_mmu(unsigned long hartid);

#endif /* TIMER_H */