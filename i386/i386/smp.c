/* smp.h - i386 SMP controller for Mach
   Copyright (C) 2020 Free Software Foundation, Inc.
   Written by Almudena Garcia Jurado-Centurion

   This file is part of GNU Mach.

   GNU Mach is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GNU Mach is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include <string.h>
#include <i386/apic.h>
#include <i386/smp.h>
#include <i386/cpu.h>
#include <i386/pio.h>
#include <i386/pit.h>
#include <i386/vm_param.h>
#include <i386at/idt.h>
#include <i386at/cram.h>
#include <i386at/acpi_parse_apic.h>
#include <kern/printf.h>
#include <mach/machine.h>

#include <kern/smp.h>

extern char *kernel_cmdline;

/*
 * smp_maxcpus_from_cmdline: option de ligne de commande noyau (multiboot)
 * limitant le nombre de CPU utilises :
 *   nosmp     -> au plus 1 CPU
 *   maxcpus=N -> au plus N CPU (N >= 1)
 * Retourne 0 si aucune limite n'est demandee (defaut : tous les CPU).
 * Permet de demarrer un hote -smp N avec un seul CPU utilisable, en
 * contournement des races SMP encore non resolues.
 */
static uint8_t
smp_maxcpus_from_cmdline(void)
{
    const char *p;
    unsigned n;

    if (strstr(kernel_cmdline, "nosmp") != NULL)
        return 1;

    p = strstr(kernel_cmdline, "maxcpus=");
    if (p != NULL) {
        n = 0;
        p += sizeof("maxcpus=") - 1;
        while (*p >= '0' && *p <= '9')
            n = n * 10 + (unsigned)(*p++ - '0');
        if (n >= 1)
            return (uint8_t) n;
    }
    return 0;
}

/*
 * smp_data_init: initialize smp_data structure
 * Must be called after smp_init(), once all APIC structures
 * has been initialized
 */
static void smp_data_init(void)
{
    uint8_t numcpus = apic_get_numcpus();
    uint8_t maxcpus = smp_maxcpus_from_cmdline();

    if (maxcpus != 0 && maxcpus < numcpus) {
        printf("SMP: maxcpus=%u, limite le noyau a %u CPU sur %u\n",
               maxcpus, maxcpus, numcpus);
        numcpus = maxcpus;
    }

    smp_set_numcpus(numcpus);

    for(int i = 0; i < numcpus; i++){
            machine_slot[i].is_cpu = TRUE;
    }

}

static void smp_send_ipi(unsigned logical_id, unsigned vector)
{
    unsigned long flags;
    int timeout = 100000;

    cpu_intr_save(&flags);

    while (lapic->icr_low.delivery_status == SEND_PENDING) {
        if (--timeout == 0) {
            printf("SMP: IPI delivery stuck, giving up send to %u\n",
                   logical_id);
            break;
        }
        cpu_pause();
    }

    apic_send_ipi(NO_SHORTHAND, FIXED, LOGICAL, ASSERT, EDGE, vector, logical_id);

    cpu_intr_restore(flags);
}

void smp_remote_ast(unsigned logical_id)
{
    smp_send_ipi(logical_id, CALL_AST_CHECK);
}

void smp_pmap_update(unsigned logical_id)
{
    smp_send_ipi(logical_id, CALL_PMAP_UPDATE);
}

/* Bounded wait for IPI delivery. Returns 0 on success, -1 on timeout,
 * so a dead APIC/QEMU SMP config degrades instead of hanging the boot. */
static int
wait_for_ipi(void)
{
    int timeout = 100000;
    while (lapic->icr_low.delivery_status == SEND_PENDING) {
        if (--timeout == 0)
            return -1;
        cpu_pause();
    }
    return 0;
}

