/* Test-only glue: registration tables for the virtio PCI front-end.
 *
 * NetBSD 2025-01 has no `device virtio_pci' in its config database, so
 * config(1) cannot emit cfdriver_ioconf_virtio_pci et al. (Debian works
 * around this by disabling virtio rump entirely: "Fix virtio later").
 * This file provides the missing tables by hand, mirroring the shapes
 * config generates in ioconf.c for normal drivers.
 */

#include <sys/param.h>
#include <sys/device.h>

/* Defined by CFDRIVER_DECL(virtio, ...) in ioconf.c. */
extern struct cfdriver virtio_cd;
/* Defined by CFATTACH_DECL3_NEW(virtio_pci, ...) in virtio_pci.c. */
extern struct cfattach virtio_pci_ca;

/* locators: dev ?, function ? (same as ioconf.c pspec0). */
static int loc[2] = {
	-1, -1,
};

static const struct cfparent pspec0 = {
	"pci", "pci", DVUNIT_ANY
};

static struct cfdata cfdata_ioconf_virtio_pci[] = {
    /* driver           attachment    unit state      loc   flags  pspec */
/*  0: virtio* at pci? dev -1 function -1 */
    { "virtio",		"virtio_pci",	 0, FSTATE_STAR, loc+  0,      0, &pspec0 },
    { NULL,		NULL,		 0,    0,    NULL,      0, NULL }
};

static struct cfattach * const virtio_pci_cfattachinit[] = {
	&virtio_pci_ca, NULL
};

static const struct cfattachinit cfattach_ioconf_virtio_pci[] = {
	{ "virtio_pci", virtio_pci_cfattachinit },
	{ NULL, NULL }
};

struct cfdriver * const cfdriver_ioconf_virtio_pci[] = {
	&virtio_cd, NULL
};
