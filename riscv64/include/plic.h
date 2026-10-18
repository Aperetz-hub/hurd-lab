/*
 * The platform level interrupt controller (PLIC) of the QEMU `virt` board, and
 * the first external interrupt the kernel takes.
 *
 * Register layout
 * ---------------
 *
 * The PLIC sits at 0x0c00_0000.  Inside it, the registers are grouped by offset,
 * and every group is indexed by "context": one interrupt context per hart per
 * privilege mode.  The offsets below are the constants QEMU's `virt` board
 * passes to its PLIC model, and are the same ones OpenSBI's own driver uses
 * (hw/riscv/virt.h and lib/utils/irqchip/plic.c respectively):
 *
 *     PLIC_BASE + 0x000000 + 4 * source      the priority of one source
 *     PLIC_BASE + 0x001000 + 4 * (id / 32)   the pending bits
 *     PLIC_BASE + 0x002000 + 0x80 * context  the enable words of a context
 *                    + 4 * (id / 32)
 *     PLIC_BASE + 0x200000 + 0x1000 * context     the threshold of a context
 *     PLIC_BASE + 0x200000 + 0x1000 * context + 4 its claim / complete word
 *
 * Which context is "S-mode"
 * -------------------------
 *
 * This is the part that cannot be guessed, because getting it wrong means the
 * interrupt is delivered to the wrong privilege mode: to M-mode, where the
 * firmware swallows it, or to a different hart's context.
 *
 * The answer comes from the device tree the firmware is handed, which is what
 * both sides actually agree on.  QEMU's `virt` machine writes the PLIC node's
 * `interrupts-extended` property as one pair of cells per (hart, mode), in the
 * order M-mode then S-mode.  OpenSBI walks that list and derives the context
 * number of an entry as `cell_index / 2`.
 *
 * Measured on 18/10/2026 by dumping the device tree QEMU 10.0.13 builds and
 * walking it the same way OpenSBI does:
 *
 *     -smp 1:  hart 0 M -> 0    hart 0 S -> 1
 *     -smp 2:  hart 0 M -> 0    hart 0 S -> 1
 *             hart 1 M -> 2    hart 1 S -> 3
 *     -smp 4:  hart 0 M -> 0    hart 0 S -> 1
 *             hart 1 M -> 2    hart 1 S -> 3
 *             hart 2 M -> 4    hart 2 S -> 5
 *             hart 3 M -> 6    hart 3 S -> 7
 *
 * so the S-mode context of hart N is 2N+1 on the `virt` board under TCG.  That
 * is `PLIC_S_CONTEXT()` below, and it is the number every register above is
 * indexed with.
 *
 * Because that is a property of how the board is described rather than of the
 * chip, it is not merely assumed: the self test establishes the same number by
 * effect and compares the two.  If a future board describes its contexts
 * differently, the comparison says so instead of the interrupt silently never
 * arriving.
 *
 * Note on KVM: under `-enable-kvm` QEMU writes only two cells per hart, so
 * OpenSBI's `cell_index / 2` gives the S-mode context of hart N as N.  The
 * kernels here are booted under TCG (the Makefile does not pass -enable-kvm),
 * and the self test is what would catch a mismatch.
 *
 * The interrupt
 * -------------
 *
 * UART0 is PLIC source 10 on this board.  Enabling `IER.RDI` on the 16550 makes
 * it assert its interrupt line as soon as a byte arrives in the receive
 * register; the PLIC raises the S-mode external interrupt when that source is
 * pending, enabled in the context, and of higher priority than the threshold.
 * The handler reads the byte, writes the claim/complete word, and the interrupt
 * is done.  Nothing is asserted about how a byte gets there: the console is
 * write-only from the guest's side, so a byte is injected by the host over the
 * serial socket (vm/riscv-uart-inject.py).
 */

#ifndef PLIC_H
#define PLIC_H

#include <stdint.h>

/* The PLIC on the QEMU `virt` board. */
#define PLIC_BASE 0x0c000000UL

/* Register group offsets, relative to PLIC_BASE. */
#define PLIC_PRIORITY_OFFSET 0x000000UL
#define PLIC_PENDING_OFFSET 0x001000UL
#define PLIC_ENABLE_OFFSET 0x002000UL
#define PLIC_ENABLE_STRIDE 0x80UL
#define PLIC_CONTEXT_OFFSET 0x200000UL
#define PLIC_CONTEXT_STRIDE 0x1000UL

