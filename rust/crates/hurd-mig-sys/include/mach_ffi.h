/*
 * mach_ffi.h -- umbrella header for bindgen.
 *
 * Purpose: give bindgen ONE stable, self-contained entry point instead of
 * making it grovel through the whole GNU Mach header tree (which pulls in
 * <sys/types.h> ordering constraints, config headers and a couple of GNU
 * extensions that trip modern clang).
 *
 * Strategy:
 *   - If the real Hurd headers are reachable (HURD_INCLUDE, or a Debian
 *     /usr/include/i386-gnu/hurd, or a source checkout), include them so
 *     bindgen sees the authoritative declarations and the authoritative
 *     struct layouts.
 *   - Otherwise fall back to the minimal, layout-compatible subset below, so
 *     the crate always builds (useful on CI and for a first `cargo check`).
 *
 * Layout compatibility is not a coincidence: on x86_64 both `natural_t` and
 * `integer_t` follow the LP64 model (int = 32-bit, long = 64-bit), and the
 * hand-written types below mirror that. bindgen is generated against the same
 * `-m64` layout the C side uses; if you ever change the C ABI flags, change
 * rust/.cargo/config.toml in the same commit or every offset will silently rot.
 *
 * What is NOT here on purpose: anything MIG-generated. MIG output is not
 * installed as headers by default, so the build script only pulls it in when
 * MIG_DEFS points at a directory that contains it. See README.md,
 * "Parsing MIG .defs files".
 */

#ifndef HURD_MIG_SYS_MACH_FFI_H
#define HURD_MIG_SYS_MACH_FFI_H

/* ------------------------------------------------------------------ */
/* Preferred path: the real headers, when they are available.          */
/* ------------------------------------------------------------------ */

#if defined(HAVE_HURD_HEADERS)
#  if __has_include(<mach/mach_types.h>)
#    include <mach/mach_types.h>
#  endif
#  if __has_include(<mach/mach.h>)
#    include <mach/mach.h>
#  endif
#  if __has_include(<mach/message.h>)
#    include <mach/message.h>
#  endif
#  if __has_include(<mach/port.h>)
#    include <mach/port.h>
#  endif
#  if __has_include(<mach/mach_traps.h>)
#    include <mach/mach_traps.h>
#  endif
#  if __has_include(<mach/task_self.h>)
#    include <mach/task_self.h>
#  endif
#  if __has_include(<mach/mig_errors.h>)
#    include <mach/mig_errors.h>
#  endif
/* MIG-generated user/server side headers, only if the build script found
 * them (HAVE_MIG_HEADERS). */
#  if defined(HAVE_MIG_HEADERS)
#    if __has_include(<mach/server.h>)
#      include <mach/server.h>
#    endif
#    if __has_include(<mach/mach_port.h>)
#      include <mach/mach_port.h>
#    endif
#  endif
#endif /* HAVE_HURD_HEADERS */

/* ------------------------------------------------------------------ */
/* Fallback: a minimal, layout-compatible subset.                      */
/*                                                                     */
/* This is what bindgen sees on a machine without the Hurd headers.     */
/* It is deliberately narrow: it covers only the entry points the      */
/* proof-of-concept actually calls.                                    */
/* ------------------------------------------------------------------ */

#if !defined(HAVE_HURD_HEADERS)

#include <stdint.h>

typedef unsigned int   mach_msg_size_t;
typedef unsigned int   mach_msg_id_t;
typedef unsigned int   kern_return_t;
typedef int            integer_t;
typedef unsigned int   natural_t;
typedef int            boolean_t;
typedef char           *vm_offset_t;
typedef unsigned long  vm_size_t;
typedef integer_t      mach_port_t;
typedef mach_port_t    task_t;
typedef mach_port_t    thread_t;
typedef mach_port_t    vm_map_t;
typedef natural_t      mach_port_name_t;
typedef natural_t      mach_msg_timeout_t;
typedef integer_t      mach_msg_option_t;

