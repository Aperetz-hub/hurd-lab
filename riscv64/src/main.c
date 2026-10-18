/*
 * The kernel proper.
 *
 * `kmain` is called from the `_start` trampoline in `boot.S`, in S-mode, with
 * the stack pointing at `__stack_top` and `.bss` already zeroed. It receives
 * the hart id that the SBI boot protocol passed in `a0`, because `a0` is the
 * first argument register and `boot.S` calls us without touching it.
 *
 * Milestone 4 does, in this order:
 *   1. bring up the console;
 *   2. exercise the SBI base extension and print what the firmware says
 *      about itself (spec version, implementation id and version, and which
 *      extensions are available);
 *   3. install the S-mode trap vector;
 *   4. arm the S-mode timer;
 *   5. build the Sv39 page tables and switch the MMU on;
 *   6. take five supervisor timer interrupts *with the MMU live*, which is
 *      what proves that the identity mapping, the permissions and the device
 *      mappings are all right;
 *   7. program the PLIC and let the UART raise an external interrupt;
 *   8. release every other hart through the SBI HSM extension, and wait for
 *      each to report that it is running with the MMU live and taking its own
 *      timer interrupts;
 *   9. report milestone 4 and park.
 *
 * Every step prints what it observed, so that a failure says where it
 * happened rather than only that it happened.
 *
 * See README.md for what comes next.
 */

#include "console.h"
#include "csr.h"
#include "hsm.h"
#include "linker.h"
#include "pmm.h"
#include "plic.h"
#include "sbi.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

/* Declared here as well because boot.S calls it by name. */
void kmain(unsigned long hartid);

/*
 * How many of the boot hart's own 10 ms ticks a wait may last before it gives up.
 *
 * The unit matters, and getting it wrong is what a bounded wait is for.  These
 * loops spend their time in `wfi`, so each iteration is not a cheap poll: it
 * lasts until the next timer interrupt, which is 10 ms.  Bounding such a loop by
 * an iteration count turns "two million attempts" into "two million ticks", which
 * is over five hours of guest time.  Bounding it in ticks says what it means:
 * this many hundred milliseconds.
 *
 * 100 ticks is one second.  The host injector answers a trigger line in
 * milliseconds, and an interrupt that has not arrived within a second is not
 * going to arrive within a run of this test.
 */
#define WAIT_TICKS 100UL

/*
 * How many of the boot hart's own 10 ms ticks it will spend waiting for the
 * secondaries.  200 is two seconds, which is far longer than a hart needs to
 * run a few instructions and take an interrupt, and short enough that a
 * secondary that never arrives is reported rather than waited on.
 */
#define HSM_WAIT_TICKS 200UL

/*
 * How many of the boot hart's own 10 ms ticks one PLIC context trial waits for
 * its interrupt.  30 is 300 ms: a correctly wired context raises the trap as soon
 * as the byte lands, and a wrongly wired one never does, so the budget only has
 * to be long enough not to miss a slow-but-correct one.
 */
#define PLIC_TRIAL_TICKS 30UL

/* The SBI specification version, as major.minor. */
static void
print_spec_version(struct sbiret r)
{
	unsigned long v;

	/*
	 * The SBI base extension returns the pair (a0 = error, a1 = value).
	 * A firmware that answers in a0 instead is not conformant, so the value
	 * is taken from a1 and the error code is printed alongside it.
	 */
	v = (unsigned long)r.value;

	console_puts("SBI spec version ");
	console_putdec((v >> 24) & 0x7f);
	console_putc('.');
	console_putdec(v & 0xffffff);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(", value ");
	console_puthex(v);
	console_puts(")\r\n");
}

static void
print_impl_id(const char *what, struct sbiret r)
{
	console_puts(what);
	console_puts(" ");
	console_puthex((unsigned long)r.value);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");
}

static void
print_probe(const char *what, unsigned long eid)
{
	struct sbiret r = sbi_probe_extension(eid);

	console_puts(what);
	console_puts(" eid ");
	console_puthex(eid);
	console_puts(" -> ");
	console_putdec((unsigned long)r.value);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");
}

/*
 * Which hart this code is on.  kmain is the boot hart; the secondary harts never
 * reach any of this, so the predicates below only ever describe the boot hart.
 */
static unsigned long boot_hart;

