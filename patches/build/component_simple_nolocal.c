/* Stripped copy of NetBSD sys/rump/dev/component_simple.c for the Hurd
 * virtio bring-up: identical, except it does NOT #include "ioconf.c".
 * The ioconf tables live in the driver object itself (the driver .c
 * #includes ioconf.c when _MODULE is undefined via unmodule.h); including
 * ioconf.c here as well would define every table twice.
 * Registration happens through this RUMP_COMPONENT init function.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: component_simple.c,v 1.3 2016/01/26 23:12:14 pooka Exp $");

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/bus.h>

#include <rump-sys/kern.h>
#include "ioconf.h"
#include "/home/demo/vioif_externs.h"

#define CONFIG_INIT_COMPONENT(a) config_init_component(		\
    __CONCAT(cfdriver_ioconf_,a),					\
    __CONCAT(cfattach_ioconf_,a),					\
    __CONCAT(cfdata_ioconf_,a))

RUMP_COMPONENT(RUMP_COMPONENT_DEV)
{

	CONFIG_INIT_COMPONENT(IOCONF);
}
