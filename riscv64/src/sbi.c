/*
 * SBI `ecall` wrappers. See include/sbi.h for the calling convention.
 */

#include "sbi.h"

struct sbi_result
sbi_ecall(unsigned long eid, unsigned long fid, unsigned long arg0,
	   unsigned long arg1)
{
	struct sbi_result r;

	/*
	 * Bind the SBI argument registers explicitly. "+r" on a0/a1 gives the
	 * inputs and captures the outputs; "r" on a6/a7 are the firmware's
	 * function and extension selectors. The "memory" clobber keeps the
	 * compiler from moving memory accesses across the trap.
	 */
	register unsigned long a0 __asm__("a0") = arg0;
	register unsigned long a1 __asm__("a1") = arg1;
	register unsigned long a6 __asm__("a6") = fid;
	register unsigned long a7 __asm__("a7") = eid;

	__asm__ volatile ("ecall"
			  : "+r" (a0), "+r" (a1)
			  : "r" (a6), "r" (a7)
			  : "memory");

	r.a0 = (long)a0;
	r.a1 = (long)a1;
	return r;
}

int
sbi_console_putc(char byte)
{
	struct sbi_result r;

	r = sbi_ecall(SBI_SYSCALL, SBI_FN_LEGACY_PUTCH, (unsigned long)byte,
		      SBI_EXT_LEGACY_CONSOLE);
	/* SBI_ERR_NO_ERROR is 0; anything else is -EIO / -ENOSYS / ... per spec. */
	return r.a0 == 0;
}

long
sbi_spec_version(void)
{
	return sbi_ecall(SBI_SYSCALL, SBI_FN_GET_SPEC_VERSION, 0, 0).a0;
}

long
sbi_impl_id(void)
{
	return sbi_ecall(SBI_SYSCALL, SBI_FN_GET_IMPL_ID, 0, 0).a0;
}