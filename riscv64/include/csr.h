/*
 * Supervisor CSR accessors.
 *
 * The cross toolchain in use is a Linux-target compiler and ships no bare
 * metal CSR header, so the CSRs are named symbolically in the assembler
 * instead.  GNU as resolves the names (sstatus, sie, stvec, sepc, scause,
 * stval, sscratch, stimecmp, time, ...) and the assembler rejects the file if
 * a name does not exist, which is a stronger check than a magic number.
 *
 * `stimecmp` is the Sstc (Supervisor Timer Compare) CSR; it only exists if the
 * hart implements the Sstc extension, and it is writable from S-mode only when
 * the firmware has set menvcfg.STCE and delegated STIP through mideleg.
 */

#ifndef CSR_H
#define CSR_H

#define CSR_SSTATUS_SIE (1UL << 1)

#define CSR_SIE_STIE (1UL << 5)

/* Supervisor trap causes. */
#define SUPERVISOR_TIMER_INTERRUPT 5

static inline unsigned long
csr_read_sstatus(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, sstatus" : "=r" (v));
	return v;
}

static inline void
csr_write_sstatus(unsigned long v)
{
	__asm__ volatile ("csrw sstatus, %0" ::"r" (v));
}

static inline unsigned long
csr_read_sie(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, sie" : "=r" (v));
	return v;
}

static inline void
csr_write_sie(unsigned long v)
{
	__asm__ volatile ("csrw sie, %0" ::"r" (v));
}

static inline unsigned long
csr_read_stvec(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, stvec" : "=r" (v));
	return v;
}

static inline void
csr_write_stvec(unsigned long v)
{
	__asm__ volatile ("csrw stvec, %0" ::"r" (v));
}

static inline unsigned long
csr_read_scratch(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, sscratch" : "=r" (v));
	return v;
}

static inline unsigned long
csr_read_sepc(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, sepc" : "=r" (v));
	return v;
}

static inline unsigned long
csr_read_scause(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, scause" : "=r" (v));
	return v;
}

static inline unsigned long
csr_read_stval(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, stval" : "=r" (v));
	return v;
}

/* The supervisor address translation register. Mode 8 is Sv39. */
static inline unsigned long
csr_read_satp(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, satp" : "=r" (v));
	return v;
}

static inline void
csr_write_satp(unsigned long v)
{
	__asm__ volatile ("csrw satp, %0" ::"r" (v));
}

static inline unsigned long
csr_read_time(void)
{
	unsigned long v;

	__asm__ volatile ("csrr %0, time" : "=r" (v));
	return v;
}

/*
 * NOTE: menvcfg and mideleg are deliberately NOT read here.  Both are
 * machine-mode CSRs, so reading them from S-mode raises an illegal
 * instruction exception.  This was measured on 18/10/2026: reading menvcfg
 * from S-mode trapped with scause=2 (illegal instruction) and stval
 * 0x30a02573, which is the encoding of "csrr t3, menvcfg".  The delegation
 * state therefore has to be established by its effect (does the tick arrive?)
 * rather than by reading the CSR.
 */

/* Sstc: supervisor timer compare. Writing it arms the S-mode timer. */
static inline void
csr_write_stimecmp(unsigned long v)
{
	__asm__ volatile ("csrw stimecmp, %0" ::"r" (v));
}

/* Set the interrupt-enable bit and the global S-mode enable in one go. */
static inline void
csr_enable_interrupts(void)
{
	csr_write_sie(csr_read_sie() | CSR_SIE_STIE);
	csr_write_sstatus(csr_read_sstatus() | CSR_SSTATUS_SIE);
}

#endif /* CSR_H */