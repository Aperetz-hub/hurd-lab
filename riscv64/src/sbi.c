/*
 * SBI `ecall` wrappers. See include/sbi.h for the calling convention.
 */

#include "sbi.h"

struct sbiret
sbi_ecall(unsigned long eid, unsigned long fid, unsigned long arg0,
	  unsigned long arg1, unsigned long arg2)
{
	struct sbiret r;

	/*
	 * Bind the SBI argument registers explicitly. "+r" on a0/a1 gives the
	 * inputs and captures the outputs; "r" on a2/a6/a7 are the arguments and
	 * the firmware's function and extension selectors. The "memory" clobber
	 * keeps the compiler from moving memory accesses across the trap.
	 */
	register unsigned long a0 __asm__("a0") = arg0;
	register unsigned long a1 __asm__("a1") = arg1;
	register unsigned long a2 __asm__("a2") = arg2;
	register unsigned long a6 __asm__("a6") = fid;
	register unsigned long a7 __asm__("a7") = eid;

	__asm__ volatile ("ecall"
			  : "+r" (a0), "+r" (a1)
			  : "r" (a2), "r" (a6), "r" (a7)
			  : "memory");

	r.error = (long)a0;
	r.value = (long)a1;
	return r;
}

struct sbiret
sbi_get_spec_version(void)
{
	return sbi_ecall(SBI_EID_BASE, SBI_FID_BASE_GET_SPEC_VERSION, 0, 0, 0);
}

struct sbiret
sbi_get_impl_id(void)
{
	return sbi_ecall(SBI_EID_BASE, SBI_FID_BASE_GET_IMPL_ID, 0, 0, 0);
}

struct sbiret
sbi_get_impl_version(void)
{
	return sbi_ecall(SBI_EID_BASE, SBI_FID_BASE_GET_IMPL_VERSION, 0, 0, 0);
}

struct sbiret
sbi_probe_extension(unsigned long eid)
{
	return sbi_ecall(SBI_EID_BASE, SBI_FID_BASE_PROBE_EXTENSION, eid, 0, 0);
}

struct sbiret
sbi_timer_time(void)
{
	return sbi_ecall(SBI_EID_TIME, SBI_FID_TIME_GET_TIME, 0, 0, 0);
}

struct sbiret
sbi_timer_timebase(void)
{
	return sbi_ecall(SBI_EID_TIME, SBI_FID_TIME_GET_TIMEBASE, 0, 0, 0);
}

struct sbiret
sbi_timer_set_stimecmp(unsigned long deadline)
{
	return sbi_ecall(SBI_EID_TIME, SBI_FID_TIME_SET_STIMECMP, deadline, 0, 0);
}

struct sbiret
sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque)
{
	return sbi_ecall(SBI_EID_HSM, SBI_FID_HSM_START, hartid, start_addr,
			 opaque);
}

struct sbiret
sbi_hart_stop(unsigned long hartid)
{
	return sbi_ecall(SBI_EID_HSM, SBI_FID_HSM_STOP, hartid, 0, 0);
}

struct sbiret
sbi_hart_get_status(unsigned long hartid)
{
	return sbi_ecall(SBI_EID_HSM, SBI_FID_HSM_GET_STATUS, hartid, 0, 0);
}

struct sbiret
sbi_hart_set_suspend_state(unsigned long hartid, unsigned long suspend_type)
{
	return sbi_ecall(SBI_EID_HSM, SBI_FID_HSM_SET_SUSPEND_STATE, hartid,
			 suspend_type, 0);
}

int
sbi_legacy_console_putc(char byte)
{
	register unsigned long a0 __asm__("a0") = (unsigned long)byte;
	register unsigned long a1 __asm__("a1") = 0;
	register unsigned long a6 __asm__("a6") = SBI_FID_LEGACY_CONSOLE_PUTCHAR;
	register unsigned long a7 __asm__("a7") = SBI_EID_LEGACY_CONSOLE;

	/*
	 * The legacy extensions ignore a6 and return their result in a0; a1 is
	 * not used as an output register here.
	 */
	__asm__ volatile ("ecall"
			  : "+r" (a0)
			  : "r" (a1), "r" (a6), "r" (a7)
			  : "memory");

	return a0 == 0;
}