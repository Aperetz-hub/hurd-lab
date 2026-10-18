/*
 * Sv39 page tables and MMU activation.
 *
 * The kernel keeps running at its physical address, so the tables are an
 * identity mapping: the virtual address of a page is its physical address.
 * What the tables buy is *permissions*, which a flat mapping cannot express.
 *
 * The layout the tables reproduce comes from the linker script:
 *
 *     [__text_start,   __text_end)    R-X
 *     [__rodata_start, __rodata_end)  R--
 *     [__data_start,   _kernel_end)   RW-  (.data, .bss, boot stack)
 *     [_kernel_end,    _pmm_pool_end) RW-  (the physical page pool)
 *     [0x1000_0000,    0x1000_1000)   RW-  (the NS16550A, MMIO)
 *     [0x0c00_0000,    0x0c00_2000)   RW-  (PLIC source priorities, pending)
 *     [0x0c00_2000,    0x0c00_3000)   RW-  (PLIC enable words)
 *     [0x0c20_0000,    0x0c20_2000)   RW-  (PLIC thresholds, claim/complete)
 *
 * The devices must be mapped explicitly: none of them is part of the kernel
 * image, and an access to an unmapped address is a page fault, not a silent
 * no-op.
 *
 * Everything is identity mapped, so the only address space the kernel uses is
 * its own image plus the two devices.  There is no user space yet, and no
 * attempt is made to map the rest of DRAM.
 */

#ifndef VM_H
#define VM_H

#include <stdint.h>

/* The size of a page, and of a page table. The same in Sv39. */
#define VM_PAGE_SIZE 4096UL

/* The number of levels in an Sv39 page table. */
#define VM_LEVELS 3

/*
 * Build the page tables and switch the MMU on, in Sv39 mode.
 *
 * After this returns, the kernel is running with the MMU live: the code is
 * executable, the read-only data is not writable, and the writable data and
 * the devices are writable but not executable.
 */
void vm_init(void);

/* True once the MMU has been switched on. */
int vm_is_active(void);

/* The physical page number of the root table, for the log. */
unsigned long vm_root_ppn(void);

/*
 * The whole `satp` value this kernel installed: the Sv39 mode field and the
 * root page number.
 *
 * A hart the boot hart releases later through the SBI HSM extension writes
 * exactly this into its own `satp`, so every hart shares one page table rather
 * than each of them building one.  Shared tables are the point: the physical
 * page pool lives in the mapping, and so do the stacks handed to the secondaries.
 */
unsigned long vm_shared_satp(void);

/* The number of leaf entries that were mapped. */
unsigned long vm_mapped_pages(void);

#endif /* VM_H */