/* Ask until `ready` agrees, or until the tick budget runs out. */
static int
wait_until(int (*ready)(void))
{
	unsigned long started = timer_tick_count(boot_hart);

	for (;;) {
		if (ready())
			return 0;
		if (timer_tick_count(boot_hart) >= started + WAIT_TICKS)
			return -1;
		/* STIE is still on, so this wakes up rather than spinning. */
		__asm__ volatile ("wfi");
	}
}

/* Whether the boot hart has taken enough timer interrupts. */
static int
enough_ticks(void)
{
	return timer_tick_count(boot_hart) >= TIMER_TICKS_TARGET;
}

#ifdef SELFTEST_PLIC
/* The other two things the PLIC self test waits for. */
static int
external_interrupt_arrived(void)
{
	return plic_irq_count(boot_hart) != 0;
}

#endif

/*
 * Report every hart the kernel knows about.
 *
 * One line per hart, with the same shape whatever the hart count, so that
 * `make verify` can count them: at -smp N it requires exactly N lines ending in
 * "running".  That is what proves every hart of the board reached the MMU, and
 * not just the one that printed this.
 */
static void
report_harts(void)
{
	unsigned long i;

	for (i = 0; i < HSM_MAX_HARTS; i++) {
		struct hsm_hart *h = &hsm_harts[i];

		if (h->started == 0)
			continue;

		console_puts("hsm: hart ");
		console_putdec(i);
		if (h->running) {
			console_puts(" running, ");
			console_putdec(timer_tick_count(i));
			console_puts(" ticks (");
			console_putdec(timer_tick_count_mmu(i));
			console_puts(" with the MMU live), satp ");
			console_puthex(h->satp);
			console_puts(", stack top ");
			console_puthex(h->stack_top);
		} else {
			console_puts(" NOT running, error ");
			console_putdec(h->error);
		}
		console_puts("\r\n");
	}
}

#ifdef SELFTEST_PLIC
/*
 * Cross-check the S-mode context number derived from the hart id against the one
 * the board actually routes the interrupt to.
 *
 * The first half is the normal path: one byte arrives, the trap fires, the
 * handler claims source 10, drains the UART and completes it.  That is the
 * deliverable, and the count of completed interrupts is the evidence.
 *
 * The second half is the check on PLIC_S_CONTEXT(), and it has to be a
 * *functional* test rather than a read of the PLIC's own registers.  The obvious
 * version -- enable the source in one candidate context at a time and read that
 * context's claim word -- does not work, and the reason is worth stating:
 *
 *   A claim word answers with a source when *that context's enable bits* allow
 *   it, whatever context the firmware wired the interrupt to.  So enabling the
 *   source in a wrong context makes that wrong context hand it straight back,
 *   and the search measures its own writes.  Measured on 18/10/2026, it reported
 *   context 0 while the interrupt-driven path had just delivered the same source
 *   correctly through context 1.
 *
 * The test that does work is to ask the question the hardware answers: enable the
 * source in one candidate at a time and see whether the supervisor external
 * interrupt arrives at all.  Only the context wired to this hart's S-mode
 * external interrupt can raise the trap.  A candidate that is not the right one
 * simply never fires, which is a result.
 *
 * Each candidate needs its own byte, because the handler drains the receive
 * register to clear the interrupt line, so the guest asks the injector for one per
 * trial.  The expected context is tried first, so on a correctly described board
 * this is a single byte and the whole search costs one trial.
 */
static int
plic_try_context(unsigned long candidate)
{
	unsigned long started = timer_tick_count(boot_hart);
	unsigned long before;

	/*
	 * Read the interrupt count before anything else, and in particular before
	 * the trigger goes out.
	 *
	 * The trap can arrive at any moment after the byte lands -- including
	 * during this function -- because interrupts are enabled and the candidate
	 * is programmed.  Reading the count after the byte arrived counts an
	 * interrupt that has already happened, and the trial then waits for a
	 * second one that never comes.
	 *
	 * There is deliberately no "has the byte landed yet" wait before it, which
	 * an earlier version had and which inverted the result: a correctly wired
	 * context raises the trap so promptly that the handler has already drained
	 * the receive register by the time the pending bit is polled, so the wait
	 * timed out on precisely the context that worked and reported the search as
	 * having found nothing.  Asking "has the interrupt arrived" already covers
	 * the byte having landed.
	 */
	before = plic_irq_count(boot_hart);

	plic_program_context(candidate, PLIC_UART0_IRQ);

	console_puts("selftest: trial context ");
	console_putdec(candidate);
	console_puts(", asking for a byte\r\n");
	console_puts("PLIC-TRIAL\r\n");

	for (;;) {
		if (plic_irq_count(boot_hart) > before)
			return 1;

		/*
		 * Bounded in ticks, because the expected outcome for a wrong
		 * candidate is silence and an unbounded wait on silence never ends.
		 */
		if (timer_tick_count(boot_hart) >= started + PLIC_TRIAL_TICKS) {
			console_puts("selftest: context ");
			console_putdec(candidate);
			console_puts(" raised no interrupt\r\n");
			return 0;
		}

		/* STIE is still on, so this wakes. */
		__asm__ volatile ("wfi");
	}
}

