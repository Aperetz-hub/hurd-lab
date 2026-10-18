/*
 * The one fact src/boot.S needs from the C side of the HSM bring-up.
 *
 * `secondary_start` runs before any C code on that hart, so it cannot call a
 * function to look its own stack up: it needs the stack while it is still
 * assembly.  This header therefore names the table, the stride of an entry and
 * the offset of the stack pointer inside an entry, and include/hsm.h builds the
 * C array and then checks, at compile time, that those numbers really describe
 * it.  A mismatch is a build error, not a hart that jumps through a wild stack.
 *
 * The same pattern is used for the trap frame: include/trap_offsets.h is shared
 * by src/trap.S and include/trap.h.
 */

#ifndef HSM_OFFSETS_H
#define HSM_OFFSETS_H

/* The C array that holds one stack top per hart. Defined in src/hsm.c. */
#define HSM_ASM_STACK_TABLE hsm_stack_top

/* Offset of the stack pointer within one entry of the table. */
#define HSM_ASM_STACK_ENTRY 0

/* log2 of the distance between two entries, i.e. the table entry stride. */
#define HSM_ASM_STACK_SHIFT 3

#endif /* HSM_OFFSETS_H */