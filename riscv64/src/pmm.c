/*
 * The physical page allocator. See include/pmm.h for the contract.
 */

#include "pmm.h"

#include "linker.h"

static unsigned long pool_start;
static unsigned long pool_end;
static unsigned long next_free;
static unsigned long used_pages;

/*
 * The pool size is fixed by the linker script, and the two must agree.  The
 * check cannot be a static assertion: the linker symbols are not constant
 * expressions in C, so it is done at run time instead, and the actual size
 * is reported.
 */
void
pmm_init(void)
{
	/* Both ends are already page aligned by the script; align anyway, so
	 * the invariant holds even if the script changes. */
	pool_start = ((unsigned long)_kernel_end + (PMM_PAGE_SIZE - 1)) &
		     ~(PMM_PAGE_SIZE - 1);
	pool_end = (unsigned long)_pmm_pool_end & ~(PMM_PAGE_SIZE - 1);
	next_free = pool_start;
	used_pages = 0;
}

/* The number of pages the linker script actually reserved. */
unsigned long
pmm_pool_pages(void)
{
	return (pool_end - pool_start) / PMM_PAGE_SIZE;
}

void *
pmm_alloc_page(void)
{
	unsigned long page;
	unsigned long i;

	if (next_free + PMM_PAGE_SIZE > pool_end)
		return NULL;

	page = next_free;
	next_free += PMM_PAGE_SIZE;

	/* Zero the page. The caller gets a page whose contents it can rely
	 * on, which matters for a page table: a stale entry would be a
	 * mapping the kernel did not intend. */
	for (i = 0; i < PMM_PAGE_SIZE / sizeof(uint64_t); i++)
		((volatile uint64_t *)page)[i] = 0;

	used_pages++;
	return (void *)page;
}

void
pmm_free_page(void *page)
{
	unsigned long p = (unsigned long)page;

	/* Only pages that came from this pool can be released. */
	if (p < pool_start || p >= pool_end)
		return;
	if (p % PMM_PAGE_SIZE != 0)
		return;

	used_pages--;
}

unsigned long
pmm_used_pages(void)
{
	return used_pages;
}

unsigned long
pmm_free_pages(void)
{
	return (pool_end - pool_start) / PMM_PAGE_SIZE - used_pages;
}
