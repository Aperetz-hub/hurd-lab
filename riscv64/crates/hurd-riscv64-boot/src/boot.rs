/*
 * Entry trampoline for the rv64gc kernel image.
 *
 * OpenSBI (the M-mode firmware QEMU's `virt` machine boots) has already
 * switched the machine to S-mode, installed a stack pointer and entered us at
 * physical 0x80200000, which is what our linker script assumes.
 *
 * Written as a naked-ish global_asm block instead of a separate .S file so the
 * whole boot path lives in one place and shows up in `cargo` dependency
 * tracking. If this ever stops being possible, move it to `src/boot/boot.S` and
 * reference it with `#[link_section]`.
 */

use core::arch::global_asm;

global_asm!(
    r#"
.section .text.init, "ax"
.globl _start
_start:
    /*
     * Park every hart but hart 0. QEMU's `virt` board powers one hart by
     * default (`-smp 1`), so this loop normally falls through immediately; with
     * more harts it keeps them from racing into the kernel before it is ready.
     *
     * The hart id arrives in `a0`, which is what the SBI boot protocol defines
     * for the S-mode entry point.  Do NOT read `mhartid` here: it is a
     * machine-mode CSR and reading it from S-mode raises an illegal
     * instruction exception, so the hart traps before it can say anything.
     */
1:  bnez    a0, 1b

    /*
     * gp must be valid before any gp-relative access. norelax is mandatory:
     * without it the linker may insert an auipc/addi pair that clobbers t0,
     * which we still need.
     */
.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    la      sp, __stack_top

    /* Zero the .bss the linker script tells us about. */
    la      t0, __bss_start
    la      t1, __bss_end
2:  bgeu    t0, t1, 3f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       2b
3:

    /* Tell the firmware we survived the trip, then hand over to Rust. */
    call    kmain

    /*
     * kmain is diverging, so this is unreachable. Park the hart politely
     * anyway: wfi keeps the core asleep instead of burning a host CPU.
     */
4:  wfi
    j       4b
.size _start, . - _start
"#
);