/* Return codes used by the POC. */
#define KERN_SUCCESS                   ((kern_return_t) 0)
#define KERN_INVALID_ADDRESS           ((kern_return_t) 1)
#define KERN_PROTECTION_FAILURE        ((kern_return_t) 2)
#define KERN_NO_SPACE                  ((kern_return_t) 3)
#define KERN_INVALID_ARGUMENT          ((kern_return_t) 4)
#define KERN_FAILURE                   ((kern_return_t) 5)
#define KERN_RESOURCE_SHORTAGE         ((kern_return_t) 6)
#define KERN_NOT_RECEIVER              ((kern_return_t) 7)
#define KERN_NO_ACCESS                 ((kern_return_t) 8)
#define KERN_MEMORY_FAILURE            ((kern_return_t) 9)
#define KERN_MEMORY_ERROR              ((kern_return_t) 10)
#define KERN_ALREADY_IN_SET            ((kern_return_t) 11)
#define KERN_NOT_IN_SET                ((kern_return_t) 12)
#define KERN_NAME_EXISTS               ((kern_return_t) 13)
#define KERN_ABORTED                   ((kern_return_t) 14)
#define KERN_INVALID_NAME              ((kern_return_t) 15)
#define KERN_INVALID_TASK              ((kern_return_t) 16)
#define KERN_INVALID_RIGHT             ((kern_return_t) 17)
#define KERN_INVALID_VALUE             ((kern_return_t) 18)
#define KERN_UREFS_OVERFLOW            ((kern_return_t) 19)
#define KERN_INVALID_CAPABILITY        ((kern_return_t) 20)
#define KERN_RIGHT_EXISTS              ((kern_return_t) 21)
#define KERN_INVALID_HOST              ((kern_return_t) 22)
#define KERN_MEMORY_PRESENT            ((kern_return_t) 23)
#define KERN_DEFAULT_SET               ((kern_return_t) 24)
#define KERN_INFO_TOO_LARGE            ((kern_return_t) 25)

/* Message header, laid out exactly as GNU Mach's <mach/message.h>. */
typedef struct {
    mach_msg_size_t msgh_size;
    mach_port_t     msgh_remote_port;
    mach_port_t     msgh_local_port;
    natural_t       msgh_bits;
    mach_msg_size_t msgh_size_rounded;
    mach_port_t     msgh_id;
} mach_msg_header_t;

/* Only the subset of mach_port rights the POC names. */
#define MACH_MSG_TYPE_MOVE_SEND        0x00000001
#define MACH_MSG_TYPE_MOVE_SEND_ONCE   0x00000002
#define MACH_MSG_TYPE_MOVE_RECEIVE     0x00000004
#define MACH_MSG_TYPE_COPY_SEND        0x00000008
#define MACH_MSG_TYPE_COPY_RECEIVE     0x00000010
#define MACH_MSG_TYPE_SEND             0x00000020
#define MACH_MSG_TYPE_RECEIVE          0x00000040
#define MACH_MSG_TYPE_SEND_ONCE        0x00000080

/* mach_msg() options and return codes (subset). */
#define MACH_MSG_OPTION_NONE               0x00000000
#define MACH_SEND_MSG                      0x00000001
#define MACH_RCV_MSG                       0x00000002
#define MACH_SEND_TIMEOUT                  0x00000010
#define MACH_RCV_TIMEOUT                   0x00000020
#define MACH_SEND_INTERRUPT                0x00000040
#define MACH_SEND_TIMEOUT_ERROR            ((mach_msg_size_t) 0x00001000)
#define MACH_RCV_TIMED_OUT                  ((mach_msg_size_t) 0x00002000)
#define MACH_RCV_TOO_LARGE                 ((mach_msg_size_t) 0x00004000)
#define MACH_RCV_INTERRUPTED               ((mach_msg_size_t) 0x00004001)
#define MACH_RCV_PORT_DIED                 ((mach_msg_size_t) 0x00004002)
#define MACH_RCV_PORT_CHANGED              ((mach_msg_size_t) 0x00004003)
#define MACH_SEND_INVALID_DATA             ((mach_msg_size_t) 0x00008001)
#define MACH_SEND_INVALID_DEST             ((mach_msg_size_t) 0x00008002)
#define MACH_SEND_TIMED_OUT                ((mach_msg_size_t) 0x00008003)
#define MACH_SEND_INTERRUPTED              ((mach_msg_size_t) 0x00008004)

/* MIG subsystem identifiers / the two entry points the POC needs. */
extern kern_return_t mig_allocate(vm_map_t, vm_size_t, vm_offset_t *);
extern kern_return_t mig_deallocate(vm_map_t, vm_offset_t, vm_size_t);

extern kern_return_t mach_port_allocate(task_t, mach_port_t, mach_port_t *);
extern kern_return_t mach_port_deallocate(task_t, mach_port_t);
extern kern_return_t mach_port_insert_right(task_t, mach_port_t, mach_port_t, mach_port_t);

extern kern_return_t mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
                              mach_msg_size_t send_size, mach_msg_size_t rcv_size,
                              mach_port_name_t rcv_name, mach_msg_timeout_t timeout,
                              mach_port_t notify);

extern kern_return_t mach_error_string(kern_return_t, char *, mach_msg_size_t);

extern mach_port_t mach_task_self(void);
extern mach_port_t mach_host_self(void);

extern const char *mach_port_type(mach_port_t, char *);

#endif /* !HAVE_HURD_HEADERS */

#endif /* HURD_MIG_SYS_MACH_FFI_H */