static int
smp_send_ipi_init(int bsp_apic_id)
{
    int err;

    lapic->error_status.r = 0;
    err = lapic->error_status.r;

    /* Assert INIT IPI:
     *
     * This is EDGE triggered to match the deassert
     */
    apic_send_ipi(ALL_EXCLUDING_SELF, INIT, PHYSICAL, ASSERT, EDGE, 0, bsp_apic_id);

    /* Wait for delivery */
    if (wait_for_ipi() != 0) {
        printf("ESR error upon INIT assert: delivery timeout\n");
        return -1;
    }

    /* Deassert INIT IPI:
     *
     * NB: This must be an EDGE triggered deassert signal.
     * A LEVEL triggered deassert is only supported on very old hardware
     * that does not support STARTUP IPIs at all, and instead jump
     * via a warm reset vector.
     */
    apic_send_ipi(ALL_EXCLUDING_SELF, INIT, PHYSICAL, DE_ASSERT, EDGE, 0, bsp_apic_id);

    /* Wait for delivery */
    if (wait_for_ipi() != 0) {
        printf("ESR error upon INIT deassert: delivery timeout\n");
        return -1;
    }

    err = lapic->error_status.r;
    if (err) {
        printf("ESR error upon INIT 0x%x\n", err);
    }
    return 0;
}

static int
smp_send_ipi_startup_twice(int bsp_apic_id, int vector)
{
    int i, accept_err, send_err;
    volatile int err;

    for (i = 0; i < 2; i++) {
        lapic->error_status.r = 0;
        err = lapic->error_status.r;
        /* Suppress unused variable warning for the necessary dummy read.*/
        (void) err;

        /* StartUp IPI:
         *
         * Have not seen any documentation for trigger mode for this IPI
         * but it seems to work with EDGE.  (AMD BKDG FAM16h document specifies dont care)
         */
        apic_send_ipi(ALL_EXCLUDING_SELF, STARTUP, PHYSICAL, DE_ASSERT, EDGE, vector, bsp_apic_id);

        pit_udelay(10);

        /* Wait for other cpu to accept IPI */
        if (wait_for_ipi() != 0)
            send_err = -1;
        else
            send_err = lapic->error_status.r;

        pit_udelay(10);

        lapic->error_status.r = 0;
        accept_err = lapic->error_status.r & 0xef;

        if (send_err || accept_err)
            break;
    }

    if (send_err)
        printf("ESR error: DID NOT SEND? 0x%x\n", send_err);
    if (accept_err)
        printf("ESR error: delivery 0x%x\n", accept_err);

    return send_err | accept_err;
}

/* See Intel IA32/64 Software Developer's Manual 3A Section 8.4.4.1 */
int smp_startup_cpus(unsigned bsp_apic_id, phys_addr_t start_eip)
{
    int err;
#if 0
    /* This block goes with a legacy method of INIT that only works with
     * old hardware that does not support SIPIs.
     * Must use INIT DEASSERT LEVEL triggered IPI to use this block.
     * (At least one AMD FCH does not support this IPI mode,
     * See AMD BKDG FAM16h document # 48751 page 461).
     */

    /* Tell CMOS to warm reset through through 40:67 */
    outb(CMOS_ADDR, CMOS_SHUTDOWN);
    outb(CMOS_DATA, CM_JMP_467);

    /* Set warm reset vector to point to AP startup code */
    uint16_t dword[2];
    dword[0] = 0;
    dword[1] = start_eip >> 4;
    memcpy((uint8_t *)phystokv(0x467), dword, 4);
#endif

    /* Local cache flush */
    asm("wbinvd":::"memory");

    printf("Sending IPIs from BSP APIC ID %u...\n", bsp_apic_id);

    err = smp_send_ipi_init(bsp_apic_id);
    if (!err)
        err = smp_send_ipi_startup_twice(bsp_apic_id, start_eip >> STARTUP_VECTOR_SHIFT);
    if (err) {
        /* Do not hang the machine: APs are still experimental, fall back
         * to the BSP so the boot can continue (e.g. QEMU -smp 2). */
        printf("SMP: APs failed to start (err 0x%x), continuing with BSP only\n", err);
        return -1;
    }
    printf("done\n");
    return 0;
}

/*
 * smp_init: initialize the SMP support, starting the cpus searching
 * and enumeration.
 */
int smp_init(void)
{
    smp_data_init();

    return 0;
}
