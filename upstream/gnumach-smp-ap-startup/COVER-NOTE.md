# Cover note — `smp: do not hang the boot when the APs fail to start` (3 patches)

Target project: **GNU Mach** (`hurd/gnumach` in the GNU Hurd Savannah group).
Upstream repository used: `https://git.savannah.gnu.org/cgit/hurd/gnumach.git`
(mirror actually used for the verification below: `git.sceen.net/hurd/gnumach.git`,
branch `master`, tip `79e98e8b` "Fix copyrights", dated 2026-10-12).

Sending procedure: `git send-email --to=bug-hurd@gnu.org *.patch`, per
<https://www.gnu.org/software/hurd/contributing/developer-workflows.html>
(section "Sending Patches"). That page also recommends a cover letter for a
series (`git format-patch --cover-letter`); this file is that cover letter,
written as plain text to be pasted into patch 0.

## Problem

With a machine that advertises more than one CPU, a GNU Mach kernel built
with `--enable-ncpus > 1` can wedge during boot, before any translator runs.
On QEMU q35 with `-enable-kvm -smp 2` the symptom is a guest that stops at

```
ahcisata0: 64-bit DMA
```

and never gets further: `rumpdisk` is stuck, the clock is frozen.

## Cause

Three separate unbounded waits in the AP bring-up path, plus one destructive
window around them.

1. `start_other_cpus()` called `lapic_disable()` before bringing the APs up
   and `lapic_enable()` afterwards. On the BSP that window is useless
   (`splhigh()` already masks interrupts for its whole duration) and actively
   harmful: clearing `LAPIC_ENABLE` makes the BSP unable to accept *any*
   IOAPIC interrupt, so the 100 Hz PIT edges that arrive during that window
   are consumed without ever being delivered. Since `clock_interrupt()` only
   advances time, the timeout list and `last_hpc_read` on `master_cpu`, the
   system clock then stops for good: no time, no timeout, no preemption. The
   wedge clears only when a vCPU re-entry forces the pending interrupt to be
   re-injected, which is why pausing the guest appeared to "unfreeze" it.
   (patch 1)

2. `wait_for_ipi()` polls `lapic->icr_low.delivery_status` with no bound, and
   `smp_startup_cpus()` reacts to any error by looping in `while (1)`. A
   machine or hypervisor that never delivers the INIT/SIPI IPIs therefore
   hangs the boot with no diagnostic at all. (patch 2)

3. The degradation of patch 2 only fixed a local variable in
   `start_other_cpus()`; `smp_get_numcpus()` still returned the
   APIC-reported count, so the scheduler, the pmap IPIs and ddb kept
   addressing application processors that were never brought up, and the
   boot hung again later on an unacknowledged IPI. (patch 3)

## Change

Patch 1 removes the `lapic_disable()` / `lapic_enable()` window from
`start_other_cpus()` and documents why it must not come back.

Patch 2 bounds every wait: `wait_for_ipi()` returns `-1` on timeout,
`smp_send_ipi_init()` and `smp_send_ipi_startup_twice()` propagate that
error instead of ignoring it, `smp_startup_cpus()` returns `-1` instead of
looping in `while (1)`, and `start_other_cpus()` degrades to BSP-only with a
5 s per-AP bound, naming the AP it gave up on.

Patch 3 calls `smp_set_numcpus()` with the number of CPUs that really
started, so the rest of the kernel only ever refers to live CPUs.

## How it was tested

All commands below were run on this machine (Debian GNU/Linux 13, x86_64,
gcc 14.2.0, GNU MIG built from `hurd/mig` tip `cb48044`).

```
# clean clone of upstream master
git worktree add --detach series-gnumach origin/master      # 79e98e8b

# the three patches, in order, on that clean tree
git apply 0001-smp-do-not-disable-the-BSP-local-APIC-while-starting.patch
git apply 0002-smp-do-not-hang-the-boot-when-the-APs-fail-to-start.patch
git apply 0003-smp-propagate-the-degradation-to-the-global-CPU-coun.patch

# build
autoreconf -i
./configure --host=x86_64-pc-gnu --enable-ncpus=2
make gnumach MIG=$HOME/.local/mig/bin/mig
```

