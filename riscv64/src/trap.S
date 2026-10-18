/*
 * The S-mode trap entry.
 *
 * Installed in `stvec` in direct mode.  It saves the interrupted context in a
 * frame on the interrupted stack, calls the C handler with a pointer to that
 * frame, restores everything, and returns to the interrupted instruction
 * with `sret`.
 *
 * The frame layout comes from include/trap_offsets.h, which
 * include/trap.h also uses to build the C struct, so the two cannot drift
 * apart: a mismatch is a compile error.
 */

#include "trap_offsets.h"

	.section .text.trap, "ax", @progbits
	.align 4
	.globl trap_entry
	.type trap_entry, @function
trap_entry:
	/*
	 * Allocate the frame.  TRAP_FRAME_SIZE is a multiple of 16, so the
	 * stack stays 16-byte aligned and the C call below follows the ABI.
	 */
	addi	sp, sp, -TRAP_FRAME_SIZE

	/*
	 * Save t0 before using it as a scratch register, so that the original
	 * t0 of the interrupted context is not lost.
	 */
	sd	x5, TRAP_OFF_T0(sp)

	/* The interrupted stack pointer is the frame's top. */
	mv	t0, sp
	addi	t0, t0, TRAP_FRAME_SIZE
	sd	t0, TRAP_OFF_ORIGSP(sp)

	/* General purpose registers. */
	sd	x1, TRAP_OFF_RA(sp)
	sd	x3, TRAP_OFF_GP(sp)
	sd	x6, TRAP_OFF_T1(sp)
	sd	x7, TRAP_OFF_T2(sp)
	sd	x8, TRAP_OFF_S0(sp)
	sd	x9, TRAP_OFF_S1(sp)
	sd	x10, TRAP_OFF_A0(sp)
	sd	x11, TRAP_OFF_A1(sp)
	sd	x12, TRAP_OFF_A2(sp)
	sd	x13, TRAP_OFF_A3(sp)
	sd	x14, TRAP_OFF_A4(sp)
	sd	x15, TRAP_OFF_A5(sp)
	sd	x16, TRAP_OFF_A6(sp)
	sd	x17, TRAP_OFF_A7(sp)
	sd	x18, TRAP_OFF_S2(sp)
	sd	x19, TRAP_OFF_S3(sp)
	sd	x20, TRAP_OFF_S4(sp)
	sd	x21, TRAP_OFF_S5(sp)
	sd	x22, TRAP_OFF_S6(sp)
	sd	x23, TRAP_OFF_S7(sp)
	sd	x24, TRAP_OFF_S8(sp)
	sd	x25, TRAP_OFF_S9(sp)
	sd	x26, TRAP_OFF_S10(sp)
	sd	x27, TRAP_OFF_S11(sp)
	sd	x28, TRAP_OFF_S2B(sp)
	sd	x29, TRAP_OFF_T3(sp)
	sd	x30, TRAP_OFF_T4(sp)
	sd	x31, TRAP_OFF_T5(sp)
	sd	x4, TRAP_OFF_ORIGTP(sp)

	/* The CSRs the trap hardware wrote. */
	csrr	t0, sepc
	sd	t0, TRAP_OFF_SEPC(sp)
	csrr	t0, sstatus
	sd	t0, TRAP_OFF_SSTATUS(sp)
	csrr	t0, scause
	sd	t0, TRAP_OFF_SCAUSE(sp)
	csrr	t0, stval
	sd	t0, TRAP_OFF_STVAL(sp)

	/*
	 * Call the C handler with the frame pointer and the hart id.
	 *
	 * The hart id comes from x4 (tp), which _start and secondary_start both
	 * set to it.  It is read here rather than taken from a shared global,
	 * because a global cannot hold a per-hart value: every hart writes the
	 * same word, so a hart asking "which hart am I" would be answered with
	 * whichever hart wrote last.  See the comment in boot.S.
	 *
	 * x4 is restored from the frame further down, so the C code may treat it
	 * as the hart id for the duration of the call.
	 */
	mv	a0, sp
	mv	a1, x4
	call	trap_handler

	/*
	 * Restore.  The CSRs first: the handler may have changed sepc (to skip
	 * a faulting instruction) or sstatus.
	 */
	ld	t0, TRAP_OFF_SEPC(sp)
	csrw	sepc, t0
	ld	t0, TRAP_OFF_SSTATUS(sp)
	csrw	sstatus, t0

	/* General purpose registers. */
	ld	x1, TRAP_OFF_RA(sp)
	ld	x3, TRAP_OFF_GP(sp)
	ld	x5, TRAP_OFF_T0(sp)
	ld	x6, TRAP_OFF_T1(sp)
	ld	x7, TRAP_OFF_T2(sp)
	ld	x8, TRAP_OFF_S0(sp)
	ld	x9, TRAP_OFF_S1(sp)
	ld	x10, TRAP_OFF_A0(sp)
	ld	x11, TRAP_OFF_A1(sp)
	ld	x12, TRAP_OFF_A2(sp)
	ld	x13, TRAP_OFF_A3(sp)
	ld	x14, TRAP_OFF_A4(sp)
	ld	x15, TRAP_OFF_A5(sp)
	ld	x16, TRAP_OFF_A6(sp)
	ld	x17, TRAP_OFF_A7(sp)
	ld	x18, TRAP_OFF_S2(sp)
	ld	x19, TRAP_OFF_S3(sp)
	ld	x20, TRAP_OFF_S4(sp)
	ld	x21, TRAP_OFF_S5(sp)
	ld	x22, TRAP_OFF_S6(sp)
	ld	x23, TRAP_OFF_S7(sp)
	ld	x24, TRAP_OFF_S8(sp)
	ld	x25, TRAP_OFF_S9(sp)
	ld	x26, TRAP_OFF_S10(sp)
	ld	x27, TRAP_OFF_S11(sp)
	ld	x28, TRAP_OFF_S2B(sp)
	ld	x29, TRAP_OFF_T3(sp)
	ld	x30, TRAP_OFF_T4(sp)
	ld	x31, TRAP_OFF_T5(sp)

	/* The thread pointer and the stack pointer go last. */
	ld	x4, TRAP_OFF_ORIGTP(sp)
	ld	t0, TRAP_OFF_ORIGSP(sp)
	mv	sp, t0

	/* Return to the interrupted instruction. */
	sret
	.size trap_entry, . - trap_entry