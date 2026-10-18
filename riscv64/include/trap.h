/*
 * The S-mode trap frame and the C handler interface.
 */

#ifndef TRAP_H
#define TRAP_H

#include <stddef.h>
#include <stdint.h>

#include "trap_offsets.h"

/*
 * The state of the interrupted context, as saved by src/trap.S.
 *
 * The member order matches the offsets in trap_offsets.h exactly; the static
 * assertions below turn any drift into a compile error.
 */
struct trap_frame {
	uint64_t ra;    /* x1  */
	uint64_t gp;    /* x3  */
	uint64_t t0;    /* x5  */
	uint64_t t1;    /* x6  */
	uint64_t t2;    /* x7  */
	uint64_t s0;    /* x8  */
	uint64_t s1;    /* x9  */
	uint64_t a0;    /* x10 */
	uint64_t a1;    /* x11 */
	uint64_t a2;    /* x12 */
	uint64_t a3;    /* x13 */
	uint64_t a4;    /* x14 */
	uint64_t a5;    /* x15 */
	uint64_t a6;    /* x16 */
	uint64_t a7;    /* x17 */
	uint64_t s2;    /* x18 */
	uint64_t s3;    /* x19 */
	uint64_t s4;    /* x20 */
	uint64_t s5;    /* x21 */
	uint64_t s6;    /* x22 */
	uint64_t s7;    /* x23 */
	uint64_t s8;    /* x24 */
	uint64_t s9;    /* x25 */
	uint64_t s10;   /* x26 */
	uint64_t s11;   /* x27 */
	uint64_t s2b;   /* x28 */
	uint64_t t3;    /* x29 */
	uint64_t t4;    /* x30 */
	uint64_t t5;    /* x31 */
	uint64_t sepc;     /* PC of the interrupted instruction */
	uint64_t sstatus;  /* status register at the trap */
	uint64_t scause;   /* cause of the trap */
	uint64_t stval;    /* faulting address / instruction, when defined */
	uint64_t orig_sp;  /* stack pointer of the interrupted context */
	uint64_t orig_tp;  /* thread pointer of the interrupted context */
	uint64_t pad;      /* keeps the frame a multiple of 16 bytes */
};

#define TRAP_CHECK(member, offset) \
	_Static_assert(offsetof(struct trap_frame, member) == (offset), \
		       "trap frame layout does not match trap_offsets.h: " #member)

TRAP_CHECK(ra, TRAP_OFF_RA);
TRAP_CHECK(gp, TRAP_OFF_GP);
TRAP_CHECK(t0, TRAP_OFF_T0);
TRAP_CHECK(t1, TRAP_OFF_T1);
TRAP_CHECK(t2, TRAP_OFF_T2);
TRAP_CHECK(s0, TRAP_OFF_S0);
TRAP_CHECK(s1, TRAP_OFF_S1);
TRAP_CHECK(a0, TRAP_OFF_A0);
TRAP_CHECK(a1, TRAP_OFF_A1);
TRAP_CHECK(a2, TRAP_OFF_A2);
TRAP_CHECK(a3, TRAP_OFF_A3);
TRAP_CHECK(a4, TRAP_OFF_A4);
TRAP_CHECK(a5, TRAP_OFF_A5);
TRAP_CHECK(a6, TRAP_OFF_A6);
TRAP_CHECK(a7, TRAP_OFF_A7);
TRAP_CHECK(s2, TRAP_OFF_S2);
TRAP_CHECK(s3, TRAP_OFF_S3);
TRAP_CHECK(s4, TRAP_OFF_S4);
TRAP_CHECK(s5, TRAP_OFF_S5);
TRAP_CHECK(s6, TRAP_OFF_S6);
TRAP_CHECK(s7, TRAP_OFF_S7);
TRAP_CHECK(s8, TRAP_OFF_S8);
TRAP_CHECK(s9, TRAP_OFF_S9);
TRAP_CHECK(s10, TRAP_OFF_S10);
TRAP_CHECK(s11, TRAP_OFF_S11);
TRAP_CHECK(s2b, TRAP_OFF_S2B);
TRAP_CHECK(t3, TRAP_OFF_T3);
TRAP_CHECK(t4, TRAP_OFF_T4);
TRAP_CHECK(t5, TRAP_OFF_T5);
TRAP_CHECK(sepc, TRAP_OFF_SEPC);
TRAP_CHECK(sstatus, TRAP_OFF_SSTATUS);
TRAP_CHECK(scause, TRAP_OFF_SCAUSE);
TRAP_CHECK(stval, TRAP_OFF_STVAL);
TRAP_CHECK(orig_sp, TRAP_OFF_ORIGSP);
TRAP_CHECK(orig_tp, TRAP_OFF_ORIGTP);

TRAP_CHECK(pad, TRAP_OFF_PAD);

_Static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE,
	       "trap frame size does not match TRAP_FRAME_SIZE");

/*
 * The trap entry, defined in src/trap.S. Declared as an object rather than a
 * function because it is an assembly label; only its address is used.
 */
extern char trap_entry[];

/*
 * Called by the assembly entry with a pointer to the interrupted context.
 * It must not return normally for an exception it cannot handle; the
 * assembly restores the saved state and executes `sret` when it does.
 */
void trap_handler(struct trap_frame *frame);

/*
 * When non-zero, an exception whose cause code equals this value is treated
 * as the expected outcome of a self test: it is reported and the kernel
 * stops, instead of being reported as an unexpected fault.  Zero means "no
 * exception is expected".
 */
extern unsigned long trap_expected_cause;

#endif /* TRAP_H */