/*
 * Linker script for the rv64gc kernel image on the QEMU `virt` board.
 *
 * Layout facts that matter:
 *   - QEMU's `virt` board maps DRAM at 0x8000_0000.
 *   - OpenSBI occupies the first megabyte or so; the kernel ELF is handed to
 *     QEMU with `-kernel`, which loads it at 0x8020_0000. That address is
 *     therefore the script's ORIGIN.
 *   - The kernel is a flat image: no MMU, no demand paging, no relocation.
 *     Absolute (and gp-relative) addressing are enough for the first milestones.
 *
 * From milestone 3 on, the image is also the thing the Sv39 page tables
 * describe, so the section boundaries are padded to 4 KiB page boundaries.
 * That is what lets each section carry its own page permissions:
 *
 *     [__text_start,   __text_end)    R-X
 *     [__rodata_start, __rodata_end)  R--
 *     [__data_start,   _kernel_end)    RW-   (data, bss, stack)
 *     [_kernel_end,    _pmm_pool_end)  RW-   (the physical page pool)
 *
 * .data and .bss are deliberately NOT separated by a page gap: they hold the
 * small-data objects that the RISC-V `gp` convention addresses relative to
 * `__global_pointer$`, and keeping them adjacent preserves that.  The
 * assembler resolves the distance and refuses to relax an access that does
 * not fit, so a mistake here is a link error rather than a wrong address.
 *
 * `__bss_start` / `__bss_end` and `__stack_top` are consumed by the `_start`
 * trampoline in `src/boot.S`; `__global_pointer$` is the gp base that the same
 * trampoline loads with `la gp, __global_pointer$`.  `_kernel_end` and
 * `_pmm_pool_end` delimit the physical page pool that `src/pmm.c` hands out.
 *
 * Built with `riscv64-linux-gnu-gcc -nostdlib -T linker/riscv64-virt.ld`.
 */

/* Number of 4 KiB pages the physical allocator owns. Must match
   PMM_POOL_PAGES in include/pmm.h; the two are cross-checked at compile
   time. */
PMM_POOL_PAGES = 64;

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
    __text_start = .;

    .text.init : {
        KEEP(*(.text.init))
    } :text

    /* A page boundary, so the code and the read-only data never share a
       page and can be given different permissions. */
    . = ALIGN(4096);

    .text : {
        *(.text .text.*)
    } :text
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :rodata
    . = ALIGN(4096);
    __rodata_end = .;

    /* Everything from here to _kernel_end is writable and not executable. */
    __data_start = .;
    .data : {
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
     * nothing until something actually uses it; a future allocator will want
     * to grow this.
     */
    . = ALIGN(4096);
    __stack_bottom = .;
    . += 64K;
    __stack_top = .;

    /*
     * The physical page pool starts here, on a page boundary, and is the
     * first thing above the kernel image.
     */
    . = ALIGN(4096);
    _kernel_end = .;
    . += PMM_POOL_PAGES * 4096;
    _pmm_pool_end = .;

    _end = .;

    /*
     * Without this, any unwind table or eh_frame section the toolchain emits
     * gets an address in the image but no way to be used (no libgcc unwinder,
     * no personality routine), and some linkers then complain.
     */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note .note.*)
    }
}