/*
 * The windows of the PLIC the page tables have to cover, and how much of each.
 * src/vm.c maps exactly these, so the geometry lives here rather than being
 * written out twice.
 *
 * The context window is the one that is easy to size wrongly.  Its stride is
 * 4 KiB -- one MMU page per interrupt context -- and a hart's context number is
 * not its hart id: PLIC_S_CONTEXT() below gives 2N+1 for the S-mode of hart N.
 * A window of two pages therefore covers the S-mode contexts of harts 0 and 1
 * and nothing else.
 *
 * That was not a hypothetical mistake, it is what this kernel did at first: it
 * booted at -smp 1, 2 and 3 every time and took a store page fault (cause 15) at
 * 0x0c205000 -- context 5, the S-mode of hart 2 -- at -smp 4, but only on the
 * runs where the firmware happened to pick hart 2 or 3 as the boot hart.  The
 * boot hart is chosen by a race, so the failure looked intermittent and was
 * really deterministic.
 *
 * Sixteen contexts is four pages, and covers every hart count this board boots
 * at with room to spare.
 */
#define PLIC_PRIORITY_WINDOW 0x0c000000UL
/* The priorities of 64 sources, then the pending bits. */
#define PLIC_PRIORITY_PAGES 2UL
#define PLIC_ENABLE_WINDOW 0x0c002000UL
/* The enable words of 16 contexts. */
#define PLIC_ENABLE_PAGES 1UL
#define PLIC_CONTEXT_WINDOW 0x0c200000UL
/* The threshold and claim/complete word of 16 contexts. */
#define PLIC_CONTEXT_CONTEXTS 16UL
#define PLIC_CONTEXT_PAGES (PLIC_CONTEXT_CONTEXTS * PLIC_CONTEXT_STRIDE / 4096UL)

/*
 * The interrupt context of the S-mode of hart N.  See the derivation above.
 */
#define PLIC_S_CONTEXT(hartid) (2UL * (hartid) + 1UL)

/* UART0 is PLIC source 10 on the QEMU `virt` board. */
#define PLIC_UART0_IRQ 10UL

/*
 * How many candidate contexts the discovery tries.  PLIC_CONTEXT_CONTEXTS is 16,
 * which is the same 16 contexts the mapping above covers, so the search cannot
 * walk off the mapping.
 */
#define PLIC_DISCOVER_LIMIT PLIC_CONTEXT_CONTEXTS

/*
 * Program the PLIC for the S-mode of this hart and let the UART raise its
 * interrupt.  Returns 0 on success.
 */
int plic_init(unsigned long hartid);

/* The context plic_init() settled on for this hart, or 0 when it has not run. */
unsigned long plic_smode_context(unsigned long hartid);

/* The PLIC context number derived from the hart id alone. */
unsigned long plic_smode_context_of(unsigned long hartid);

/* Called from the trap handler for a supervisor external interrupt. */
void plic_handle_external(unsigned long hartid);

/* External interrupts that were claimed and completed. */
unsigned long plic_irq_count(unsigned long hartid);

/* The source id of the last completed interrupt. */
unsigned long plic_last_source(void);

/* How many times the claim register came back empty. */
unsigned long plic_spurious_count(unsigned long hartid);

/*
 * True while the source is asserted *for this hart*: pending, enabled in this
 * hart's context, and of higher priority than that context's threshold.  That is
 * the condition for the interrupt to be taken.
 *
 * It deliberately mentions the context, so it answers about *this hart's*
 * context only.  While the context discovery has some other candidate context
 * programmed, this hart's enable bit is not the one that was just written, which
 * is why the discovery does not use it and does not wait on a byte at all; see
 * the comment on plic_try_context() in src/main.c.
 */
int plic_source_asserted(unsigned long hartid, unsigned long source);



/*
 * Program one candidate context and nothing else: threshold zero, this source
 * enabled in it, and this source disabled everywhere else.
 *
 * This is the only way to test a candidate honestly.  Reading a context's claim
 * word proves nothing about the board: the claim word answers with a source when
 * *that context's enable bits* allow it, whichever context the firmware actually
 * routes the interrupt to.  Enabling the source in a wrong context made that
 * wrong context hand it straight back, so the search reported its own writes.
 * See src/main.c for what the test does instead.
 */
void plic_program_context(unsigned long context, unsigned long source);

/*
 * How many candidates the functional search skips.
 *
 * Context 0 is the M-mode context of hart 0 on this board.  Enabling a source
 * there raises an M-mode external interrupt, which lands in the firmware, and
 * deliberately provoking the firmware is not a test the kernel should conduct --
 * the M-mode path is not the kernel's to drive.  The S-mode contexts are the odd
 * numbers, so excluding 0 costs the search nothing on a correctly described
 * board, and a board that routed S-mode to context 0 would be a board whose
 * device tree says so, which the DTB derivation in this header would show.
 */
#define PLIC_DISCOVER_FIRST 1UL

#endif /* PLIC_H */