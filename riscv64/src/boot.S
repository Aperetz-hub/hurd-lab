/*
 * Entry trampoline for the rv64gc kernel image.
 *
 * OpenSBI (the M-mode firmware QEMU's `virt` board boots with `-bios default`)
 * has already switched the machine to S-mode and entered us at physical
 * 0x80200000, which is what the linker script assumes. Per the SBI boot
 * protocol, `a0` holds the hart id and `a1` holds the device tree address.
 *
 * This file is assembled by `riscv64-linux-gnu-gcc`; the C compiler only ever
 * runs from `kmain` onwards.
 */

	.section .text.init, "ax", @progbits
	.globl _start
	.type _start, @function
_start:
	/*
	 * Only the boot hart ever reaches this code.
	 *
	 * Measured on 18/10/2026 with QEMU 10.0.13 and OpenSBI v1.6 on the
	 * `virt` board: with -smp 1, 2, 3 and 4, exactly one hart enters the
	 * kernel, and it is the one OpenSBI picked as the boot hart. The other
	 * harts are held by the firmware and never get here.
	 *
	 * There is deliberately NO "park the other harts" loop here. The boot hart
	 * is not necessarily hart 0: OpenSBI's boot hart is whichever hart wins
	 * the race to the reset vector, so on the same host the banner reported
	 * "Boot HART ID: 0", "1" and "2" on different runs. A loop of the form
	 * "bnez a0, 1b" therefore parks the boot hart itself, and the kernel
	 * never starts. That bug cost two runs out of six at -smp 3 before it was
	 * found.
	 *
	 * Do NOT read `mhartid` here either: it is a machine-mode CSR, and
	 * reading it from S-mode raises an illegal instruction exception, so the
	 * hart would trap before it could say anything. The hart id arrives in
	 * `a0`, which is what the SBI boot protocol passes at the S-mode entry
	 * point; kmain takes it as its first argument.
	 *
	 * When a later milestone starts further harts itself, through the SBI HSM
	 * extension, the parking of those harts belongs in that path, where the
	 * kernel knows which harts it released.
	 */

	/*
	 * gp must be valid before any gp-relative access. norelax is mandatory:
	 * without it the linker may relax the auipc/addi pair into a
	 * gp-relative reference built from gp itself, which is not set yet.
	 */
.option push
.option norelax
	la	gp, __global_pointer$
.option pop

	la	sp, __stack_top

	/*
	 * Publish the hart id in `tp` before anything else.
	 *
	 * `tp` (x4) is the only register that can carry a per-hart value here.
	 * There is no per-CPU storage yet, and a plain global would be shared by
	 * every hart on the board: a static that each hart writes its own id into
	 * is read back by whichever hart wrote last, not by the one asking.
	 * That is not a theoretical concern -- it is exactly what happened the
	 * first time this was written as a static, and the symptom was one hart
	 * reporting zero ticks while another reported the boot hart's count.
	 *
	 * `tp` is preserved across function calls by the calling convention and
	 * is already saved and restored by the trap frame, so the interrupt
	 * handler can read it as the per-hart value it is.  See the second
	 * argument to trap_handler in src/trap.S.
	 */
	mv	tp, a0

	/* Zero the .bss the linker script tells us about. */
	la	t0, __bss_start
	la	t1, __bss_end
2:	bgeu	t0, t1, 3f
	sd	zero, 0(t0)
	addi	t0, t0, 8
	j	2b
3:

	/*
	 * Hand over to C. `a0` still holds the hart id, and since it is the first
	 * argument register it becomes kmain's first parameter -- so the value is
	 * captured before any C code can clobber it.
	 */
	call	kmain

	/*
	 * kmain is diverging, so this is unreachable. Park the hart politely
	 * anyway: wfi keeps the core asleep instead of burning a host CPU.
	 */
4:	wfi
	j	4b
	.size _start, . - _start

/*
 * Entry trampoline for a hart the kernel released itself, through the SBI HSM
 * extension's `sbi_hart_start`.
 *
 * The firmware jumps here in S-mode, with the hart id in `a0` (the opaque value
 * that was passed to sbi_hart_start) and the device tree address in `a1` -- the
 * same protocol that brought the boot hart to `_start`.  Unlike the boot hart,
 * this one has no stack of its own yet, so getting a stack is the first job.
 *
 * The stack comes from the table in src/hsm.c, indexed by hart id.  That table
 * has to be addressed here, in assembly, because C cannot run before `sp` is
 * set; include/hsm_offsets.h names it and the stride, and include/hsm.h checks
 * at compile time that those numbers describe the C array.
 *
 * The compiler is not involved in the stack switch, so no gp-relative access
 * and no C call happens before `sp` and `gp` are both right.  norelax is
 * mandatory on the gp load for the same reason as in `_start`.
 */

#include "hsm_offsets.h"

	.section .text.init.secondary, "ax", @progbits
	.globl secondary_start
	.type secondary_start, @function
secondary_start:
	/*
	 * a0 is the hart id; t0 holds it while t1 walks the stack table.
	 *
	 * The hart id is published in tp first, for the reason given in
	 * _start: tp is the only per-hart register available here, and the
	 * interrupt handler reads the hart id from it.
	 */
	mv	tp, a0
	mv	t0, a0

	/* sp = hsm_stack_top[hartid] */
	la	t1, HSM_ASM_STACK_TABLE
	slli	t0, t0, HSM_ASM_STACK_SHIFT
	add	t1, t1, t0
	ld	sp, HSM_ASM_STACK_ENTRY(t1)

	/*
	 * A zero here means the boot hart never published a stack for this hart,
	 * or released it without one.  There is nothing safe to do on a stack
	 * pointer of zero, so park instead of faulting on the way down.
	 */
	beqz	sp, 6f

.option push
.option norelax
	la	gp, __global_pointer$
.option pop

	/* hsm_secondary_main() diverges; a0 still holds the hart id. */
	call	hsm_secondary_main

	/*
	 * Unreachable, exactly as after kmain above.  Park rather than run off
	 * the end of the image.
	 */
6:	wfi
	j	6b
	.size secondary_start, . - secondary_start