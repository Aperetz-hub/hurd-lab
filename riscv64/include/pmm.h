/*
 * The physical page allocator.
 *
 * A fixed pool of 4 KiB pages lives above the kernel image, between the
 * linker symbols `_kernel_end` and `_pmm_pool_end`.  The pool is deliberately
 * static: there is no memory to discover at boot yet (that needs the device
 * tree), and a fixed pool is enough for the page tables of the first
 * milestone.
 *
 * Pages are handed out 4 KiB aligned and zeroed, so a caller can use one as a
 * page table or as fresh memory without having to clear it first.
 */

#ifndef PMM_H
#define PMM_H

#include <stddef.h>
#include <stdint.h>

/* The pool must match PMM_POOL_PAGES in linker/riscv64-virt.ld. */
#define PMM_POOL_PAGES 64

/* The page size: 4 KiB, the RISC-V page granule. */
#define PMM_PAGE_SIZE 4096UL

/* Prepare the allocator. Call once, before anything asks for a page. */
void pmm_init(void);

/*
 * Return one zeroed, 4 KiB aligned page, or NULL when the pool is
 * exhausted.
 */
void *pmm_alloc_page(void);

/*
 * Return a page to the pool. The page is not zeroed on release; the next
 * allocation zeroes it.
 */
void pmm_free_page(void *page);

/* Number of pages currently handed out. */
unsigned long pmm_used_pages(void);

/* Number of pages still available. */
unsigned long pmm_free_pages(void);

/* Number of pages the linker script actually reserved. */
unsigned long pmm_pool_pages(void);

#endif /* PMM_H */