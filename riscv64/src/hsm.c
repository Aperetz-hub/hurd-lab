/*
 * Secondary hart bring-up through the SBI HSM extension.  See include/hsm.h for
 * the protocol, for why only the boot hart prints, and for how the status
 * numbering is established by measurement.
 */

#include "hsm.h"

#include "console.h"
#include "csr.h"
#include "pmm.h"
#include "sbi.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

/*
 * The entry point the firmware jumps to, defined in src/boot.S.  It needs a
 * stack before it can call anything, so it indexes hsm_stack_top directly.
 */
extern char secondary_start[];

unsigned long hsm_stack_top[HSM_MAX_HARTS];

struct hsm_hart hsm_harts[HSM_MAX_HARTS];

/*
 * The `satp` value the boot hart publishes to the secondaries, plus how many
 * candidate harts the firmware actually acknowledged.
 */
static unsigned long published_satp;
static unsigned long probed_harts;

/* What the firmware's status numbers turned out to mean. */
static int status_encoding = HSM_ENCODE_UNKNOWN;

/* The slot for a hart id, or NULL when the id is out of range. */
static struct hsm_hart *
hsm_slot(unsigned long hartid)
{
	if (hartid >= HSM_MAX_HARTS)
		return NULL;

	return &hsm_harts[hartid];
}

static void
hsm_publish_satp(unsigned long satp_value)
{
	/*
	 * Release: everything the secondary reads after this has to be visible
	 * to it once it starts.  The SBI ecall that releases the hart is a
	 * second barrier, but the ordering is stated here rather than assumed.
	 */
	__atomic_store_n(&published_satp, satp_value, __ATOMIC_RELEASE);
}

static unsigned long
hsm_published_satp(void)
{
	return __atomic_load_n(&published_satp, __ATOMIC_ACQUIRE);
}

/*
 * The boot hart is, by definition, the hart that is running, so the state it
 * reports for itself is the firmware's STARTED value.  Comparing that against
 * the two encodings the specification has used settles which one applies,
 * instead of guessing -- and guessing wrong means either starting nothing or
 * starting every hart twice.
 */
int
hsm_learn_status_encoding(unsigned long boot_hartid)
{
	struct sbiret r = sbi_hart_get_status(boot_hartid);
	unsigned long raw;

	if (r.error != 0) {
		console_puts("hsm: sbi_hart_get_status(");
		console_putdec(boot_hartid);
		console_puts(") failed with error ");
		console_putdec_signed(r.error);
		console_puts("\r\n");
		status_encoding = HSM_ENCODE_UNKNOWN;
		return status_encoding;
	}

	raw = (unsigned long)r.value;
	status_encoding = HSM_ENCODE_UNKNOWN;
	if (raw == HSM_STATUS_V3_STARTED)
		status_encoding = HSM_ENCODE_SPEC_V3;
	else if (raw == HSM_STATUS_V02_STARTED)
		status_encoding = HSM_ENCODE_SPEC_V02;

	console_puts("hsm: status of the boot hart ");
	console_putdec(boot_hartid);
	console_puts(" is ");
	console_putdec(raw);
	console_puts(", so the firmware uses ");
	switch (status_encoding) {
	case HSM_ENCODE_SPEC_V3:
		console_puts("the SBI v3.0 encoding (STARTED=0, STOPPED=1)");
		break;
	case HSM_ENCODE_SPEC_V02:
		console_puts("the SBI v0.2 encoding (STOPPED=0, STARTED=1)");
		break;
	default:
		console_puts("neither known encoding; refusing to start harts");
		break;
	}
	console_puts("\r\n");

	return status_encoding;
}

int
hsm_status_is_started(unsigned long raw)
{
	switch (status_encoding) {
	case HSM_ENCODE_SPEC_V3:
		return raw == HSM_STATUS_V3_STARTED;
	case HSM_ENCODE_SPEC_V02:
		return raw == HSM_STATUS_V02_STARTED;
	default:
		return 0;
	}
}

int
hsm_status_is_stopped(unsigned long raw)
{
	switch (status_encoding) {
	case HSM_ENCODE_SPEC_V3:
		return raw == HSM_STATUS_V3_STOPPED;
	case HSM_ENCODE_SPEC_V02:
		return raw == HSM_STATUS_V02_STOPPED;
	default:
		return 0;
	}
}

/*
 * Give a secondary a stack and release it.
 *
 * The stack is one page from the physical pool, which the page tables already
 * map writable, so it is usable both before and after the secondary switches
 * the MMU on.  Its top is published with a release store, because the secondary
 * picks it up with an acquire load in assembly.
 */
