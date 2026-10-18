/*
 * Layout of the trap frame.
 *
 * These offsets are the single source of truth for both sides: src/trap.S
 * includes this file, and include/trap.h builds a struct out of the same
 * macros and then checks, at compile time, that every member really lands
 * where the assembly puts it.  A mismatch is a build error, not a trap that
 * corrupts the stack.
 *
 * The frame holds the callee-saved and caller-saved general purpose
 * registers that the S-mode calling convention says a function may clobber,
 * plus the four CSRs the trap hardware writes.
 */

#ifndef TRAP_OFFSETS_H
#define TRAP_OFFSETS_H

#define TRAP_OFF_RA 0	   /* x1  */
#define TRAP_OFF_GP 8	   /* x3  */
#define TRAP_OFF_T0 16	   /* x5  */
#define TRAP_OFF_T1 24	   /* x6  */
#define TRAP_OFF_T2 32	   /* x7  */
#define TRAP_OFF_S0 40	   /* x8  */
#define TRAP_OFF_S1 48	   /* x9  */
#define TRAP_OFF_A0 56	   /* x10 */
#define TRAP_OFF_A1 64	   /* x11 */
#define TRAP_OFF_A2 72	   /* x12 */
#define TRAP_OFF_A3 80	   /* x13 */
#define TRAP_OFF_A4 88	   /* x14 */
#define TRAP_OFF_A5 96	   /* x15 */
#define TRAP_OFF_A6 104   /* x16 */
#define TRAP_OFF_A7 112   /* x17 */
#define TRAP_OFF_S2 120   /* x18 */
#define TRAP_OFF_S3 128   /* x19 */
#define TRAP_OFF_S4 136   /* x20 */
#define TRAP_OFF_S5 144   /* x21 */
#define TRAP_OFF_S6 152   /* x22 */
#define TRAP_OFF_S7 160   /* x23 */
#define TRAP_OFF_S8 168   /* x24 */
#define TRAP_OFF_S9 176   /* x25 */
#define TRAP_OFF_S10 184  /* x26 */
#define TRAP_OFF_S11 192  /* x27 */
#define TRAP_OFF_S2B 200  /* x28 */
#define TRAP_OFF_T3 208   /* x29 */
#define TRAP_OFF_T4 216   /* x30 */
#define TRAP_OFF_T5 224   /* x31 */
#define TRAP_OFF_SEPC 232
#define TRAP_OFF_SSTATUS 240
#define TRAP_OFF_SCAUSE 248
#define TRAP_OFF_STVAL 256
#define TRAP_OFF_ORIGSP 264
#define TRAP_OFF_ORIGTP 272
#define TRAP_OFF_PAD 280

/* Multiple of 16, so the stack stays 16-byte aligned for the C call. */
#define TRAP_FRAME_SIZE 288

#endif /* TRAP_OFFSETS_H */