Result: `make exit=0`, artifact `gnumach`, 3624288 bytes,
`ELF 64-bit LSB executable, x86-64, statically linked, with debug_info`.
The warnings emitted during the build are pre-existing ones in `kern/` and
`i386/`; none of them points at the patched hunks.

The kernel was then installed into a **disposable** overlay
(`vm/ovl-up-gnumach.qcow2`, a copy of the fork master overlay; the dev disk
and the fork master overlay were not touched) and booted under
`vm/run-hurd64-smp.sh` with `-m 4096`:

```
SMP=2 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-up-gnumach.qcow2 3
SMP=1 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-up-gnumach.qcow2 2
```

Each round is classified BOOT or WEDGE by reading the console once at the
end of the round, and a BOOT is confirmed over SSH by reading `nproc` and
`md5sum /boot/gnumach-1.8-amd64-up.gz` inside the guest.

```
round smp2 #1 => BOOT
   ssh : 2 c6a153381234fa808dc742a33e798755  /boot/gnumach-1.8-amd64-up.gz
round smp2 #2 => BOOT
   ssh : 2 c6a153381234fa808dc742a33e798755  /boot/gnumach-1.8-amd64-up.gz
round smp2 #3 => BOOT
   ssh : 2 c6a153381234fa808dc742a33e798755  /boot/gnumach-1.8-amd64-up.gz
=== DONE rounds smp2 (ovl-up-gnumach.qcow2) ===
round smp2 #1 => BOOT
   ssh : 1 c6a153381234fa808dc742a33e798755  /boot/gnumach-1.8-amd64-up.gz
round smp2 #2 => BOOT
   ssh : 1 c6a153381234fa808dc742a33e798755  /boot/gnumach-1.8-amd64-up.gz
=== DONE rounds smp2 (ovl-up-gnumach.qcow2) ===
```

`-smp 2`: **3/3 BOOT**, `nproc = 2`. `-smp 1`: **2/2 BOOT**, `nproc = 1`.
The md5 read inside the guest on every BOOT round is the md5 of the
uploaded `.gz`, i.e. the kernel that was actually running is the one built
from the patched upstream tree.

The same harness on the *unpatched* upstream master kernel, same overlay
base, same QEMU line: 0/3 BOOT (all three rounds WEDGE). That is the
before/after pair for patch 1.

The same three patches were measured earlier on the fork branch, where the
fix kernel had already been validated: `-smp 2` went from 0/3 to 12/12 boots
with `nproc=2`, `-smp 1` 3/3.

`maxcpus=1` and `nosmp` were also checked on the unpatched kernel and still
boot (2/2 each), which confirms the degradation path does not regress the
single-CPU configuration.

## Known limits

- **"Fixed" here means "boots and stays usable with two CPUs".** No
  multi-CPU workload has been run against it. The AP exists and runs
  (`nproc=2`), but nothing in this series demonstrates that the Hurd userland
  is SMP-correct, and upstream's amd64 SMP work is known to be in progress.
  Please do not read this series as an SMP enablement.
- The measurements were taken on **one** machine under QEMU with KVM
  (`-M q35`, `-enable-kvm`, `-smp 2`, `-m 4096`) and on one guest
  distribution (Debian GNU/Hurd 0.9). No bare-metal test, no `-smp 4`, no
  non-KVM (TCG) test, no i386 build test.
- The 5 s per-AP bound in patch 2 is a first guess, not a calibrated value.
- `--enable-ncpus=2` is required for the SMP path to be compiled in at all;
  on a UP build these hunks are dead code.

## Before sending

- `Signed-off-by:` is missing from all three patches. They must be added
  with the sender's real name and address; the `From:` currently carries the
  address used by the author of the original fork commits.
- Worth deciding with the maintainers whether patch 1 should be sent alone
  first, since it is the actual fix and does not depend on patches 2 and 3.
  It applies to a clean `master` on its own (verified), as do patches 2 and
  3 independently of each other only in the order 2 then 3.