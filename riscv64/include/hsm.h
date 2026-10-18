/*
 * Secondary hart bring-up through the SBI HSM extension.
 *
 * The firmware starts one hart and holds the rest at their S-mode entry point,
 * waiting to be released.  `sbi_hart_start()` (HSM, function 0) is how a
 * payload releases them; the kernel uses it to run its own code on every hart
 * of the board.
 *
 * What the boot hart does:
 *
 *   1. probe the HSM extension, and refuse to go on without it;
 *   2. establish what the firmware's status *numbers* mean (see below);
 *   3. ask for the status of every candidate hart id until the firmware says
 *      the id does not exist;
 *   4. give every stopped secondary a 4 KiB stack page from the physical pool;
 *   5. publish the `satp` value of the Sv39 page table the boot hart installed;
 *   6. release the secondaries one at a time, and wait for each of them to
 *      report that it is running.
 *
 * What a secondary does, in `hsm_secondary_main()`:
 *
 *   1. install the same trap vector as the boot hart;
 *   2. arm its own timer tick, by writing `stimecmp` directly;
 *   3. write the published `satp` and `sfence.vma`, so it runs with the MMU
 *      live and on the *same* page table;
 *   4. enable interrupts and count its ticks, recording how many of them
 *      arrived with the MMU active.
 *
 * Two conventions, both forced by how the interface works:
 *
 *   - A secondary never writes to the console.  Only the boot hart prints, so
 *     the log stays readable; a secondary that cannot do its job records an
 *     error code in its slot and the boot hart prints it.
 *   - Nothing reads `mhartid`.  It is a machine-mode CSR: reading it from
 *     S-mode raises an illegal instruction exception (measured on 18/10/2026,
 *     scause 2, stval 0x30a02573).  The hart id arrives in `a0` from the SBI
 *     boot protocol, and from `sbi_hart_start()` for the secondaries, which is
 *     also what `hsm_secondary_main()` receives.
 *
 * The status numbers
 * ------------------
 *
 * `sbi_hart_get_status()` returns a state code, and the specification has not
 * always numbered the states the same way: SBI v0.2 listed STOPPED first and
 * STARTED second, SBI v3.0 lists STARTED first and STOPPED second.  Guessing
 * would either start nothing or start every hart twice.
 *
 * So the encoding is measured.  The boot hart is, by definition, the one hart
 * that is definitely STARTED, so whatever number it reports for itself is the
 * STARTED value of the firmware in use, and the other one is STOPPED.  The
 * raw value is printed, together with which of the two encodings it matches.
 */

#ifndef HSM_H
#define HSM_H

#include <stdint.h>

#include "hsm_offsets.h"

/*
 * The number of hart slots the kernel keeps state for.  It only has to cover
 * the hart count the board is booted with; a hart id at or above this is
 * reported and left alone rather than indexed into the arrays.
 */
#define HSM_MAX_HARTS 8

/* The status codes of the two specification versions that differ. */
#define HSM_STATUS_V02_STOPPED 0UL
#define HSM_STATUS_V02_STARTED 1UL
#define HSM_STATUS_V3_STARTED 0UL
#define HSM_STATUS_V3_STOPPED 1UL

/* Which of the two encodings the firmware in use turned out to follow. */
#define HSM_ENCODE_UNKNOWN 0
#define HSM_ENCODE_SPEC_V02 1
#define HSM_ENCODE_SPEC_V3 2

/* Why a secondary did not come up. Zero means it did. */
#define HSM_ERR_NONE 0
#define HSM_ERR_NO_ROOT_TABLE 1
#define HSM_ERR_NO_STACK 2
#define HSM_ERR_START_FAILED 3

/*
 * How many candidate hart ids to ask about.  The firmware answers with an error
 * once the id is past the last hart, so the list is self-terminating; this bound
 * only keeps a misbehaving firmware from spinning, and it covers every hart
 * count the board is booted with.
 */
#define HSM_PROBE_LIMIT HSM_MAX_HARTS

/* Per-hart state. Indexed by hart id. */
struct hsm_hart {
	unsigned long hartid;
	unsigned long status;  /* raw sbi_hart_get_status() value */
	unsigned long started; /* the firmware was asked to release it */
	unsigned long entered; /* it reached hsm_secondary_main() */
	unsigned long running; /* stvec, satp and interrupts are all live */
	unsigned long error;   /* HSM_ERR_* */
	unsigned long stack_top;
	unsigned long satp; /* what this hart installed */
};

/*
 * One stack top per hart, and zero for the harts that have none.
 *
 * Named here because src/boot.S indexes it: the secondary entry trampoline has
 * to load its stack before it can run a single C instruction.  hsm_offsets.h
 * is the single source of truth for how, and the assertions below check the C
 * array against it.
 */
extern unsigned long hsm_stack_top[HSM_MAX_HARTS];

_Static_assert(sizeof(unsigned long) == (1UL << HSM_ASM_STACK_SHIFT),
	       "hsm_offsets.h: the stack table stride does not match a C unsigned long");
_Static_assert(HSM_ASM_STACK_ENTRY == 0,
	       "hsm_offsets.h: the stack pointer is the entry itself");

/* The per-hart slots. */
extern struct hsm_hart hsm_harts[HSM_MAX_HARTS];


/*
 * Work out what the firmware's status numbers mean, from the status the boot
 * hart reports for itself.  Returns an HSM_ENCODE_* value.
 */
int hsm_learn_status_encoding(unsigned long boot_hartid);

/* True when the raw status code means "started". */
int hsm_status_is_started(unsigned long raw);

/* True when the raw status code means "stopped, waiting to be released". */
int hsm_status_is_stopped(unsigned long raw);

/*
 * Ask the firmware about every candidate hart, give the stopped secondaries a
 * stack, publish the shared page table and release them.  Returns the number of
 * harts released, or a negative value if the HSM extension is unusable.
 */
int hsm_start_secondaries(unsigned long boot_hartid);

/*
 * The secondary entry, called from `secondary_start` in src/boot.S.
 * Diverges: the hart parks in `wfi` when it has nothing left to do.
 */
void hsm_secondary_main(unsigned long hartid);

/*
 * How many secondaries reported `running` before the boot hart's own tick budget
 * ran out.  `expected` is the number that was released: returning as soon as the
 * first one answers would report the rest as missing when they were merely
 * slower.
 */
unsigned long hsm_wait_for_secondaries(unsigned long boot_hartid,
				       unsigned long expected,
				       unsigned long timeout_ticks);

/*
 * Wait until `hartid` has taken `ticks` timer interrupts with the MMU live.
 * Returns 0, or -1 if the boot hart's tick budget ran out first.
 */
int hsm_wait_for_hart_ticks(unsigned long boot_hartid, unsigned long hartid,
			    unsigned long ticks,
			    unsigned long timeout_ticks);

#endif /* HSM_H */