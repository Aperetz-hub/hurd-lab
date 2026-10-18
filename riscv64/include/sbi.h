/*
 * Thin wrappers over the RISC-V Supervisor Binary Interface.
 *
 * The calling convention is fixed by the SBI specification and is not the same
 * as the RISC-V standard calling convention, which is why these are issued as
 * raw `ecall`s from inline assembly instead of normal function calls:
 *
 *   a7 = extension ID
 *   a6 = function ID
 *   a0..a5 = arguments, a0/a1 = return value
 *
 * `sbi_ecall()` returns both return registers so callers can tell an SBI error
 * (a negative errno in a0) from a real result, and can see the extension-level
 * code (a1, -1 when the extension is absent).
 *
 * Only `ecall` is used. The time/dbase/IPI extensions are deliberately not
 * wired yet: they need the device tree to find the timer, which is the first
 * real milestone after this skeleton.
 */

#ifndef SBI_H
#define SBI_H

/* Supervisor call extension: the base `legacy` extension set. */
#define SBI_SYSCALL 0x10UL

/* `sbi_get_spec_version` -- spec version encoded as (major << 24) | minor. */
#define SBI_FN_GET_SPEC_VERSION 0UL

/* `sbi_get_impl_id` -- firmware implementation identifier. */
#define SBI_FN_GET_IMPL_ID 0x01UL

/* `SBI_EXT_LEGACY_CONSOLE` -- the pre-0.2 console extension. */
#define SBI_EXT_LEGACY_CONSOLE 0x01UL

/* `legacy_putch`: a0 = character to emit. */
#define SBI_FN_LEGACY_PUTCH 0x00UL

struct sbi_result {
	long a0; /* result, or a negative errno */
	long a1; /* extension-level code, -1 when absent */
};

/*
 * Issue one SBI `ecall`.
 *
 * Traps into M-mode firmware; only valid once OpenSBI has handed control to
 * S-mode, i.e. never before `_start` has run.
 */
struct sbi_result sbi_ecall(unsigned long eid, unsigned long fid,
			    unsigned long arg0, unsigned long arg1);

/*
 * Write one byte to the firmware console. Returns 1 when the firmware accepted
 * the call (a0 == SBI_ERR_NO_ERROR), 0 otherwise.
 *
 * This is the only output path that works before any device tree parsing,
 * which is why it is the fallback of last resort in console.c.
 */
int sbi_console_putc(char byte);

/*
 * Read the SBI specification version, encoded as (major << 24) | minor.
 * OpenSBI implements this, so a negative return is an error.
 */
long sbi_spec_version(void);

/*
 * Read the firmware implementation identifier. OpenSBI returns a fixed vendor
 * value; the legacy SBI does not define a stable meaning, so treat it as
 * informational.
 */
long sbi_impl_id(void);

#endif /* SBI_H */