static int
release_hart(unsigned long hartid)
{
	struct hsm_hart *slot = hsm_slot(hartid);
	struct sbiret r;
	void *page;

	if (slot == NULL) {
		console_puts("hsm: hart ");
		console_putdec(hartid);
		console_puts(" is past the ");
		console_putdec(HSM_MAX_HARTS);
		console_puts(" slots the kernel keeps; not started\r\n");
		return -1;
	}

	page = pmm_alloc_page();
	if (page == NULL) {
		console_puts("hsm: no page left in the pool for the stack of hart ");
		console_putdec(hartid);
		console_puts("\r\n");
		__atomic_store_n(&slot->error, (unsigned long)HSM_ERR_NO_STACK,
				 __ATOMIC_RELEASE);
		return -1;
	}

	/* The stack grows down, so its top is the end of the page. */
	slot->stack_top = (unsigned long)page + PMM_PAGE_SIZE;
	__atomic_store_n(&hsm_stack_top[hartid], slot->stack_top, __ATOMIC_RELEASE);

	r = sbi_hart_start(hartid, (unsigned long)secondary_start, hartid);
	if (r.error != 0) {
		console_puts("hsm: sbi_hart_start(");
		console_putdec(hartid);
		console_puts(") failed with error ");
		console_putdec_signed(r.error);
		console_puts("\r\n");
		__atomic_store_n(&hsm_stack_top[hartid], 0UL, __ATOMIC_RELEASE);
		__atomic_store_n(&slot->error, (unsigned long)HSM_ERR_START_FAILED,
				 __ATOMIC_RELEASE);
		return -1;
	}

	__atomic_store_n(&slot->started, 1UL, __ATOMIC_RELEASE);

	console_puts("hsm: released hart ");
	console_putdec(hartid);
	console_puts(" at ");
	console_puthex((unsigned long)secondary_start);
	console_puts(", stack top ");
	console_puthex(slot->stack_top);
	console_puts("\r\n");

	return 0;
}

/* Ask the firmware about every candidate id and report what it says. */
static void
census(unsigned long boot_hartid)
{
	unsigned long id;

	for (id = 0; id < HSM_PROBE_LIMIT; id++) {
		struct sbiret r = sbi_hart_get_status(id);
		struct hsm_hart *slot;

		/*
		 * The firmware answers with an error once the id is past the last
		 * hart.  That is the end of the list, not a failure.
		 */
		if (r.error != 0) {
			console_puts("hsm: hart ");
			console_putdec(id);
			console_puts(" does not exist (error ");
			console_putdec_signed(r.error);
			console_puts("), so the board has ");
			console_putdec(id);
			console_puts(" harts\r\n");
			break;
		}

		slot = hsm_slot(id);
		if (slot != NULL)
			slot->status = (unsigned long)r.value;

		console_puts("hsm: hart ");
		console_putdec(id);
		console_puts(" status ");
		console_putdec((unsigned long)r.value);
		console_puts(" (");
		if (hsm_status_is_started((unsigned long)r.value))
			console_puts("started");
		else if (hsm_status_is_stopped((unsigned long)r.value))
			console_puts("stopped");
		else
			console_puts("unknown");
		if (id == boot_hartid)
			console_puts(", this is the boot hart");
		console_puts(")\r\n");

		probed_harts++;
	}
}

int
hsm_start_secondaries(unsigned long boot_hartid)
{
	struct sbiret probe;
	unsigned long id;
	int released = 0;

	probe = sbi_probe_extension(SBI_EID_HSM);
	if (probe.error != 0 || (unsigned long)probe.value == 0) {
		console_puts("hsm: extension 0x");
		console_puthex(SBI_EID_HSM);
		console_puts(" is not available (probe error ");
		console_putdec_signed(probe.error);
		console_puts(", value ");
		console_putdec((unsigned long)probe.value);
		console_puts("); staying on one hart\r\n");
		return -1;
	}

	console_puts("hsm: extension probe 0x");
	console_puthex(SBI_EID_HSM);
	console_puts(" -> available\r\n");

	if (hsm_learn_status_encoding(boot_hartid) == HSM_ENCODE_UNKNOWN)
		return -1;

	census(boot_hartid);

	/* Share the page table the boot hart installed before anybody starts. */
	hsm_publish_satp(vm_shared_satp());
	console_puts("hsm: published satp ");
	console_puthex(hsm_published_satp());
	console_puts(", shared by every hart\r\n");

	for (id = 0; id < probed_harts; id++) {
		struct hsm_hart *slot = hsm_slot(id);

		if (slot == NULL)
			continue;
		/* The boot hart is already running. */
		if (id == boot_hartid)
			continue;
		if (!hsm_status_is_stopped(slot->status))
			continue;
		if (release_hart(id) == 0)
			released++;
	}

	console_puts("hsm: released ");
	console_putdec((unsigned long)released);
	console_puts(" secondary harts\r\n");

	return released;
}

