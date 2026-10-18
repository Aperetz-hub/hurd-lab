/*
 * Sv39 page tables and MMU activation. See include/vm.h for the layout.
 */

#include "vm.h"

#include "console.h"
#include "csr.h"
#include "linker.h"
#include "pmm.h"

/* PTE bits, from the RISC-V privileged specification. */
#define PTE_V (1UL << 0) /* valid */
#define PTE_R (1UL << 1) /* read */
#define PTE_W (1UL << 2) /* write */
#define PTE_X (1UL << 3) /* execute */
#define PTE_U (1UL << 4) /* user */
#define PTE_A (1UL << 5) /* accessed */
#define PTE_D (1UL << 6) /* dirty */

/* Permission sets for the three kinds of mapping the kernel needs. */
#define PTE_PERM_RX (PTE_V | PTE_R | PTE_X | PTE_A)
#define PTE_PERM_R (PTE_V | PTE_R | PTE_A)
#define PTE_PERM_RW (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D)

/* The number of entries in one page table, and the index mask. */
#define PTE_PER_TABLE (VM_PAGE_SIZE / sizeof(uint64_t))
#define PTE_INDEX_MASK (PTE_PER_TABLE - 1)

/* satp mode field: 8 is Sv39. */
#define SATP_MODE_SV39 (8ULL << 60)

/* The NS16550A on the QEMU `virt` board. */
#define UART_BASE 0x10000000UL

static uint64_t *root_table;
static unsigned long mapped_pages;
static int active;

unsigned long
vm_root_ppn(void)
{
	return (unsigned long)root_table >> 12;
}

unsigned long
vm_mapped_pages(void)
{
	return mapped_pages;
}

int
vm_is_active(void)
{
	return active;
}

/*
 * Follow one level of the walk.  If the entry is not valid, allocate a new
 * table from the physical pool and point the entry at it.
 */
static uint64_t *
table_next(uint64_t *table, unsigned index)
{
	uint64_t *entry = &table[index];
	uint64_t *next;

	if ((*entry & PTE_V) == 0) {
		next = (uint64_t *)pmm_alloc_page();
		if (next == NULL)
			return NULL;

		/* A non-leaf entry points at the next level and has no
		 * permission bits of its own. */
		*entry = ((((unsigned long)next) >> 12) << 10) | PTE_V;
		return next;
	}

	return (uint64_t *)(((*entry >> 10) << 12));
}

/*
 * Return the leaf entry for a virtual address, creating the intermediate
 * tables if needed.
 */
static uint64_t *
pte_for(uint64_t va)
{
	unsigned i2 = (unsigned)((va >> 30) & PTE_INDEX_MASK);
	unsigned i1 = (unsigned)((va >> 21) & PTE_INDEX_MASK);
	unsigned i0 = (unsigned)((va >> 12) & PTE_INDEX_MASK);
	uint64_t *level1;
	uint64_t *level2;

	level1 = table_next(root_table, i2);
	if (level1 == NULL)
		return NULL;

	level2 = table_next(level1, i1);
	if (level2 == NULL)
		return NULL;

	return &level2[i0];
}

/* Map every page of [start, end) with the given permissions. */
static void
map_range(uint64_t start, uint64_t end, uint64_t perm)
{
	uint64_t va;

	for (va = start & ~(uint64_t)(VM_PAGE_SIZE - 1); va < end;
	     va += VM_PAGE_SIZE) {
		uint64_t *pte = pte_for(va);
		uint64_t ppn;

		if (pte == NULL)
			return;

		ppn = (va & ~(uint64_t)(VM_PAGE_SIZE - 1)) >> 12;
		*pte = (ppn << 10) | perm;
		mapped_pages++;
	}
}

void
vm_init(void)
{
	uint64_t satp_value;

	/* The root table comes from the physical pool, which is already zeroed
	 * by the allocator. */
	root_table = (uint64_t *)pmm_alloc_page();
	if (root_table == NULL) {
		console_puts("vm: no memory for the root table\r\n");
		return;
	}

	/* The code. */
	map_range((unsigned long)__text_start, (unsigned long)__text_end,
		  PTE_PERM_RX);
	/* The read-only data. */
	map_range((unsigned long)__rodata_start, (unsigned long)__rodata_end,
		  PTE_PERM_R);
	/* The writable data, the bss and the boot stack. */
	map_range((unsigned long)__data_start, (unsigned long)_kernel_end,
		  PTE_PERM_RW);
	/* The physical page pool, which holds the page tables themselves. */
	map_range((unsigned long)_kernel_end, (unsigned long)_pmm_pool_end,
		  PTE_PERM_RW);
	/* The UART. It is MMIO, but it still needs a mapping. */
	map_range(UART_BASE, UART_BASE + VM_PAGE_SIZE, PTE_PERM_RW);

	console_puts("vm: root table ppn ");
	console_puthex(vm_root_ppn());
	console_puts(", ");
	console_putdec(mapped_pages);
	console_puts(" pages mapped, ");
	console_putdec(pmm_used_pages());
	console_puts(" pool pages used\r\n");

	/* Switch the MMU on: Sv39, with the root table as the first level. */
	satp_value = SATP_MODE_SV39 | ((unsigned long)root_table >> 12);
	csr_write_satp(satp_value);

	/* Flush the TLB so that no stale translation survives the switch. */
	__asm__ volatile ("sfence.vma" ::: "memory");

	active = 1;
	console_puts("vm: Sv39 active, satp ");
	console_puthex((unsigned long)csr_read_satp());
	console_puts("\r\n");
}
