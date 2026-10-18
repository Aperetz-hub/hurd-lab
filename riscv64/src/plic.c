/*
 * The QEMU `virt` PLIC, and the first external interrupt the kernel takes.
 * See include/plic.h for the register layout and for where the S-mode context
 * number comes from.
 */

#include "plic.h"

#include "csr.h"
#include "hsm.h"
#include "uart.h"

/* The S-mode context each hart settled on, and whether it settled on one. */
static unsigned long smode_context[HSM_MAX_HARTS];
static unsigned char plic_ready[HSM_MAX_HARTS];

/* External interrupts handled, and the ones that found nothing to claim. */
static unsigned long irq_handled[HSM_MAX_HARTS];
static unsigned long irq_spurious[HSM_MAX_HARTS];
static unsigned long last_source;

static inline uint32_t
plic_read32(unsigned long addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
plic_write32(unsigned long addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

/* The priority of one source. Zero means "never interrupt for this source". */
static inline void
plic_set_priority(unsigned long source, uint32_t priority)
{
	plic_write32(PLIC_BASE + PLIC_PRIORITY_OFFSET + 4 * source, priority);
}

static inline uint32_t
plic_source_priority(unsigned long source)
{
	return plic_read32(PLIC_BASE + PLIC_PRIORITY_OFFSET + 4 * source);
}

/* The pending bits of one group of 32 sources. */
static inline uint32_t
plic_pending_word(unsigned long source)
{
	return plic_read32(PLIC_BASE + PLIC_PENDING_OFFSET + 4 * (source / 32));
}

/* One enable word of a context. */
static inline unsigned long
plic_enable_addr(unsigned long context, unsigned long source)
{
	return PLIC_BASE + PLIC_ENABLE_OFFSET + PLIC_ENABLE_STRIDE * context +
	       4 * (source / 32);
}

static inline void
plic_enable_source(unsigned long context, unsigned long source, int on)
{
	unsigned long addr = plic_enable_addr(context, source);
	uint32_t word = plic_read32(addr);
	uint32_t bit = 1u << (source % 32);

	plic_write32(addr, on ? (word | bit) : (word & ~bit));
}

static inline int
plic_source_enabled(unsigned long context, unsigned long source)
{
	return (plic_read32(plic_enable_addr(context, source)) &
		(1u << (source % 32))) != 0;
}

/* The threshold and the claim/complete word of a context. */
static inline unsigned long
plic_threshold_addr(unsigned long context)
{
	return PLIC_BASE + PLIC_CONTEXT_OFFSET + PLIC_CONTEXT_STRIDE * context;
}

static inline unsigned long
plic_claim_addr(unsigned long context)
{
	return plic_threshold_addr(context) + 4;
}

/*
 * Read the claim word of a context.  This has a side effect by design: it is
 * how the PLIC is told which source the context is taking, and it returns 0
 * when the context has nothing enabled that is pending and above its threshold.
 */
static inline uint32_t
plic_claim(unsigned long context)
{
	return plic_read32(plic_claim_addr(context));
}

static inline void
plic_complete(unsigned long context, uint32_t source)
{
	plic_write32(plic_claim_addr(context), source);
}

unsigned long
plic_smode_context_of(unsigned long hartid)
{
	return PLIC_S_CONTEXT(hartid);
}

unsigned long
plic_smode_context(unsigned long hartid)
{
	if (hartid >= HSM_MAX_HARTS || !plic_ready[hartid])
		return 0;

	return smode_context[hartid];
}

int
plic_source_asserted(unsigned long hartid, unsigned long source)
{
	unsigned long context = plic_smode_context(hartid);

	if ((plic_pending_word(source) & (1u << (source % 32))) == 0)
		return 0;
	if (!plic_source_enabled(context, source))
		return 0;
	/* The PLIC interrupts on priority strictly above the threshold. */
	if (plic_read32(plic_threshold_addr(context)) >= plic_source_priority(source))
		return 0;

	return 1;
}

int
plic_init(unsigned long hartid)
{
	unsigned long context;

	if (hartid >= HSM_MAX_HARTS)
		return -1;

	context = plic_smode_context_of(hartid);
	smode_context[hartid] = context;
	plic_ready[hartid] = 1;

	/*
	 * A priority of one is enough: the threshold of this context is set to
	 * zero below, and the comparison the PLIC makes is strict, so the source
	 * has to be above the threshold and not equal to it.
	 */
	plic_set_priority(PLIC_UART0_IRQ, 1);

	/* Deliver at once, rather than at whatever the firmware left behind. */
	plic_write32(plic_threshold_addr(context), 0);

	/* This source, in this context. */
	plic_enable_source(context, PLIC_UART0_IRQ, 1);

	/*
	 * Let the UART assert the interrupt, and let S-mode take it.  SEIE is the
	 * per-source enable; SIE is the global one, which the timer already set.
	 * Only the boot hart does this: the UART is a single device, its
	 * interrupt belongs to one context, and a second hart claiming the same
	 * source would just find it already claimed.
	 */
	uart_enable_rx_irq();
	csr_write_sie(csr_read_sie() | CSR_SIE_SEIE);

	return 0;
}

void
plic_handle_external(unsigned long hartid)
{
	unsigned long context;
	uint32_t source;

	if (hartid >= HSM_MAX_HARTS || !plic_ready[hartid])
		return;

	context = smode_context[hartid];

	source = plic_claim(context);
	if (source == 0) {
		/*
		 * The interrupt was signalled but the context has nothing to hand
		 * over: the source was already claimed, or it went away before the
		 * trap was taken.  Counted rather than ignored, because a count
		 * that climbs for ever means the claim/complete pair is wrong.
		 */
		irq_spurious[hartid]++;
		return;
	}

	/*
	 * Drain the source before completing it.  The 16550 holds its interrupt
	 * line asserted while the receive register holds data, so completing
	 * first and reading second would leave the source pending and the
	 * interrupt immediately signalled again.
	 */
	if (source == PLIC_UART0_IRQ && uart_rx_ready())
		(void)uart_read_rbr();

	plic_complete(context, source);

	last_source = source;
	irq_handled[hartid]++;
}

unsigned long
plic_irq_count(unsigned long hartid)
{
	if (hartid >= HSM_MAX_HARTS)
		return 0;
	return irq_handled[hartid];
}

unsigned long
plic_spurious_count(unsigned long hartid)
{
	if (hartid >= HSM_MAX_HARTS)
		return 0;
	return irq_spurious[hartid];
}

unsigned long
plic_last_source(void)
{
	return last_source;
}

void
plic_program_context(unsigned long context, unsigned long source)
{
	unsigned long other;

	plic_set_priority(source, 1);

	/* This source, in this context and no other. */
	for (other = 0; other < PLIC_DISCOVER_LIMIT; other++)
		plic_enable_source(other, source, other == context);

	/* Deliver at once rather than at whatever was left behind. */
	plic_write32(plic_threshold_addr(context), 0);
}