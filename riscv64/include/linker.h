/*
 * The symbols the linker script exports.
 *
 * They are declared as arrays because they are linker-provided labels, not C
 * objects; only their addresses are ever used.  Taking the address of an
 * array gives the label's address.
 *
 * The four region boundaries are what the Sv39 page tables are built from;
 * see src/vm.c.
 */

#ifndef LINKER_H
#define LINKER_H

/* Start of the executable text, which the script places at the load address. */
extern char __text_start[];
/* End of the executable text, page aligned. */
extern char __text_end[];

/* Start and end of the read-only data, page aligned. */
extern char __rodata_start[];
extern char __rodata_end[];

/* Start of the writable region: .data, .bss and the boot stack. */
extern char __data_start[];
/* First byte above the kernel image, page aligned. */
extern char _kernel_end[];

/* End of the physical page pool. */
extern char _pmm_pool_end[];

/* The boot stack. */
extern char __stack_bottom[];
extern char __stack_top[];

#endif /* LINKER_H */