/*
 * The candidates, with the expected one first.
 *
 * Trying the expected context first means a correctly described board needs one
 * trial and one injected byte; the rest of the list is only walked when the
 * formula is wrong, which is exactly when it is worth the bytes.
 */
static unsigned long
plic_candidate(unsigned long n)
{
	unsigned long expected = plic_smode_context_of(boot_hart);
	unsigned long candidate;

	if (n == 0)
		return expected;

	/*
	 * Then every context from PLIC_DISCOVER_FIRST upwards, skipping the one
	 * already tried.  Counting up from PLIC_DISCOVER_FIRST and stepping over
	 * `expected` gives a sequence that is strictly increasing and covers each
	 * context once; modulo arithmetic here repeated a candidate, which wasted an
	 * injected byte and made the log read as though two different contexts had
	 * failed.
	 */
	candidate = PLIC_DISCOVER_FIRST + (n - 1);
	if (candidate >= expected)
		candidate++;

	return candidate;
}

static void
plic_selftest(void)
{
	long found = -1;
	unsigned long expected;
	unsigned long trial;

	/*
	 * The boot hart reports every tick by default, which is what makes a boot
	 * log readable and would bury this test's own output: each search spends a
	 * whole tick budget waiting for an interrupt that is never going to come.
	 * Quiet the tick report for the duration and put it back at the end.
	 */
	timer_set_verbose(boot_hart, 0);

	console_puts("plic: context from the hart id ");
	console_putdec(plic_smode_context_of(boot_hart));
	console_puts(", PLIC base 0x");
	console_puthex(PLIC_BASE);
	console_puts(", source ");
	console_putdec(PLIC_UART0_IRQ);
	console_puts("\r\n");

	/*
	 * Take the interrupt the interrupt-driven way first, because that is what
	 * the PLIC path has to do: trap in, claim, read, complete.
	 */
	console_puts("selftest: waiting for an injected byte on the serial line\r\n");
	console_puts("PLIC-INJECT-1\r\n");

	if (wait_until(external_interrupt_arrived) != 0) {
		console_puts("selftest: FAIL, no external interrupt within the tick budget\r\n");
		return;
	}

	console_puts("selftest: external interrupt ");
	console_putdec(plic_irq_count(boot_hart));
	console_puts(", source ");
	console_putdec(plic_last_source());
	console_puts(", spurious claims ");
	console_putdec(plic_spurious_count(boot_hart));
	console_puts("\r\n");

	if (plic_irq_count(boot_hart) != 1 || plic_last_source() != PLIC_UART0_IRQ) {
		console_puts("selftest: FAIL, the interrupt did not claim source 10\r\n");
		return;
	}

	/* Now establish the context by effect, and compare it with the formula. */
	expected = plic_smode_context_of(boot_hart);

	for (trial = 0; trial < PLIC_DISCOVER_LIMIT; trial++) {
		unsigned long candidate = plic_candidate(trial);

		if (candidate >= PLIC_DISCOVER_LIMIT)
			continue;

		if (plic_try_context(candidate)) {
			found = (long)candidate;
			break;
		}
	}

	console_puts("selftest: the board routed source ");
	console_putdec(PLIC_UART0_IRQ);
	console_puts(" to context ");
	if (found < 0) {
		console_puts("none\r\n");
		console_puts("selftest: FAIL, no context raised the interrupt\r\n");
		timer_set_verbose(boot_hart, 1);
		return;
	}

	console_putdec((unsigned long)found);
	console_puts(", the hart id formula gives ");
	console_putdec(expected);
	console_puts("\r\n");

	/* Put the real configuration back, and the tick report with it. */
	plic_init(boot_hart);
	timer_set_verbose(boot_hart, 1);

	if ((unsigned long)found != expected) {
		console_puts("selftest: FAIL, PLIC_S_CONTEXT() does not match the board\r\n");
		return;
	}

	console_puts("selftest: scause was ");
	console_puthex((1ULL << 63) | SUPERVISOR_EXTERNAL_INTERRUPT);
	console_puts(", claimed and completed cleanly\r\n");
	console_puts("selftest: PASS\r\n");
}
#endif /* SELFTEST_PLIC */

