/*
 * Linker script for the rv64gc kernel image on the QEMU `virt` board.
 *
 * Layout facts that matter:
 *   - QEMU's `virt` board maps DRAM at 0x8000_0000.
 *   - OpenSBI occupies the first megabyte or so; `cargo build` output is handed
 *     to QEMU with `-kernel`, which places it at 0x8020_0000. That address is
 *     therefore the script's ORIGIN.
 *   - The kernel is a flat image: no MMU, no demand paging, no relocation.
 *     Absolute (and gp-relative) addressing are enough for the first milestones.
 *
 * `__bss_start` / `__bss_end` and `__stack_top` are consumed by the `_start`
 * trampoline in `src/boot.rs`; `__global_pointer$` is the gp base that the same
 * trampoline loads with `la gp, __global_pointer$`.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* r-x */
    rodata PT_LOAD FLAGS(4);   /* r-- */
    data   PT_LOAD FLAGS(6);   /* rw- */
}

SECTIONS
{
    /* Must match where QEMU loads the image (`-kernel` default). */
    . = 0x80200000;

    .text.init : {
        KEEP(*(.text.init))
    } :text

    .text : ALIGN(4) {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :rodata

    .data : ALIGN(8) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    . = ALIGN(8);
    __bss_start = .;

    .bss (NOLOAD) : ALIGN(8) {
        /*
         * Mid-way into .bss, as the RISC-V psABI suggests: keeps gp-relative
         * addressing valid for small objects without wasting much space.
         */
        . = ALIGN(8);
        __global_pointer$ = . + 0x800;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(8);
    __bss_end = .;

    /*
     * Boot stack. 64 KiB is generous for a freestanding kernel and costs
     * nothing until something actually uses it; no_initramfs-style garbage
     * collection will want to grow this.
     */
    . = ALIGN(4096);
    __stack_bottom = .;
    . += 64K;
    __stack_top = .;

    _end = .;

    /*
     * Without this, any unwind table or eh_frame section the toolchain emits
     * gets an address in the image but no way to be used (panic=abort, no
     * personality routine), and some linkers then complain.
     */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note .note.*)
    }
}