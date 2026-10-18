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