/*
 * Wait until `expected` secondaries have reported running, or the boot hart's
 * own tick budget runs out.
 *
 * The count is compared against the number that was actually released, not
 * against "at least one": with several secondaries, returning as soon as the
 * first one is up would report the others as missing when they were merely
 * slower.
 */
unsigned long
hsm_wait_for_secondaries(unsigned long boot_hartid, unsigned long expected,
			       unsigned long timeout_ticks)
{
	unsigned long started = timer_tick_count(boot_hartid);

	for (;;) {
		unsigned long i;
		unsigned long up = 0;

		/*
		 * Only the harts this call released.  The boot hart has its
		 * 'running' flag set as well -- kmain sets it so that report_harts()
		 * can describe every hart of the board in one shape of line -- so
		 * counting every running hart would count the boot hart too, and
		 * report one more than was ever expected.
		 */
		for (i = 0; i < HSM_MAX_HARTS; i++) {
			if (i == boot_hartid)
				continue;
			if (__atomic_load_n(&hsm_harts[i].running, __ATOMIC_ACQUIRE))
				up++;
		}

		if (up >= expected ||
		    timer_tick_count(boot_hartid) >= started + timeout_ticks)
			return up;

		__asm__ volatile ("wfi");
	}
}

/*
 * Wait until one particular hart has taken `ticks` timer interrupts with the MMU
 * live.  This is the interesting half of the multi-hart result: a secondary that
 * merely got as far as installing `satp` and saying so proves nothing about the
 * shared mapping, whereas one that has taken a timer interrupt on it has walked
 * those tables and come back.
 */
int
hsm_wait_for_hart_ticks(unsigned long boot_hartid, unsigned long hartid,
			unsigned long ticks, unsigned long timeout_ticks)
{
	unsigned long started = timer_tick_count(boot_hartid);

	for (;;) {
		if (timer_tick_count_mmu(hartid) >= ticks)
			return 0;

		if (timer_tick_count(boot_hartid) >= started + timeout_ticks)
			return -1;

		__asm__ volatile ("wfi");
	}
}

/*
 * The secondary hart's whole job.  Nothing here writes to the console; see
 * include/hsm.h for why.
 */
void
hsm_secondary_main(unsigned long hartid)
{
	struct hsm_hart *slot;
	unsigned long satp_value;

	if (hartid >= HSM_MAX_HARTS) {
		/*
		 * No slot to record anything in.  Park.  The boot hart notices,
		 * because this hart's slot stays zero.
		 */
		for (;;) {
			__asm__ volatile ("wfi");
		}
	}

	slot = &hsm_harts[hartid];
	__atomic_store_n(&slot->entered, 1UL, __ATOMIC_RELEASE);

	/*
	 * The boot hart publishes the page table before releasing anybody, so
	 * this load cannot observe a half written value.
	 */
	satp_value = hsm_published_satp();
	if ((satp_value >> 60) != 8) {
		__atomic_store_n(&slot->error, (unsigned long)HSM_ERR_NO_ROOT_TABLE,
				 __ATOMIC_RELEASE);
		goto parked;
	}

	/*
	 * The trap vector goes in before the MMU: a fault while the secondary is
	 * still walking the tables would otherwise have nowhere to land.
	 */
	csr_write_stvec((unsigned long)trap_entry);

	/*
	 * This hart's own tick.  `stimecmp` is written directly rather than
	 * through the SBI TIME extension: the rearm happens in interrupt
	 * context, where an ecall into the firmware is the wrong thing, and a
	 * hart that has only just been released should not depend on the
	 * firmware still servicing calls from it.
	 */
	timer_arm(hartid, TIMER_INTERVAL_TICKS, TIMER_ARM_SECONDARY);

	/* The shared Sv39 mapping, then the TLB flush that must accompany it. */
	csr_write_satp(satp_value);
	__asm__ volatile ("sfence.vma" ::: "memory");
	__atomic_store_n(&slot->satp, satp_value, __ATOMIC_RELEASE);

	/* STIE plus the global S-mode enable. */
	csr_enable_interrupts();

	__atomic_store_n(&slot->running, 1UL, __ATOMIC_RELEASE);

parked:
	for (;;) {
		__asm__ volatile ("wfi");
	}
}