void
kmain(unsigned long hartid)
{
	struct sbiret r;
	void *probe_page;
	unsigned long running;
	unsigned long i;
	int secondaries;

	/*
	 * Hold on to the hart id: `a0` is the first argument register, so any call
	 * from here on may overwrite it.  boot.S has already published the same
	 * value in `tp`, which is what the interrupt handler reads, because a
	 * global could not serve that purpose: every hart on the board would share
	 * it, and each would read back whichever hart wrote last.
	 */
	boot_hart = hartid;

	console_init();

	/*
	 * Report the hart id the firmware handed us. Printing it is how "a0 is
	 * the hart id" gets checked rather than assumed.
	 */
	console_puts("SBI hart id ");
	console_puthex((uint64_t)hartid);
	console_puts("\r\n");

	/*
	 * The legacy console extension, to prove the firmware path is alive.
	 * The legacy extensions return their result in a0, not in the a0/a1
	 * pair, so this call does not go through sbi_ecall().
	 */
	if (sbi_legacy_console_putc('!'))
		console_puts("\r\nSBI legacy_putc works: yes\r\n");
	else
		console_puts("\r\nSBI legacy_putc works: no (extension absent)\r\n");

	/* Identify the firmware we are running under. */
	print_spec_version(sbi_get_spec_version());
	print_impl_id("SBI impl id", sbi_get_impl_id());
	print_impl_id("SBI impl version", sbi_get_impl_version());

	/* Which extensions are actually there. */
	print_probe("SBI extension probe base", SBI_EID_BASE);
	print_probe("SBI extension probe legacy console", SBI_EID_LEGACY_CONSOLE);
	print_probe("SBI extension probe time", SBI_EID_TIME);
	print_probe("SBI extension probe hsm", SBI_EID_HSM);

	/* The SBI TIME extension reads the wall clock. */
	r = sbi_timer_time();
	console_puts("SBI wall clock ");
	console_putdec((unsigned long)r.value);
	console_puts(" (error ");
	console_putdec_signed(r.error);
	console_puts(")\r\n");

	console_puts("hurd-riscv64: milestone 1 reached\r\n");

	/*
	 * The physical allocator. It runs before the MMU is on, because the
	 * page tables are built from it.
	 */
	pmm_init();
	console_puts("pmm: pool ");
	console_putdec(pmm_free_pages());
	console_puts(" pages of 4 KiB\r\n");

	/* Take one page and hand it straight back, to show that the pool
	 * hands out aligned, zeroed pages. */
	probe_page = pmm_alloc_page();
	if (probe_page == NULL) {
		console_puts("pmm: no page available\r\n");
	} else {
		console_puts("pmm: allocated page at ");
		console_puthex((unsigned long)probe_page);
		console_puts(", aligned=");
		console_puthex(((unsigned long)probe_page % 4096) == 0 ? 1 : 0);
		console_puts(", free now ");
		console_putdec(pmm_free_pages());
		console_puts("\r\n");
		pmm_free_page(probe_page);
	}

	/*
	 * Install the S-mode trap vector. The ecall from S-mode is not
	 * delegated to S-mode by the firmware, so this does not disturb the SBI
	 * path: the ecall traps straight to M-mode.
	 */
	console_puts("firmware stvec was ");
	console_puthex(csr_read_stvec());
	console_puts("\r\n");
	csr_write_stvec((unsigned long)trap_entry);
	console_puts("installed stvec ");
	console_puthex(csr_read_stvec());
	console_puts("\r\n");
#ifdef SELFTEST_ILLEGAL_INSTRUCTION
	/*
	 * Self-test: raise a deliberate illegal instruction and check that the
	 * trap handler catches it and reports the context.  The handler treats
	 * an exception it does not expect as fatal, so the kernel stops here on
	 * purpose; the report is what is being checked.
	 */
	trap_expected_cause = 2;
	console_puts("selftest: raising an illegal instruction\r\n");
	__asm__ volatile (".word 0x00000000");
	console_puts("selftest: FAIL, the illegal instruction did not fault\r\n");
#endif

	console_puts("hurd-riscv64: milestone 2 reached\r\n");

	/* Arm this hart's timer. */
	if (timer_arm(hartid, TIMER_INTERVAL_TICKS, TIMER_ARM_BOOT) != 0) {
		console_puts("hurd-riscv64: milestone 4 NOT reached, no timer\r\n");
		for (;;) {
			__asm__ volatile ("wfi");
		}
	}
	timer_report(hartid);

	/*
	 * Switch the MMU on. Everything after this point runs with the page
	 * tables live, which is the point of the exercise: the console, the
	 * trap vector, the timer and the PLIC must all keep working.
	 */
	vm_init();

#ifdef SELFTEST_RODATA_FAULT
	/*
	 * Self-test: the read-only data is mapped read-only, so a store to it
	 * must raise a store/AMO page fault (cause 15).  If the permissions
	 * were wrong the store would simply succeed, and the test would fail
	 * by reaching the code after it.
	 */
	trap_expected_cause = 15;
	console_puts("selftest: storing to .rodata at ");
	console_puthex((unsigned long)__rodata_start);
	console_puts("\r\n");
	*(volatile uint64_t *)__rodata_start = 0;
	console_puts("selftest: FAIL, the store to .rodata did not fault\r\n");
#endif

	/* Wait for the interrupts, with the MMU live. */
	wait_until(enough_ticks);

	console_puts("hurd-riscv64: milestone 3 reached\r\n");

	/*
	 * The PLIC. This is done before the other harts are released, so the
	 * only hart that programs it is the one that reports it, and a secondary
	 * that was released first could not have found the UART interrupt in a
	 * state worth describing.
	 */
	if (plic_init(hartid) != 0) {
		console_puts("plic: init failed for hart ");
		console_putdec(hartid);
		console_puts("\r\n");
	}

	/* The boot hart's own slot, so that report_harts() covers every hart. */
	if (hartid < HSM_MAX_HARTS) {
		hsm_harts[hartid].started = 1;
		hsm_harts[hartid].running = 1;
		hsm_harts[hartid].satp = csr_read_satp();
		hsm_harts[hartid].stack_top = (unsigned long)__stack_top;
	}

	/* Release every other hart the firmware is holding. */
	secondaries = hsm_start_secondaries(hartid);
	if (secondaries < 0)
		secondaries = 0;

	if (secondaries > 0) {
		/*
		 * Wait for all of them to come up, then for each to have taken a
		 * timer interrupt with the MMU live.  Both waits are bounded, in
		 * this hart's own ticks, because a hart that never arrives is
		 * exactly the case the report below has to be able to describe.
		 *
		 * The second wait is the one that matters.  A secondary that has
		 * installed `satp` and set a flag has proved almost nothing; one
		 * that has taken a timer interrupt since has taken a trap through
		 * the shared page tables and returned, which is the whole claim.
		 */
		running = hsm_wait_for_secondaries(hartid, (unsigned long)secondaries,
						   HSM_WAIT_TICKS);
		console_puts("hsm: ");
		console_putdec(running);
		console_puts(" of ");
		console_putdec((unsigned long)secondaries);
		console_puts(" secondary harts reported running\r\n");

		for (i = 0; i < HSM_MAX_HARTS; i++) {
			if (hsm_harts[i].started == 0 || i == hartid)
				continue;
			if (hsm_wait_for_hart_ticks(hartid, i, 1, HSM_WAIT_TICKS) == 0) {
				console_puts("hsm: hart ");
				console_putdec(i);
				console_puts(" took a timer interrupt with the MMU live\r\n");
			}
		}
	}

	report_harts();

#ifdef SELFTEST_PLIC
	plic_selftest();
	for (;;) {
		__asm__ volatile ("wfi");
	}
#endif

	console_puts("hurd-riscv64: milestone 4 reached, halting\r\n");

	/* Park the hart; the timer is disarmed by the handler's halt path. */
	csr_write_sie(csr_read_sie() & ~(CSR_SIE_STIE | CSR_SIE_SEIE));
	csr_write_sstatus(csr_read_sstatus() & ~CSR_SSTATUS_SIE);
	for (;;) {
		__asm__ volatile ("wfi");
	}
}