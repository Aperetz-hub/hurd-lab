/*
 * Thin wrappers over the RISC-V Supervisor Binary Interface.
 *
 * The calling convention is fixed by the SBI specification (v3.0, section
 * "Binary Encoding"):
 *
 *   a7 = extension ID (EID)
 *   a6 = function ID  (FID)
 *   a0..a5 = arguments
 *
 * and, for any extension defined in SBI v0.2 or later:
 *
 *   "SBI functions must return a pair of values in a0 and a1, with a0
 *    returning an error code.  This is analogous to returning the C
 *    structure
 *
 *        struct sbiret { long error; union { long value; unsigned long uvalue; }; };"
 *
 * So a0 is the error code (0 == SBI_SUCCESS) and a1 carries the queried
 * value.  Reading a0 alone is what made this skeleton report "SBI spec
 * version 0.0" and "SBI impl id 0x0": the calls were succeeding and the
 * answers were arriving in a1, which the old wrapper threw away.
 *
 * The legacy extensions (EID 0x00-0x0F) are the exception: the
 * specification states that "nothing is returned in a1" and that "the value
 * returned in a0 register is SBI legacy extension specific".  The legacy
 * console is therefore handled by its own wrapper, which reads a0.
 */

#ifndef SBI_H
#define SBI_H

#include <stdint.h>

/* The SBI return pair, as the specification defines it. */
struct sbiret {
	long error; /* a0: 0 == SBI_SUCCESS, negative errno otherwise */
	long value; /* a1: the queried value */
};

/* Extension IDs. */
#define SBI_EID_BASE 0x10UL
#define SBI_EID_LEGACY_CONSOLE 0x01UL
#define SBI_EID_TIME 0x54494D45UL
/*
 * HSM, the Hart State Management extension.  The identifier is the three
 * ASCII characters "HSM" packed big-endian, as the specification defines it
 * (the same rule gives "TIME" = 0x54494D45 above).
 */
#define SBI_EID_HSM 0x48534DUL

/* Base extension function IDs. */
#define SBI_FID_BASE_GET_SPEC_VERSION 0UL
#define SBI_FID_BASE_GET_IMPL_ID 1UL
#define SBI_FID_BASE_GET_IMPL_VERSION 2UL
#define SBI_FID_BASE_PROBE_EXTENSION 3UL

/* TIME extension function IDs. */
#define SBI_FID_TIME_GET_TIME 0UL
#define SBI_FID_TIME_GET_TIMEBASE 1UL
#define SBI_FID_TIME_SET_STIMECMP 7UL

/* HSM extension function IDs. */
#define SBI_FID_HSM_START 0UL
#define SBI_FID_HSM_STOP 1UL
#define SBI_FID_HSM_GET_STATUS 2UL
#define SBI_FID_HSM_SET_SUSPEND_STATE 3UL

/* Legacy console function IDs. */
#define SBI_FID_LEGACY_CONSOLE_PUTCHAR 0UL

/* The "extension is not implemented" error the specification mandates. */
#define SBI_ERR_NOT_SUPPORTED (-2L)

/*
 * Issue one SBI `ecall` and return both result registers.
 *
 * The specification allows a0..a5 to carry arguments, so the wrapper takes
 * three of them; sbi_hart_start() is the call that needs more than one.
 *
 * Traps into M-mode firmware; only valid once the firmware has handed
 * control to S-mode, i.e. never before `_start` has run.
 */
struct sbiret sbi_ecall(unsigned long eid, unsigned long fid,
			unsigned long arg0, unsigned long arg1, unsigned long arg2);

/* Base extension. */
struct sbiret sbi_get_spec_version(void);
struct sbiret sbi_get_impl_id(void);
struct sbiret sbi_get_impl_version(void);
struct sbiret sbi_probe_extension(unsigned long eid);

/* TIME extension. */
struct sbiret sbi_timer_time(void);
struct sbiret sbi_timer_timebase(void);
struct sbiret sbi_timer_set_stimecmp(unsigned long deadline);

/*
 * HSM extension.
 *
 * sbi_hart_start() releases a hart the firmware is holding at its S-mode entry
 * point, passing `opaque` in a0 and the device tree address in a1 -- the same
 * protocol the firmware uses to call the boot hart, so the same trampoline
 * serves both.
 *
 * sbi_hart_get_status() returns the hart's state in `value`.  Which number
 * means what depends on the specification version the firmware follows; see
 * include/hsm.h, where the encoding is established by measurement rather than
 * assumed.
 */
struct sbiret sbi_hart_start(unsigned long hartid, unsigned long start_addr,
			     unsigned long opaque);
struct sbiret sbi_hart_stop(unsigned long hartid);
struct sbiret sbi_hart_get_status(unsigned long hartid);
struct sbiret sbi_hart_set_suspend_state(unsigned long hartid,
					 unsigned long suspend_type);

/*
 * Write one byte to the firmware console through the legacy console
 * extension. Returns 1 when the firmware accepted the call.
 *
 * The legacy extension returns its result in a0, not in the a0/a1 pair, so
 * this one deliberately does not go through sbi_ecall().
 */
int sbi_legacy_console_putc(char byte);

#endif /* SBI_H */