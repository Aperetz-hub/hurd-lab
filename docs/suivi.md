# GNU Hurd x86_64 fork — local base

Local branches `fork/x86_64` created on `mig`, `gnumach`, `hurd` (upstream = savannah, master of 16/10/2026).

## Observed state (audit 16/10/2026)

- x86_64 boots under QEMU q35 + e1000 + rumpdisk, `noide`. Image tested: `~/hurd-fork/vm/debian-hurd-amd64-*.img`.
- `gnumach/x86_64/` = shim over `i386/i386/` (symlink `x86_64/x86_64 -> ../i386/i386`).
  The 64-bit `Makefrag.am` reuses `i386/i386at/*`. Flags: `-mcmodel=kernel -mno-red-zone`, `KERNEL_MAP_BASE=0xffffffff80000000`.
- SMP: code exists (`i386/i386/smp.c`, `apic.c`, `cpuboot.S` x2, `acpi_parse_apic.c`, `ioapic.c`) but is declared broken (`configfrag-first.ac`: `Multiprocessor support is still broken`, default `NCPUS=1`).
- MIG: generic, `cpu.sym` handles LP64 via `sizeof(long/uintptr_t`. No arch dir.
- `hurd/`: only ~37 `#ifdef __x86_64__`. Remaining 64-bit issues: `exec/elfcore.c:102`, `rumpnet/net-rump.c:128 bpf_hdr`, `utils/login.c:168 utmp`, `procfs/cpuid`.
- rump: `rumpdisk` (ahcisata/piixide/ata + usb) and `rumpnet` (wm Intel PRO/1000 only, no virtio-net). `RUMP_NCPU=1` forced in both `main.c`.
- `noide`: no occurrence in the code; it is a boot argument + a `sd/hd/ud` guard in `rumpdisk/block-rump.c`.

## QEMU scripts

- `~/hurd-fork/vm/run-hurd64.sh`: standard 2G q35 e1000 boot, ssh 2222.
- `~/hurd-fork/vm/run-hurd64-smp.sh`: `-smp 2` test, ssh 2223, snapshot on.

## Proposed fork roadmap

1. P0 — Harness: QEMU scripts + `ssh -p 2222 root@localhost`, translator test doc (`settrans`, `fsysopts`, `showtrans`, `rpctrace`).
2. P1 — x86_64 correctness: `elfcore`, `bpf_hdr`, `login utmp`, `qemu-system-x86_64` test in CI.
3. P2 — rumpnet: add `virtio-net` to `rumpnet/Makefile`, bench e1000 vs virtio.
4. P3 — SMP: AP boot timeout (`smp.c`), `qemu -smp 2` tests, make `ipc/vm` MP-safe. Do not enable `NCPUS>1` by default before that.
5. P4 — rumpdisk: `--no-ide-probe` option, clarify the sd/hd guard.

## Fork follow-up (16/10/2026)

- `hurd/fork/x86_64`: `5636dd8` bpf_hdr sec/usec, `b9bbcf5` saturating login utmp
  (layout/time_t tests verified OK, temporary scripts cleaned up).
- `gnumach/fork/x86_64`: `cfe04a2` SMP timeout, boot degrades to BSP-only instead of
  `while(1)`. To test: `run-hurd64-smp.sh` with the kernel rebuilt in the VM.
- `mig`: host build OK (cpu.h LP64 correct, temporary directory cleaned up).
  `make check` produces 7 expected FAILs without `gnumach-dev` (stubs want `<mach/boolean.h>`).

## Kernel tests (16/10/2026, debian-hurd-amd64 VM, q35/e1000)

- Kernel `cfe04a2` (`--enable-ncpus=2`, 3.5M): **boots + SSH OK at `-smp 1`**
  (complete userland). Evidence: the patch does not break the UP path.
- Same kernel at `-smp 2`: **hangs after the rump/ahcisata phase** (`ahcisata0:
  64-bit DMA` frozen, no SSH). Likely cause: APs started but SMP is
  globally shaky overall (known races), or later IPIs going to dead APs
  (pmap/sched use the global `smp_get_numcpus()`).
- Fixup `12bd028`: `smp_set_numcpus()` propagated (global degradation).
  Tested at `-smp 2`: **same hang after `ahcisata0: 64-bit DMA`**.
  VGA capture at 1s (`VGA capture (not kept)`): `Sending IPIs from BSP APIC ID 0...`
  then `AP=(1) paging/mpdesc/gdt/... done` + `LAPIC timer configured on
  cpu 1` → **SIPIs OK, the AP starts, our degradation path does not
  trigger**. The hang comes from downstream SMP races (incomplete locking,
  cf. `#warning SMP support missing` in `io_perm.c`, `pcb.c`), not from
  our patch. Real SMP = upstream work (fine-grained locks), out of scope for a
  quick fix.
- VM lessons: `/tmp` is wiped on every reboot → work in `~`;
  `snapshot=on` discards everything at QEMU shutdown; persistent base image OK;
  `screendump` via the HMP monitor for boot (no Mach serial console).

## Network: pcn (16/10/2026, dev VM overlay, QEMU pcnet added)

- `7b5ab52` links `rumpdev_pci_if_pcn` (already packaged) next to `wm`. Build OK
  in the VM, both `.so` linked. No hard-coded names in `net-rump.c`.
- Live test: the driver **attaches** (`pcn0 at pci0 dev 3: AMD PCnet-PCI`,
  syslog) but reads MAC **`ff:ff:ff:ff:ff:ff`** → PIO (I/O port) reads
  return `0xFF`, interface unusable.
- Hypothesis: the rump PCI PIO path (`libpciaccess` → `pci-arbiter`)
  is broken under Hurd; `wm` (MMIO) and `rumpdisk` AHCI (MMIO) work.
- Test lesson: never launch a rumpnet instance with `wm` linked while
  `e1000` is active (`netdde`) — the wm probe claims the NIC and kills the network
  (recovered via `system_reset`). Test with a pcn-only variant.
- Consequence: **prioritize `vioif`/virtio-net (MMIO)** over pcn.
  `libvirtio_if_vioif` + `libpci_virtio` exist in `rumpkernel/src-netbsd`
  (branch `kernel-src`) but are not packaged by Debian → build the
  rump libs, link them into `RUMPLIBS`, test with `-device virtio-net-pci`.

## rump virtio-net (`vioif`) on Hurd (17/10/2026) — crash fixed

- Context: `libvirtio_if_vioif` + `libpci_virtio` are not packaged by
  Debian → built from `nbsrc25` (NetBSD 2025-01, commit `4a1a4e8`).
  Recipe: `patches/build/build-virtio.sh`; relink:
  `patches/build/link-vioif.sh` (non-PIE binary so crashes symbolize).
- **Bug**: after a correct `vioif0` attach (MAC `52:54:00:12:34:58`,
  rx/tx/ctrl virtqueues, IRQ 20), SIGSEGV at `device_open` (path
  `up_interface` → `rump_sys_ioctl(SIOCSIFFLAGS)` → `vioif_init` →
  `vioif_rx_filter` → `vioif_set_rx_filter` → `vioif_ctrl_acquire` →
  `curlwp`) on `mov %gs:0xac0,%rax` (`%gs` base = 0, subcode 0xAC0).
- **Root cause**: our objects were compiled with the arch's `<machine/cpu.h>`,
  where `curlwp == x86_curlwp()` (`%gs` access). But under Hurd, rump does
  **not** use `%gs` (glibc reserves `%fs` for TLS): it needs
  `curlwp == rump_curlwp_fast()` (*hypercall* mechanism,
  `rump_lwproc_curlwp_hypercall()`), as defined by the
  `sys/rump/include/machine/cpu.h`. The upstream include order
  (`sys/rump/Makefile.rump`) puts `-I${RUMPTOP}/include` **first**; our
  recipe put `-I$D` (build dir, `machine -> arch`) first → wrong
  `cpu.h`. (`-I$D` first had been chosen so that `<machine/intr.h>` = arch,
  because `x86/pci_machdep.h` requires `intr_handle_t`.)
- **Fix**: interpose `$HOME/ovr/machine/cpu.h -> rump` and
  `$HOME/ovr/machine/intr.h -> arch`, `ovr` first in the `-I` list, +
  `-D_RUMPKERNEL`. Verification: `if_vioif.pico` no longer has any
  `%gs:` or `x86_curlwp` reference, and calls `rump_lwproc_curlwp_hypercall`.
- **Result**: `device_open("vioif0") -> err=0` via the rumpnet translator,
  stable attach, no more crashes (backtrace under gdb). Next step:
  `pfinet` on the device / starting it at boot.
- Method: run the translator under gdb (`settrans -ac NODE wrapper-gdb`).
  **Caution**: `settrans` without `-a` = *passive* translator (starts
  only when the node is opened). Clean up leftover translator nodes:
  `find /` opens them and can freeze the VM.

## pfinet on vioif0 — live test (17/10/2026)

- rumpnet translator (`rumpnet-vioif-test`) + devnode on `/dev/vioif0`:
  `settrans -ac /dev/rumpnet /home/demo/rumpnet-vioif.sh` then
  `settrans -c /dev/vioif0 /hurd/devnode -M /dev/rumpnet vioif0`.
  `open-dev /dev/vioif0 eth` → `device_open("eth") -> err=0` (pfinet opens the
  node then calls `device_open("eth")`, cf. `pfinet/ethernet.c`).
- **Switch without restarting pfinet** (chosen method): `ifconfig` (Hurd)
  adds an interface to the live pfinet instance (pfinet manages N interfaces).
  Validated sequence (`scripts/net-switch-vioif.sh`):
  1. `pkill -f dhclient` (otherwise dhclient brings `eth0` back up) ;
  2. `ifconfig /dev/eth0 down` ;
  3. `ifconfig /dev/vioif0 10.0.2.15 netmask 255.255.255.0 up`.
  Result: **ping gw 10.0.2.2 + DNS 10.0.2.3 OK**, and **SSH via host
  hostfwd 2223 OK** (TX+RX+TCP+resolution, single-interface) → virtio-net driver
  fully functional. Automatic rollback (`KEEP` watchdog) validated.
- **Limit**: the default route stays bound to `eth0`. `fsysopts
  /servers/socket/2 --gateway 10.0.2.2` → `EBUSY` (interfaces in the
  same subnet); external access (outside 10.0.2.0/24) therefore fails after the switch.
- **Trap**: restarting pfinet (`settrans -g` then `settrans -ac` on
  `/servers/socket/2`) **freezes** everything: `ifconfig /dev/vioif0` blocks forever
  (the old pfinet keeps the device open). Do not restart pfinet at runtime;
  use `ifconfig` on the live instance, or configure vioif at boot.
  Recovery: reboot (but `/etc/init.d/networking` stop → `dhclient -r`
  can freeze shutdown without network → `system_reset` + fsck).

## virtio-net (vioif): integration at boot (17/10/2026) — DONE, validated

- Files (in `scripts/`):
  - `start-rumpnet.sh` → installed as `/usr/local/sbin/start-rumpnet.sh`: starts
    the rumpnet translator + the devnode `/dev/vioif0` (idempotent, log
    `/tmp/start-rumpnet.log`).
  - `interfaces.vioif` → `/etc/network/interfaces`: `auto /dev/vioif0` with
    `pre-up /usr/local/sbin/start-rumpnet.sh` ; `eth0` kept as fallback
    (not auto) ; backup `/etc/network/interfaces.eth0.bak`.
- **Result after reboot**: clean boot (no hang at shutdown because `eth0`
  is no longer `auto` → no more `dhclient -r` on a dead network). The single
  interface is `/dev/vioif0` (MAC `52:54:00:12:34:58`) and `pfinet` does have
  `--gateway=10.0.2.2` (at boot pfinet is fresh → **no `EBUSY`**).
  Verified: `ping 10.0.2.2`, `ping 8.8.8.8`, DNS (`getent ahostsv4`),
  HTTPS (`wget https://example.com` rc=0), SSH via host hostfwd 2223.
- **Caveat**: do not `ifup /dev/eth0` on a live vioif system: the
  `dhcp`/`fsysopts` method of ifup rebuilds the pfinet options and **loses
  `--gateway`** (no more external access) in addition to adding a 2nd interface.
  If needed, reboot (the boot state is clean) or re-fix the route.
- Recovery on failure: console access (QEMU monitor → `screendump`/
  `sendkey`) → `sudo cp /etc/network/interfaces.eth0.bak
  /etc/network/interfaces && sudo ifup /dev/eth0 && sudo reboot`.

## Resuming `-smp 2` (17/10/2026) — new observation

> ⚠️ **Nuance (see § *Autopsy of the `-smp 2` hang*, 2nd/3rd passes)**: the
> "CPU0 userspace spin `RIP=0x5225ac`" measurements below were taken **with
> the gdb stub (`-s`) and without verified resumption** → they are skewed by
> **pausing** the VM (gdb session exited without `detach`, `info status` =
> `paused`). On a fresh overlay, the real hang happens **earlier**, in the
> AHCI attach of `rumpdisk` (`ahcisata0: 64-bit DMA`) — and it is
> **intermittent** (see 3rd pass).

- The dev VM's default kernel (GRUB entry 0) is already
  `gnumach-smp-test.gz`: `-smp 1` = working system (vioif network +
  internet). The `-smp 2` test = restart QEMU with `-smp 2` (same disk,
  same kernel) — preferably on a disposable overlay (cf. README).
  *(Verified since, 3rd pass: the base image `hurd-dev.qcow2` actually boots
  the **stock** kernel, `/boot/gnumach-1.8-amd64-up.gz` = 236,555 bytes, md5
  `58334bef…` ; the **fork** kernel (1,611,776 bytes) is installed in the overlays
  — `hurd-smp-test.qcow2`, `ovl-stock-smp2.qcow2` — cf. "Kernel identities".)*
- **Revised observation**: at `-smp 2` the boot goes **further** than the old
  "frozen after `ahcisata0: 64-bit DMA`". (Measurement made at the time with X
  active; the project has switched since to **pure console, no X**.) The boot
  progresses to the login prompt but:
  - **the network does not come up** (neither 2222 nor 2223, `pfinet` useless) ;
  - **CPU0 stays stuck in userspace**: `RIP=0x5225ac` (`CPL=3`)
    constant across several samples → user spin (libpthread/glibc
    or kernel IPC), and **CPU1 is idle** (`machine_idle`, `HLT=1`).
  ⇒ this is a **soft-wedge** (spin), not a single kernel loop. Consistent with
    upstream SMP races (missing fine-grained locks). No quick fix.
- **Diagnosis without gdb** (absent from the host): HMP
  `cpu N` + `info registers` to sample `RIP` per CPU. Symbols:
  `addr2line -e /home/ben/hurd-fork/patches/gnumach-smp-test -f -p 0x...`
  (ELF with `debug_info`). `-s` (gdbserver tcp:1234) starts the VM *paused* →
  run `cont` before observing.
- **Recovery**: the wedged SMP guest does not shut down cleanly → kill QEMU
  → fs becomes dirty → at reboot, maintenance prompt → `Enter` →
  `fsck -y /dev/wd0s2` → `Ctrl-D` (boot resumes).
- Reminder: `eth0`/netdde remains a fallback; at `-smp 1` the VM works
  (vioif network + internet).

## Boot option `maxcpus=N` / `nosmp` (17/10/2026) — kept

- **Role**: kernel command-line (multiboot) option that **caps the number of
  CPUs used** at startup:
  - `nosmp` → at most 1 CPU ;
  - `maxcpus=N` → at most N CPUs.
  Implemented in `i386/i386/smp.c::smp_data_init()` (commit `a4237652`,
  git branch `fork/x86_64`), it reads `kernel_cmdline`. It therefore allows
  **forcing single-CPU mode on a multi-core VM** (`-smp N`), without
  recompiling the kernel or changing QEMU's `-smp`.
- **Usage**: add `nosmp` (or `maxcpus=1`) to the GRUB line
  `multiboot /boot/gnumach… root=part:2:device:wd0`. Boot-time confirmation:
  `SMP: maxcpus=N, limite le noyau a N CPU sur M`.
- **What it does (and does not do)**: `maxcpus=1` **avoids the hang most of
  the time** — verified (3rd pass, gdb: `smp_info.num_cpus = 1`, AP never
  started): `-smp 2` **+ `maxcpus=1` boots 5 times out of 6**, while `-smp 2`
  **without the option hangs 4 times out of 4**. **The AP startup is therefore
  the main trigger** (the SMP *build* alone, at `-smp 1`, never hangs).
  There remains a **residual intermittence** (1 hang / 6 with `maxcpus=1`).
  The option is a useful workaround, not a fix.
- **Status**: **kept**. Clean working tree, commit pushed to
  `remotes/github/fork/x86_64`. No revert.

## Autopsy of the `-smp 2` hang (17/10/2026) — GDB over QEMU

Method: `vm/run-hurd64-smp.sh` now accepts `GDB=1` (QEMU stub
`-gdb tcp::1234`), `STOP=1` (`-S`), `FRESH=0` (reuse the overlay). gdb is
absent from the host and `sudo` is blocked → gdb fetched **without root** via
`apt-get download gdb` + `dpkg-deb -x` (same for its libs: libbabeltrace, libipt,
libsource-highlight, libdebuginfod) then
`LD_LIBRARY_PATH=/tmp/gdbroot/usr/lib/x86_64-linux-gnu /tmp/gdbroot/usr/bin/gdb
/tmp/gnumach-new` (fork kernel, ELF with `debug_info`).

**Major methodological trap (fixed in the 3rd pass)**: with `-gdb`, if a
gdb session exits **without `detach`** (gdb script interrupted by an error,
for instance), QEMU leaves the VM **paused**: `info status` → `VM status: paused`.
All samples (VGA screen, RIP) are then **frozen** and give the illusion of a
"hang" — this is what produced false measurements on previous days
(including one "boot" run). HMP `cont` is ignored while the gdb stub has not
been released. **Always end the gdb script with `detach`, then check that
`info status` = `running` before any measurement.**

### Result (reproducible)

- **Fresh** overlay (`FRESH=1`, clean fs) + fork kernel installed at
  `/boot/gnumach-1.8-amd64-up.gz`:
  - `-smp 1` → full boot, vioif network up, SSH OK ;
  - `-smp 2` → boot **always stalls** on the same console line:
    ```
    ahcisata0 at pci0 dev 31 function 2: vendor 8086 product 2922 (rev. 0x02)
    ahcisata0: 64-bit DMA
    ```
    (observed 4 times, including > 5 min without progress ; `cont` + `sendkey`
    do not unblock it). In the 3rd pass, `-smp 2` without options hangs
    **4 rounds out of 4**, but the hang is **intermittent** once the AP is
    not started (see § 3rd pass).
- This output comes from the **user translator `rumpdisk`**
  (`/hurd/rumpdisk.static`): the `ahcisata` string **is not** in the kernel
  (`strings gnumach | grep -c ahcisata` = 0) and `rump` is not compiled into
  gnumach. It is the rump kernel embedded in `rumpdisk` that stalls in the
  middle of the AHCI attach, **before any fs mount**.
- Kernel state at the hang (gdb, symbols `/tmp/gnumach-new`):
  - **CPU0**: `rip 0xffffffff810168e4` = `hpclock_read_counter+20`
    (`i386/i386/apic.c:495`) ; backtrace
    `clock_interrupt (usec=10000) ← kern/mach_clock.c:341 ← interrupt()`
    (`x86_64/interrupt.S:128`). ⇒ CPU0 only handles the 100 Hz clock interrupt.
  - **CPU1**: `rip 0xffffffff8103403d` = `machine_idle+13`
    (`i386/i386at/model_dep.c:257`), `HLT=1`.
  - `percpu_array[0..1].active_thread->task->name` = `"gnumach"` (idle task)
    ⇒ **no user task is runnable**: `rumpdisk` is **blocked inside the
    kernel**, it is not spinning.
  - The HPET counter advances (`xp/2xw 0xfed000f0` changes) ⇒ the clock is
    **not** frozen; not a timer problem.
- Interpretation: under `-smp 2` the `rumpdisk` AHCI attach never completes
  (waiting for an event that never arrives: undelivered PCI IRQ / lost
  wakeup). The rest of the kernel is healthy. This is indeed an **SMP race**:
  at `-smp 1` the *same* `rumpdisk` binary gets through.
- gnumach-side lead: `device/intr.c` — a user IRQ is **masked** until the
  user's `irq_acknowledge()` (`queue_intr()` → `__disable_irq()`), and
  cleanup of dead delivery ports prints
  `release a dead delivery port` / `still N unacked irqs` (messages already seen
  during the network-switch freeze). An undelivered/unacknowledged IRQ under SMP
  leaves the driver blocked forever. **No isolated fix identified at this stage.**
- Limit: walking `default_pset.tasks` via gdb returns invalid entries, and a
  boot hung before the shell precludes `ps` ; the exact user PC of `rumpdisk`
  could therefore not be captured. The **faulty user binary is nonetheless
  unambiguously identified** (`rumpdisk`) and the **blocking point** is the
  AHCI attach (`ahcisata`, after "64-bit DMA").

⇒ Work **not isolated** (rump/Hurd, not a simple gnumach lock):
`-smp 2` remains **unusable**; `-smp 1` remains the working reference.

> ⚠️ **Update (2nd then 3rd pass, same day)**: the interpretation "SMP race
> in `device/intr.c` / masked user IRQ" above is **dropped**: at the hang the
> user IRQ (PCI 16/AHCI) is **never established** — the stall is *before*
> `pci_intr_establish`. The interpretation "no hardware IRQ arrives once the
> IOAPIC is emulated by KVM" (2nd pass) is **also dropped** by the 3rd pass:
> under `kernel-irqchip=on` the IOAPIC is correctly programmed and hardware
> IRQs are delivered during the hang. See the two next sections.

## Autopsy of the `-smp 2` hang — 2nd pass: IOAPIC differential **KVM vs QEMU** (17/10/2026)

New method (no writes into `hurd-dev.qcow2`; disposable overlays):

- **Capture of the full boot log via the VGA memory, VM never frozen**:
  HMP `xp/4000cb 0xb8000` reads the text buffer (physical `0xb8000`,
  byte/attribute pairs → characters at even offsets, 80 columns). The sampler
  `vm/vgacap.py` (`SOCK=… OUT=… DUR=…`) only writes screens that change.
  This is the **only** way to get the kernel + user log headless:
  `-serial file:` stays **empty**, everything goes through the VGA.
- **Reading the IOAPIC via gdb**: possible *only* if the IOAPIC is not held
  by KVM, i.e. with `-M q35,kernel-irqchip=split`. One writes the selector
  then reads the window through the kernel mapping of the IOAPIC
  (`apic_data.ioapic_list[0].ioapic` = `0xffffffffca927000`):
  `set {unsigned int}0xffffffffca927000 = 1` ; `x/1xw 0xffffffffca927010`
  → `0x00170020` (24 pins, version `0x20`). With `kernel-irqchip=on` (default),
  these accesses are **not** readable from the host (region owned by
  the kernel irqchip): hence the failure of the previous reads.

### Decisive result: the hang follows the KVM-emulated IOAPIC (but not for the assumed reason — see 3rd pass)

| QEMU configuration | kernel | `-smp` | result |
|---|---|---|---|
| `-M q35` (KVM irqchip, default) | fork (SMP build, Oct. 2026) | 2 | **hang**: screen frozen > 206 s at `ahcisata0: 64-bit DMA` |
| `-M q35` | fork | 1 | full boot → `login:` (AHCI attach OK at `1.040 s` of rump time) |
| **`-M q35,kernel-irqchip=split`** | **fork** | **2** | **full boot (~40 s) → `login:`, keyboard OK** |
| `-M q35` (KVM irqchip) | **stock (2026-02-24, `-up`)** | 2 | full boot, keyboard OK |

- At `-smp 2` (KVM irqchip) the stall happens **exactly** between
  `ahcisata0: 64-bit DMA` and `ahcisata0: interrupting at …` (message printed
  just after `rumpdisk` establishes the PCI IRQ). At `-smp 1` the rest scrolls
  immediately: `interrupting at pausebreak`, `atabus0..5`,
  `wd0 at atabus0 drive 0`, `wd0(ahcisata0:0:0): using PIO mode 4 …`, then
  login / `sshd`.
- **With the IOAPIC emulated by QEMU (`kernel-irqchip=split`), the same fork
  kernel at `-smp 2` boots normally and the keyboard responds** (each
  `sendkey ret` yields a new `login:` prompt). The KVM vs QEMU differential is
  therefore real, but **the cause is not "the KVM IOAPIC misprogrammed"**: the
  3rd pass (below) shows that under `kernel-irqchip=on` the redir entries are
  **correct** and hardware IRQs are **delivered** at the time of the hang. The
  `split` mode mainly changes **timing** (outgoing IOAPIC accesses instead of
  direct writes).
- Control: the **stock kernel** (2026-02-24, `-up` suffix) works with the
  KVM IOAPIC ⇒ the KVM IOAPIC is not intrinsically broken; the regression is
  indeed on the **fork kernel** side.
- The fork kernel is an **SMP build** (`NCPUS=2`) whereas the stock one is
  `-up`. At `-smp 1` (1 vCPU) the fork **never hangs** (verified: keyboard OK,
  `info irq` IRQ1 10→12). At `-smp 2` it hangs **4 times out of 4**; at `-smp 2`
  **+ `maxcpus=1`** (AP never started) it boots **5 times out of 6** ⇒ the
  **effective AP startup** is the main trigger (3rd pass).

### 3rd pass: `info pic`/`info irq` under KVM, role of the AP, intermittence (17/10/2026)

⚠️ **Correction of the 2nd pass**: the claim "no hardware IRQ arrives once
the IOAPIC is emulated by KVM" is **false**. Under `kernel-irqchip=on`
the IOAPIC belongs to KVM, but HMP `info pic` exposes its exact state, and
that state is **correct**; hardware IRQs **are delivered** during the hang
(gdb measurements of the 2nd pass: `hardclock` ≈ 1854 in ~18 s at 100 Hz,
`ioapic_irq_eoi(pin==0)` = 1854, `kdintr` = 10). The kernel is alive; it is
the **user side** (`rumpdisk`, ACPI `_CRS` chain → `pci_intr_establish`) that no
longer progresses.

**Compared routing tables** (HMP `info pic` ; KVM IOAPIC `ver=0x11 id=0x00`,
`sel` = last selected GSI):

| case | `sel` | pin 1 (kbd) | pin 2 (PIT) | pin 4 | pin 9 | pin 12 | pin 16 (AHCI) | pin 23 |
|---|---|---|---|---|---|---|---|---|
| stock `-up`, `-smp 2` (OK) | `0x30` | `0x0031` v49 | `0x0030` v48 | `0x0034` v52 | `0x8039` | `0x003c` v60 | **`0xa040` unmasked** | **`0xa047` unmasked** |
| fork `-smp 1` (OK) | `0x30` | same | same | same | same | same | **unmasked** | **unmasked** |
| fork `-smp 2` (**hang**) | `0x22` | same | same | same | same | `0x1003c` masked | `0x1a040` **masked** | `0x1a047` **masked** |

⇒ at the hang, pins 1/2/4/9 are **identical** to the working cases
(`info irq`: `1:3`, `4:1`): the IOAPIC is **not** misprogrammed by KVM. The
only difference is the **establishment of IRQ 16 (AHCI)**: at the hang, pin 16
is **still masked** ⇒ the stall occurs **before** `pci_intr_establish`, i.e.
exactly between `ahcisata0: 64-bit DMA` and `ahcisata0: interrupting at …`.
In the working cases, pin 16 is unmasked and `info irq` shows `10:` and
`16:` at ~13,000–33,000 (AHCI activity; the same line counted via PIC **and**
IOAPIC).

**The AP startup is the main trigger.** Automated rounds
(`/tmp/gm/trials.sh`: overlay copied identically before each round, no
gdb intervention except the `maxcpus=1` injection, screen read via
`xp/16384cb 0xb8000`):

| recipe (`-smp 2`, KVM IOAPIC, fork kernel) | result |
|---|---|
| AP started (no option) | **hang 4/4** (+ `F2`, same point) |
| `maxcpus=1` — AP **not** started (gdb: `smp_info.num_cpus = 1`) | **boot 5/6** (hang 1/6) |
| `kernel-irqchip=split` | **boot** |
| `-smp 1` (1 vCPU, but SMP *build*) | **boot**, keyboard OK (`info irq` IRQ1 10→12) |
| stock `-up`, `-smp 2` | **boot**, keyboard OK (IRQ1 → 237) |

⇒ `-smp 1` (SMP build, 1 vCPU) **never hangs**: the SMP build alone is
innocent. With a 2nd vCPU **and** the AP started, the hang is nearly
systematic (4/4); with a 2nd vCPU but the AP **not** started, it becomes rare
(1/6). The fork-only AP bring-up code (`mp_desc.c`: `lapic_disable()` then
wait `pit_udelay(1000)` with interrupts off, then `lapic_enable()`) is therefore the
**prime suspect** — but it is **not** the sole cause (the 1/6 with
`maxcpus=1`, where this code is not executed, shows a **race component**).

**Intermittence / race**: the same `maxcpus=1` that boots 5 times out of 6
hung once (`MX`); conversely, the "boots" obtained after a series of gdb
pauses are explained by the shifted **timing**. The `kernel-irqchip=split`
mode (real, reproducible differential) mainly changes the timing of IOAPIC
accesses.

**Audit window 2026-02-24 → HEAD** (`git diff v1.8+git20260224..HEAD`):

| file | Δ | content |
|---|---|---|
| `i386/i386/mp_desc.c` | +34 | **fork-only**: bounded AP wait (5 s) with `pit_udelay(1000)`, BSP degradation (`smp_set_numcpus(1)`) — executed **only if `ncpus > 1`** |
| `i386/i386/smp.c` | +96 | **fork-only**: `maxcpus=N`/`nosmp` option ; bounded `smp_send_ipi`/`wait_for_ipi` (degradation instead of deadlock) |
| `i386/i386/pit.c` | +16 | upstream window `38e82f35`: `pit_udelay` fixed (> `MAX_PIT_USEC`, `usec` semantics, 64-bit `val`) — it is also what replaces `hpet_udelay` with `pit_udelay` in `smp.c` |
| `i386/i386/apic.c` | +12 | upstream window `40166870`: optional HPET (`hpet_init` returns if absent, `hpclock_read_counter` keeps `hpet_addr`) |
| `kern/processor.c` | +7 | upstream window `a30402ef`: `pset->max_priority` `BASEPRI_USER` → `BASEPRI_SYSTEM` (+ `processor_name_self`) |
| `kern/sched_prim.c` | +6 | upstream window `d30629e6`: guards `processor != PROCESSOR_NULL` before `processor->state == PROCESSOR_IDLE` |
| `kern/ast.c`, `kern/machine.c` | ±2 | window: `#ifndef MIGRATING_THREADS` guards removed |
| `kern/debug.c` | +6 | window: `panic_init` initializes `Assert_print_lock` + `panic_lock` |
| `x86_64/locore.S` | −88 | MACH_TTD removal, `cpuid`/`ebx`, boot paths |
| `i386/spl.S`, `x86_64/spl.S` | ±8 | cosmetic |
| `ioapic.c`, `i386at/interrupt.S`, `int_init.c`, `idt.c`, `irq.c` | 0 | **unchanged** |
| `i386/i386at/model_dep.c` | +439 | multiboot2/UEFI, `machine_init` reordering |

None of these deltas demonstrates an isolated defect; the only **fork-only
code executed only when the AP starts** is the `mp_desc.c` loop
(`pit_udelay(1000)` / PIT channel 2 / port `0x61`, interrupts off via `splhigh()`),
with the `lapic_disable()`/`lapic_enable()` pair surrounding it (these are
upstream). That is the lead to investigate first (cf. "Next experiments").

Also noteworthy in the window: the pair `f41f8801` "periodic intrs: Change the
clock source to be lapic timer on all cpus" + its `ddb0766a` **Revert** (net
effect nil), and the two fork commits `99a21a39`/`c11e6cea` (AP degradation).

### Observed IOAPIC entries (in the working case, `kernel-irqchip=split`)

```
VER = 0x00170020                    (24 pins, version 0x20)
pin  1 (keyboard) low = 0x00000031 → vector 49, unmasked, edge, active high ✔
pin  2 (timer)   low = 0x00000030 → vector 48, unmasked ✔   (remapped timer)
pin  3           low = 0x00010033 → masked
pin  9 (ACPI)    low = 0x00008039 → masked, level, active low
pin 10 (SATA)    low = 0x0001803a → masked
pin 16           low = 0x0000a040 → masked, level
has_irq_specific_eoi = 1 (version 0x20 ⇒ directed EOI)
```

The kernel therefore programs pin1/pin2 correctly; the PCI pins stay
**masked** even after user handlers are installed
(`user_intr_handlers[9/16/23] ≠ NULL`).

### Hypotheses ruled out by measurement

- **`has_irq_specific_eoi` / EOI "workaround"**: with the KVM IOAPIC the
  version read is `0x11` (KVM does not advertise `0x20`) ⇒
  `has_irq_specific_eoi = 0` ⇒ *mask/edge/restore* path at **every** EOI.
  But (a) the **stock** kernel also reads `0x11` and **works**; (b) forcing
  the value to 0 hot in the working case **breaks nothing**. ⇒ this flag is
  not the trigger.
- **`ioapic.c` is identical** between the stock kernel (2026-02-24) and the
  fork HEAD: `ioapic.c`, `int_init.c`, `idt.c`, `irq.c`, `apic.h`,
  `device/intr.c`, `kd.c`, `spl.c` → **no modification**
  (`git diff v1.8+git20260224..HEAD`, see the 3rd-pass audit table).
- **`device/intr.c`** (`assert_wait`/`queue_intr`/`irq_acknowledge`/
  `intr_thread`): **never reached** — `irqtab.tot_num_intr = 0`, queues
  `main_intr_queue` (ids 9/16/23) at `interrupts = n_unacked = 0`,
  `nested_irqs[].ndisabled = 0`, handlers `user_intr_handlers` installed.
  The blocking point is **before** this machinery: at the hang, PCI IRQ 16
  (AHCI) is **not yet established** by `rumpdisk` (`irqtab.tot_num_intr = 0`,
  pin 16 still masked — 3rd pass). The hardware IRQs already established
  (keyboard IRQ1, remapped PIT) **are** delivered and acknowledged; `kdintr`
  is in place (`ivect[1]`) and `unexpected ACK from keyboard` proves
  `kdinit` (hence `unmask_irq(KBD_IRQ)`) runs in the fork kernel too.
- **Regression window** (2026-02-24 → HEAD, `git diff
  v1.8+git20260224..HEAD`): file-by-file detail in the 3rd-pass table.
  In short: `ioapic.c`/`interrupt.S`/`int_init.c`/`idt.c`/`irq.c`
  **unchanged**; fork side, `mp_desc.c` (+34: bounded AP wait +
  `pit_udelay(1000)` + BSP degradation, only if `ncpus > 1`) and `smp.c`
  (+96: `maxcpus`/`nosmp`, bounded IPIs); upstream window,
  `pset->max_priority` (`BASEPRI_USER`→`BASEPRI_SYSTEM`),
  `processor != PROCESSOR_NULL` guards from `sched_prim.c`, `pit_udelay`
  fix (`pit.c`), optional HPET (`apic.c`), `model_dep.c` (multiboot2/UEFI),
  `locore.S` (MACH_TTD removal), `spl.S` (cosmetic).

### Audit conclusion — no blind fix

- The initial hypothesis (`assert_wait` + `queue_intr` +
  `device_intr_ack` + `ioapic_toggle` locking in `device/intr.c`) is
  **dropped**: the machinery is coherent and **never solicited** at the hang
  (nothing queued, nothing masked, no lock held) — because PCI IRQ 16 is not
  established yet. The hypothesis "the KVM IOAPIC does not deliver IRQs" is
  **dropped** too (3rd pass: correct tables, IRQs delivered).
- **Positive** element: **the AP startup** moves the hang from "rare"
  (1/6 with `maxcpus=1`) to "nearly systematic" (4/4). The fork-only code
  executed at that moment (`mp_desc.c`: `lapic_disable()`, wait
  `pit_udelay(1000)` with interrupts off, `lapic_enable()`) is the
  **suspect #1**.
- **No isolated and demonstrable defect** was identified ⇒ **no kernel
  patch** (an unproven fix would break the working `-smp 1` reference).
- **Validated workarounds** (disposable overlay only), in order of
  reliability:
  1. `-smp 1` (working reference, keyboard OK) ;
  2. `-smp 2` with a QEMU-emulated IOAPIC — full boot + keyboard:
     ```sh
     MACHINE=q35,kernel-irqchip=split SMP=2 OVL=… MON=… SSH_PORT=2322 \
       ./run-hurd64-smp.sh /home/ben/hurd-fork/vm/hurd-dev.qcow2
     ```
  3. `-smp 2` **+ `maxcpus=1`** (fork option): boot 5/6 — useful but **not
     guaranteed** (the 2nd vCPU still visible to the kernel).
  (never modify `hurd-dev.qcow2` ; the `MACHINE=q35` default stays unchanged.)

### Reproduce

```sh
cd /home/ben/hurd-fork/project/vm
# disposable overlay with the fork kernel at /boot/gnumach-1.8-amd64-up.gz
OVL=/home/ben/hurd-fork/vm/ovl-stock-smp2.qcow2 FRESH=0 MON=/tmp/hurd-smp2-mon \
  SMP=2 SSH_PORT=2322 VIOIF_PORT=2323 ./run-hurd64-smp.sh \
  /home/ben/hurd-fork/vm/hurd-dev.qcow2
# read the screen without freezing the VM:
printf 'xp/4000cb 0xb8000\n' | socat - UNIX-CONNECT:/tmp/hurd-smp2-mon
# continuous log:
SOCK=/tmp/hurd-smp2-mon OUT=/tmp/bootlog-smp2.txt DUR=300 python3 vm/vgacap.py
# variant that boots (IOAPIC userspace):
MACHINE=q35,kernel-irqchip=split SMP=2 OVL=… MON=/tmp/hurd-split-mon \
  ./run-hurd64-smp.sh /home/ben/hurd-fork/vm/hurd-dev.qcow2
# IOAPIC via gdb (stub: printf 'gdbserver tcp::1234\n' | socat - UNIX-CONNECT:$MON) :
#   set {unsigned int}0xffffffffca927000 = 1 ; x/1xw 0xffffffffca927010
```

Repeated rounds (3rd pass): copy the overlay **before each round**
(`cp ovl-stock-smp2.qcow2 ovl-trial-N.qcow2`), start the VM, wait ~85 s,
then classify according to the screen read via `xp/16384cb 0xb8000`:
**WEDGE** if the last line is `ahcisata0: 64-bit DMA`, **BOOT** if
`login:` appears. Inject `maxcpus=1` **without touching GRUB**: start with
`GDB=1 STOP=1`, `hbreak smp_init`, then write into `kernel_cmdline`
(`0xffffffffc17ff000`, fresh page from `init_alloc_aligned`) the string
`root=part:2:device:wd0 maxcpus=1\0`, `delete` the breakpoint and `detach` —
**always check `info status` = `running`** afterwards (see methodological
trap above). Confirmation: `smp_info = {num_cpus = 1}`,
`machine_slot[1].is_cpu = 0`, knowing that `start_other_cpus()` returns
immediately (`mp_desc.c:284`, `if (ncpus == 1) return;`).

**Kernel identities** (verified in the guest, `uname` + `md5sum`):

- **stock**: `/boot/gnumach-1.8-amd64-up.gz` from the base image
  (`hurd-dev.qcow2`), **236,555 bytes**, md5
  `58334befca2eaa36dc7358cc996ecffe` — same as `/boot/gnumach-stock.gz`
  in the fork overlay. `uname` = `GNU-Mach 1.8+git20260224-up-amd64`,
  `nproc` = **1**.
- **fork**: `/boot/gnumach-1.8-amd64-up.gz` from `ovl-stock-smp2.qcow2`,
  **1,611,776 bytes**, md5 `c81dfe4716a9216d6bf0c2047998cdf7` (copy typo fixed
  in the 4th pass: the previous md5 was truncated to 30 hex chars).
  Working variants (4th pass, `/tmp/gnumach-smp2.gz` = fork reference):
  `kA` = HEAD + `k4-A` marker, md5 `de1518e0517d160dcda0858e6a4057f7` ;
  `kB` = AP wait in `cpu_pause()`, md5 `51241aed847422c7a51486d43518c52f`.

⇒ a **`FRESH=1`** overlay (plain copy of the base) boots the **stock**
kernel, not the fork: the fork recipe **requires**
`OVL=ovl-stock-smp2.qcow2 FRESH=0` (or a copy of that overlay, cf. rounds
above).

### Next experiments (precise)

1. ~~Settle "SMP build" vs "AP started"~~ **DONE (3rd pass)**: `-smp 1`
   never hangs; `-smp 2` hangs 4/4; `-smp 2` + `maxcpus=1` (AP not started,
   `smp_info.num_cpus = 1` verified by gdb) boots 5/6. ⇒ **trigger = AP
   startup**, with a residual **race component** (1/6).
2. ~~Compare the PIC/IOAPIC tables in the 3 cases~~ **DONE (3rd pass)**:
   active pins identical; only difference = **pin 16 (AHCI) still masked
   at the hang** ⇒ stall **before** `pci_intr_establish`.
3. **Priority** — `pit_udelay(1000)` lead from `mp_desc.c`: the only fork-only
   code executed only when the AP starts (PIT channel 2 / port `0x61`,
   interrupts off via `splhigh()`). Experiment: hot-patch via gdb the loop's
   `call pit_udelay` (NOP) and see whether `-smp 2` boots; if not, replace
   `pit_udelay(1000)` with a bounded `cpu_pause()` (spin without touching the
   PIT, like upstream) and compare the hang rate (4/4 expected → ?).
4. Instrument `ioapic_toggle_entry` / `unmask_irq` / `ioapic_irq_eoi`
   (ring buffer `pin`/`mask`/`entry.lo`/`cpu`) and read at the hang via gdb →
   detect a corrupted read-modify-write.
5. Compare the `KVM_SET_GSI_ROUTING` PIC→GSI routing host-side between the
   stock kernel (OK) and the fork (KO) — requires the stock kernel symbols.
6. User side: instrument the ACPI `_SB.GSIA._CRS` → `pci_intr_establish`
   chain of `rumpdisk`/`pci-arbiter` (IPC `device_intr`/`memory object`)
   to see *where* the thread blocks at the hang (see **4th pass**:
   the claim "the kernel itself is alive" is **false** — it is precisely the
   opposite, the kernel clock is dead, so no timeout ever expires; the thread
   that "blocks" is just waiting for a delay that never arrives).

### 4th pass: locked `FRESH` trap, PIT lead dropped, **measured cause = dead master clock** (18/10/2026)

Goals of this pass: (1) lock down the `FRESH=1` trap (a fresh overlay
silently restarted from the **stock** kernel) ; (2) test the
`pit_udelay(1000)` lead for the AP wait ; (3) at the "hang", locate with GDB
where `rumpdisk` sleeps.

#### 1) `FRESH=1` trap locked down (launcher)

`hurd-dev.qcow2` (base image) contains the **stock** kernel: a fresh overlay
(`FRESH=1`) chained on the base thus booted the **stock** kernel, not the
fork. All existing rounds did use `OVL=… FRESH=0`, but the trap was silent;
it is now impossible to trigger by accident:

- new variable `MASTER` (default `ovl-fork-master.qcow2`) = **master**
  overlay carrying the already-installed fork kernel;
- with `FRESH=1`, the launcher **refuses (exit 1)** if the master is absent, and
  creates the overlay **chained on the master** — 3-level qcow2 chain
  `ovl-trial ← ovl-fork-master ← ovl-stock-smp2 ← hurd-dev.qcow2`, validated ;
- `STOCK=1` allows (knowingly) chaining on the base to test the stock kernel.

#### 2) `pit_udelay(1000)` AP-wait lead: **dropped by measurement**

Variant builds in `/tmp/gm` (`src` = fork HEAD `a4237652`,
`gcc 14.2.0`, `--host=x86_64-pc-gnu --enable-ncpus=2`) :

| kernel | specific trait | md5 of `gnumach-…up.gz` |
|---|---|---|
| **A** | HEAD + `k4-A` marker | `de1518e0517d160dcda0858e6a4057f7` |
| **B** | AP wait in `cpu_pause()` instead of `pit_udelay()` | `51241aed847422c7a51486d43518c52f` |

Matrix (disposable overlays, `vgaprobe.py` at the end of each round):

| kernel | option | WEDGE | BOOT |
|---|---|---|---|
| fork (ref) | `-smp 2` | 3 | 0 |
| A (`k4-A`) | `-smp 2` | 5 | 1 |
| **B (`cpu_pause`)** | `-smp 2` | **6** | **0** |
| B | `-smp 2 maxcpus=1` | 0 | 6 |

⇒ replacing `pit_udelay()` with `cpu_pause()` changes **nothing** (6/6 hang):
the PIT lead for the AP wait is **dead**. Static check: `pit_udelay` is
**upstream** (`38e82f35`, replaces `hpet_udelay` in `smp.c`) and only uses
**channel 2** (`PIT_C2|PIT_LOADMODE|PIT_ONESHOTMODE`) ; only `clkstart()`
(`pit.c:123`) touches channel 0 (`pit0_mode = PIT_C0|PIT_SQUAREMODE|PIT_READMODE`,
`pitctl_port`/`pitctr0_port` written only at `pit.c:137/140/142`). Only
`pit_udelay(1000)` from `mp_desc.c` is fork-only (`99a21a39`).

#### 3) At the "hang": the guest was not dead — the **clock** was

First surprise (GDB): a `-smp 2` round (overlay `ovl-t3`) actually reaches
**`login:`**, responds to SSH (`nproc = 2`, `up 74 seconds`, 61 processes, 7
sessions) ⇒ the screen-read "WEDGE" classification is a **false positive**.
Kernel-side, CPU0's backtrace in that state is
`host_get_uptime64 → time_value64_add_hpc → hpclock_read_counter`: a user
**polling loop** (clock poll), **not** a `wait_event`.

Kernel thermometer (read **in guest memory** via the QEMU monitor, never via
gdb — cf. commands below):

| | `-smp 1` (control) | `-smp 2` (two rounds, **no** gdb) |
|---|---|---|
| `uptime` / `mtime->uptime_value` | **+20,000 s in 20 s** | **frozen** at `0.120`…`0.130 s` over 15–30 s |
| `last_hpc_read` | advances (2³² wrap) | **unchanged** (`12281282 → 12281282`) |
| screen | `login:` at **T+30 s** | frozen on rump PCI numbering (`[ 1.0000050]`) |
| `login:` reached | yes | **no** (≥ 640 s) |
| `master_cpu` | `0` | `0` |

**Mechanistic cause (measured)**: under `-smp 2`, the **master CPU (0) stops
receiving the ~100 Hz clock interrupt as soon as the AP is started**, while
the guest itself is alive. But `clock_interrupt()`
(`kern/mach_clock.c`) advances the time (`time`/`uptime`), the **timeouts**
and `last_hpc_read` **only on `master_cpu`** (`if (my_cpu == master_cpu)` ;
`last_hpc_read = hpclock_read_counter()` l.341). Chain of consequences:

1. the kernel **and** user clock (`host_get_uptime64`) **freeze** ;
2. no **timeout** expires anymore and no **preemption** (quantum /
   `sched_tick`) happens on CPU 0 ⇒ the user thread polling the clock
   **spins forever** ;
3. `rumpdisk` waits for an AHCI attach delay/timeout that **never arrives** ⇒
   stall at `ahcisata0: 64-bit DMA` = **the root cause of the "I/O hang"**;
   it is **neither** an IOAPIC routing defect **nor** an I/O defect.

APIC/IRQ evidence (monitor):

- `info pic`: **pin 2 = `0x0000000000000030` dest=0 vec=48, active-high, edge,
  unmasked** — **identical** under `-smp 1` (which works) and `-smp 2` (which
  hangs) ⇒ not an IOAPIC masking/misrouting (pins 0-4 identical) ;
- `info lapic`: **CPU0's LVTT is masked/one-shot** (`initial_count =
  0xffffffff`, leftover from `calibrate_lapic_timer()` /
  `timer_measure_10x_apic_hz()` which ends with `lvt_timer.r |= LAPIC_DISABLE`) —
  in **both** cases `-smp 1` and `-smp 2` ⇒ CPU0's tick **does not come** from
  its LAPIC timer ;
- under `-smp 2`, **CPU1 (AP) does have a periodic LAPIC timer**
  (`LVTT 0x00020030 periodic`, `initial_count = 5859840`, vec 48) ⇒ this is
  the `hardclock` ≈ 100 Hz measured in the 2nd/3rd passes: a tick on the AP,
  which does not advance the clock (`my_cpu != master_cpu`). **Explicit
  correction** of the 2nd/3rd pass: "IRQs delivered, clock OK" is **wrong** —
  the clock IRQs are delivered to **the AP**, not to the master; `master_cpu`
  is 0 in both cases (so this is not a CPU numbering error).
- account consistent with `-smp 1`: no AP, so the master generates its own
  tick (remapped PIT, pin 2) and the clock lives ⇒ `-smp 1` **remains the
  working reference**.

Remaining zone to settle (**5th pass**) — **no kernel patch** (the
defect is not instruction-level isolated yet):

- only code executed **exclusively when the AP starts**: `start_other_cpus()`
  (`mp_desc.c:280`) — `splhigh()` → `lapic_disable()` (`:314`) → bounded AP
  wait (`pit_udelay(1000)`, channel 2, `:334`) →
  `pmap_remove_temporary_mapping()` → `interrupt_processor()` →
  `lapic_enable()` (`:360`). The `spl` is **not** left high (`TPR = 0x00` at
  the hang) ⇒ prime suspect = the `lapic_disable()`/`lapic_enable()` pair (or
  the AP wait sequence) ;
- corroborating detail: `startrtclock()` (`model_dep.c:926`) calls
  `lapic_enable_timer()` **only if `cpu_number() != 0`** ⇒ on CPU0 the LAPIC
  timer is deliberately left masked (masked by the calibration) and it is
  indeed the **remapped PIT (pin 2)** that must supply the master's tick;
  that source is what dries up under `-smp 2` ;
- planned instrumentation: **per-CPU** counters (or gdb breakpoint) in
  `hardclock`/`clock_interrupt`, read **before/after each step** of
  `start_other_cpus()`, to see **at which instruction the master tick
  disappears** — this is the first time we have a **binary and reproducible**
  effect (live vs frozen clock) to instrument.

#### Methodology (corrections from this pass)

- **The screen-read "hang" classification is misleading** (observed false
  positive: a "hung" round actually boots). Read the screen only **once**, at
  the end of the round (continuous sampling, repeated `xp`, holds QEMU's BQL
  and desynchronizes the vCPUs ⇒ it skews the hang rate); classify BOOT on
  the presence of `login:` / `debian tty1` (the Hurd console server clears
  the screen and writes at the top).
- **Never measure a paused VM**: a `gdb` killed by `timeout` **without
  `detach`** leaves QEMU `paused` without a client ⇒ SSH "Connection timed
  out during banner exchange". Always end with `detach` and check
  `info status` = `running`.
- Under `kernel-irqchip=on`, `info pic`/`info irq` reflect the **KVM** IOAPIC
  and are partial/incomplete; rely on the **kernel counters** (`uptime`,
  `last_hpc_read`, `mtime`, `master_cpu`), read in **guest memory** via the
  monitor.
- LAPIC/IOAPIC not readable from the host when KVM owns the irqchip:
  use HMP `cpu N` then `info lapic`.

#### Reproduce (4th pass)

```sh
# control -smp 1 (live clock) vs -smp 2 (dead clock):
cp /home/ben/hurd-fork/vm/ovl-stock-smp2.qcow2 /home/ben/hurd-fork/vm/ovl-exp-C1.qcow2
cd /home/ben/hurd-fork/project/vm
SMP=1 FRESH=0 OVL=/home/ben/hurd-fork/vm/ovl-exp-C1.qcow2 MON=/tmp/exp-C1-mon \
  SSH_PORT=2801 VIOIF_PORT=2901 ./run-hurd64-smp.sh /home/ben/hurd-fork/vm/hurd-dev.qcow2
# (replace SMP=1 with SMP=2, and C1 with C2, for the hanging case)

# kernel thermometer: read uptime / mtime->uptime_value / last_hpc_read / master_cpu
# (fork symbols: uptime 0xffffffff810ba2b0, mtime 0xffffffff810b8eb0,
#  last_hpc_read 0xffffffff810b8eb8, master_cpu 0xffffffff810ba8c8) :
printf 'x/2gx 0xffffffff810ba2b0\nx/wx 0xffffffff810b8eb8\nx/wx 0xffffffff810ba8c8\n' \
  | socat - UNIX-CONNECT:/tmp/exp-C1-mon
# re-read 20 s later: -smp 1 => uptime +20,000000000 ; -smp 2 => frozen.
# info lapic (per CPU) and info pic:
printf 'cpu 0\ninfo lapic\ninfo pic\n' | socat - UNIX-CONNECT:/tmp/exp-C1-mon

# repeated BOOT/WEDGE rounds (a single screen read, SSH check on BOOT) :
SMP=2 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-fork-master.qcow2 6
SMP=2 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-fork-master.qcow2 6 maxcpus /tmp/gnumach-new
```

### 5th pass: CPU0 loses its 100 Hz tick at AP start-up — **FIXED** (18/10/2026)

**Root cause found and fixed.** `start_other_cpus()` disabled the **BSP's own local
APIC** (`lapic_disable()`) around the AP bring-up. While `LAPIC_ENABLE` is clear the
BSP cannot accept *any* IOAPIC interrupt, so the 100 Hz PIT edges that arrive in
that window are consumed without ever being delivered, and the master CPU never
receives a clock tick again. Because `clock_interrupt()` advances time, the
timeout list and `last_hpc_read` **only on `master_cpu`**, the whole system clock
freezes: no time, no timeout, no preemption — and `rumpdisk` spins forever in its
AHCI attach. The fix is to **not** disable the BSP's LAPIC (`splhigh()` already
blocks interrupts for the whole window).

#### 1) New instrumentation (why the 4th pass could not see it)

- `vm/kprobe.py` — reads kernel globals straight out of **guest memory** through
  the QEMU monitor (`x/…`). Never gdb: the guest is never paused, so it cannot
  perturb or heal the fault. The ELF of the kernel under test must be passed in
  `KPROBE_ELF`, otherwise the addresses are wrong (all zeros).
- `vm/smp-clock-check.sh` — runs N rounds of `-smp 2` and classifies each round by
  the **kernel clock** instead of by the screen: `BOOT` if `elapsed_ticks` grows,
  `FROZEN` if it is stuck in the low tens.
- `vm/install-kernel.sh` — installs a kernel image into a **disposable copy** of
  the fork master (boots `-smp 1`, scp, `sudo cp`, md5 verify, halt). Note: the
  guest answers SSH on the **virtio-net** forward (`VIOIF_PORT`), not on the e1000
  one, and `scp` needs `-P` (uppercase) for the port.

#### 2) Measured state of a frozen `-smp 2` guest

| observation | value |
|---|---|
| `elapsed_ticks` / `uptime` | **frozen at 12 / 0.120 s** |
| `master_cpu` | `0` |
| `curr_ipl[0]` / `curr_ipl[1]` | `0` / `0` — software IPL is **not** stuck high |
| `info pic` pin 2 (PIT) | `0x30` dest=0 vec=48, edge, **unmasked** — identical to the working `-smp 1` case |
| `info lapic` CPU0 | `LVTT 0x10030` masked/one-shot, `SPIV 0x1ff` (APIC **enabled**), `TPR 0x00`, `ISR (none)`, `IRR (none)` |
| `info lapic` CPU1 (AP) | `LVTT 0x20030 periodic`, `initial_count = calibrated_ticks` → the AP's own periodic LAPIC timer works |
| CPU0 `RIP` / `EFL` | `hpclock_read_counter`, **`IF = 1`** → a user-mode polling loop with interrupts enabled |

Hypotheses eliminated by measurement:

- **unbalanced `splhigh()`** in `start_other_cpus()` → **disproven**:
  `curr_ipl[0] = 0` and `EFL.IF = 1`;
- **`pit_udelay()` of the AP wait** → already dropped in the 4th pass (kernel B,
  `cpu_pause()`, still froze 6/6);
- **IOAPIC misprogramming** → **disproven**: pin 2 is unmasked, `dest=0`,
  `vec=48`, identical to the working case;
- **the PIT specifically is dead** → **disproven**: after `sendkey a` the IOAPIC
  *does* count IRQ1 (3 → 9), yet CPU0 keeps polling and the clock stays frozen
  ⇒ the BSP stopped accepting IOAPIC interrupts **in general**, not just the
  timer.

**Decisive clue**: attaching and detaching gdb **once** (a pure vCPU
pause/resume) took `elapsed_ticks` from **12 → 1013**; the clock then ran at
exactly 100 Hz and the guest booted all the way to `login:`. So the interrupt is
*pending but never re-injected* — a latched delivery that only a vCPU re-entry
clears. The only window in the entire AP start-up where the BSP cannot accept an
IOAPIC interrupt is the `lapic_disable()` … `lapic_enable()` one, and edges
arriving inside it are dropped.

#### 3) The fix

`src/gnumach` (branch `fork/x86_64`), `i386/i386/mp_desc.c` →
`start_other_cpus()`: drop the `lapic_disable()` / `lapic_enable()` pair around
the AP bring-up. `splhigh()` already blocks interrupts on the BSP for the whole
window, so masking the LAPIC buys nothing; and it never covered the APs anyway
(there is no execution on them yet). 17 insertions / 11 deletions, one function.

#### 4) Validation

Both kernels built with the pass-4 toolchain
(`--host=x86_64-pc-gnu --enable-ncpus=2`, `make gnumach`, `gzip -n -9`), each
installed into a disposable copy of the fork master:

| kernel | md5 of `gnumach-1.8-amd64-up.gz` | `-smp 2` result |
|---|---|---|
| baseline (HEAD, unpatched) | `0e9409e9bc034f817e7c6ca65147713c` | **0 BOOT / 3 FROZEN** (`elapsed_ticks` = 12) |
| **patched** | `98439c22ce31bbd58397eb333b515c23` | **12 BOOT / 0 FROZEN** |

- `smp-clock-check.sh`, 8 rounds patched (two independent batches of 4):
  `elapsed_ticks` +1007 per 10 s, i.e. **100.7 Hz**, `uptime` +10.0 s per 10 s —
  **no double ticking** (CPU0's LAPIC timer stays masked by design,
  `model_dep.c:931`, so the PIT remains the single clock source on CPU0).
- `trials-smp.sh`, 4 rounds patched (screen verdict + SSH control): **4/4 BOOT**,
  SSH reports **`nproc = 2`** and the patched kernel md5 — the AP is genuinely up,
  not merely "not hanging".
- **Regression** — patched kernel at `-smp 1`: **3/3 BOOT**, clock at 100 Hz.

⇒ The `-smp 2` hang is fixed, and the `-smp 1` reference is untouched.

#### 5) Reproduce (5th pass)

```sh
# 1. build the patched kernel (src = a checkout of fork/x86_64)
cd /tmp/gm/build && make gnumach
gzip -n -9 -c /tmp/gm/build/gnumach > /tmp/kP1.gz

# 2. install it into a DISPOSABLE copy of the fork master (never the master)
vm/install-kernel.sh /tmp/kP1.gz /home/ben/hurd-fork/vm/ovl-p5-fix.qcow2 2860

# 3. clock-based validation (KPROBE_ELF must match the kernel under test)
KPROBE_ELF=/tmp/gm/build/gnumach vm/smp-clock-check.sh \
  /home/ben/hurd-fork/vm/ovl-p5-fix.qcow2 4 50          # expect BOOT=4 FROZEN=0

# 4. screen verdict + SSH control (nproc must be 2)
SMP=2 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-p5-fix.qcow2 4

# 5. freeze a guest on purpose to re-observe the wedge, then heal it:
#    gdb attach/detach (or any vCPU pause) turns elapsed_ticks 12 -> 1013
```

#### 6) Notes / collateral findings

- **`info irq` does not count the PIT.** On a healthy guest running at 100 Hz the
  IOAPIC counters show only device lines (keyboard, serial, AHCI) and **no
  IRQ 0**; the keyboard line *does* move. Never conclude "the PIT is dead" from
  `info irq` — use `elapsed_ticks` / `uptime`.
- `vm/kprobe.py` must normalise the monitor's `\r` echoes before parsing: the HMP
  prompt and the value end up on the same `splitlines()` element, which silently
  dropped the readings depending on socket chunking.
- The fix reverts an **upstream** behaviour ("Disable IOAPIC interrupts … to
  prevent IOAPIC interrupts from interfering with SMP startup"), so it is worth
  reporting upstream: `splhigh()` already covers the stated intent on the BSP.

## Virtio storage (`virtio-blk` / `ld_virtio`) (17/10/2026) — recipe ready, not executable here

Goal: extend the virtio rump recipe (already OK for
`librumpdev_pci_if_vioif`) to **storage** (`librumpdev_virtio_ld`), then see
whether `rumpdisk` detects an `ld0` disk.

### Tooling state

- `patches/build/build-virtio.sh` only builds the **network**:
  `libpci_virtio` (`dev/pci/virtio.c` + `virtio_pci.c`) and
  `libvirtio_if_vioif` (`dev/pci/if_vioif.c`). **No** `ld_virtio`.
- The `ld_virtio` recipe **would be the same**: `build_lib libvirtio_ld
  $S/dev/pci/ld_virtio.c $R/dev/lib/component_simple.c` with
  `IONAME=LD_VIRTIO.ioconf`, plus the same fix (`<machine/cpu.h>` → rump
  + `-D_RUMPKERNEL`): the `curlwp`/`%gs` stall is identical (rump PCI code).
  The PCI front-end (`libpci_virtio` + `virtio_pci_glue.c`) is already there
  and is enough to attach `ld_virtio` to the `virtio` bus.
- **BUT** the build requires the **NetBSD 2025-01** source tree
  (`$HOME/nbsrc25` = `sys` + `common` + `rump`) **and** the `tooldir`
  (`config(1)` + cross gcc). That tree is **absent** from the host
  (`/home/ben/nbsrc25`, `~/rumplib`, `~/ovr`: nonexistent) **and** from the
  guest (`/home/demo/nbsrc25`: nonexistent ; only
  `~/hurd-build/rumpnet/rumpnet-vioif-test` remains).
  ⇒ **`ld_virtio` cannot be compiled in this environment.**

### Packages side (guest)

- Debian provides the rump **runtime** `0~20250111-7`:
  `librumpdev_ahcisata.so`, `librumpdev_ata.so`, `librumpdev_disk.so`,
  `librumpdev_pci.so`… but **no `librumpdev_virtio*`** (Debian disabled
  virtio on the rump side: "Fix virtio later"). `apt-cache search rump`
  confirms it: no `*virtio*` package (neither dev nor runtime). Hence the
  need for our in-house build (vioif).

### Secondary virtio disk test

- `vm/run-hurd64-vioif.sh` now accepts `BLK=1` (+ `BLKIMG`, `BLKSIZE`):
  adds `-drive file=$BLKIMG,if=none,id=vd0,format=raw -device
  virtio-blk-pci,drive=vd0`.
- Boot `-smp 1`, 64 MB disk attached:
  - QEMU sees the controller: `info pci` →
    `SCSI controller: PCI device 1af4:1001` (vendor `1af4` = virtio,
    `0x1001` = virtio-blk) ;
  - guest side: **no `/dev/ld0`** (`ls /dev/ld*` → absent) ; `rumpdisk`
    (Debian, without virtio) ignores the device.
- Conclusion: the disk is present at the PCI level but **no driver picks it
  up**. To get `ld0` one would need to **rebuild `rumpdisk`** (Hurd source
  available on the host) **linking in** a `librumpdev_virtio_ld` built with
  the NetBSD tree — blocked by the absence of that tree.

⇒ STEP 3: recipe identified and ready, **not executable here** (NetBSD
source dependency missing) ; QEMU `ld0` test negative (expected).

## INCIDENT (17/10/2026) — dev VM disk loss

- **Root cause**: re-extraction of `vm/debian-hurd-amd64-20260314.img`
  *while* `hurd-dev.qcow2` (overlay whose backing file is that image) was in
  use. The re-extracted tarball contained a different version → the backing
  changed → inconsistent disk view → fs corruption.
- **Symptom**: bootstrap panic
  `user_bootstrap: Cannot load user executable module (error code 6000): exec`
  = `EX_NOT_EXECUTABLE`. Culprit: `/lib/x86_64-gnu/ld-x86-64.so.1`
  (the `exec` module mounted by GRUB is actually `ld.so`) corrupted — its
  content had turned into an `ar` archive.
- **Attempts**: `e2fsck -y` (2 passes, fs back to "clean") +
  restoring `ld.so` from the pristine image → no more panic, but a definitive
  hang at bootstrap (ext2fs/exec servers do not start).
  ⇒ unrecoverable disk. Kept: `hurd-dev.qcow2.corrupt`,
  `hurd-dev.qcow2.fixed-corrupt`.
- **Lesson**: never re-extract/overwrite the base image of an overlay while
  the overlay exists; prefer **autonomous** copies (`qemu-img convert`)
  without a backing file.

## Dev VM rebuild (17/10/2026)

- Fresh **autonomous** disk: `qemu-img convert debian-hurd-amd64-20260314.img
  → hurd-dev.qcow2` (no more backing file).
- Network: `netdde`/e1000 (`10.0.2.15`), SSH by **key** (port 2222) ;
  `demo` (in `sudo`), password `demo`. `apt` OK.
- Rump artifacts recovered from the corrupt disk (debugfs):
  binary `.../rumpnet/rumpnet-vioif-test`, libs `/usr/lib/librump*.so*`
  (114 files), scripts. **glibc** must be ≥ **2.43** (the rump binaries
  require `GLIBC_2.43`) → `libc0.3` upgraded `2.42 → 2.43-7`.
- **vioif**: starts and attaches **only with 4 GB of RAM**
  (cf. § "vioif resolution" below). With 2 GB:
  `virtio0: virtqueue 0 for rx0 allocation failed, error code 1`.
- **Pure console / headless**: no more X11/XFCE (do not install
  `xfce4`/`xdm`/Xorg).

## vioif resolution / network switch (17/10/2026)

- **Boot-time attach = OK (chosen path).** `/etc/network/interfaces` =
  `scripts/interfaces.vioif`: `auto /dev/vioif0` +
  `pre-up /usr/local/sbin/start-rumpnet.sh` ; `eth0` as non-auto fallback.
  Validated after reboot on the dev VM: `ifconfig /dev/vioif0` = `10.0.2.15`,
  `ping` gateway + `8.8.8.8`, DNS, HTTPS (`wget` rc=0), SSH host `2223`.
  `start-rumpnet.sh` does start the `rumpnet-vioif-test` translator then the
  devnode `/dev/vioif0` (log `/tmp/start-rumpnet.log`).
- **4 GB mandatory.** With 2 GB the driver does not attach:
  `virtio0: virtqueue 0 for rx0 allocation failed, error code 1`.
  With 4 GB: `virtio0: interrupting at pausebreak`, IRQ OK, process alive.
  Hence `-m 4G` in `vm/run-*.sh`.
- **The HOT SWITCH makes the machine unusable.** Doing
  `ifconfig /dev/eth0 down` on a live system where rumpnet runs →
  kernel messages `irq handler [11]: release a dead delivery port …` then
  `irq handler [11]: still 1 unacked irqs in entry …` ; network dead **and**
  VGA console no longer echoing ; kernel in `machine_idle`
  (`RIP ffffffff8100c7fd`) → **reboot mandatory**. Reproduced on a
  disposable overlay.
  ⇒ `scripts/net-switch-vioif.sh` is **locked** (refused by default ;
  cf. the script header): no more hot switching, reboot instead.
- **Nuance / corrected diagnosis**: a plain `ifconfig /dev/eth0 down`
  *without* rumpnet active **does not** crash `pfinet` — it stays alive
  (verified on the console via `ps`). The "pfinet has crashed" diagnoses
  made over SSH were biased: cutting `eth0` also cuts the SSH session
  (which goes through `eth0`).
- **Lead A (fork Hurd binaries): dropped.** The old disk
  (`hurd-dev.qcow2.fixed-corrupt`) did contain **the whole fork Hurd**
  installed (`/hurd/*` + `/lib/x86_64-gnu` as of 2026-10-08), against the
  base Debian package `hurd 1:0.9.git20251029-7` (binaries of 2026-03-07) ;
  `netdde` is identical on both disks. But an A/B test (grafting the fork's
  `pfinet` + `libtrivfs/libfshelp/libports/libshouldbeinlibc` from the fork)
  **changes nothing** about the stall → not a `pfinet` bug but a hot IRQ
  teardown problem. The fork binaries are kept out of the repository in
  `~/hurd-fork/recovered/hurd-fork-20261008/` (see its `README.md`) but are
  **not needed** — the boot-time attach works with the base Debian binaries.

## Disk safety (permanent rules)

- **Never** boot `debian-hurd-amd64-*.img` in write mode.
- **Never** re-extract/overwrite an image used as a *backing file*.
- `vm/run-*.sh` only use an **autonomous** copy (`hurd-dev.qcow2`,
  `-m 4G`) ; `run-hurd64-smp.sh` and `run-hurd64-vioif.sh` additionally start
  on a **disposable overlay** (`hurd-*-test.qcow2`, recreated at each launch)
  for risky experiments → the dev disk is never touched.
- Backup of a working state: `hurd-dev.qcow2.good-20261017`
  (copy made with the VM cleanly shut down).

## References

- FOSDEM 2026 Thibault: x86_64 essentially complete (MIG RPC), ~75% Debian.
- Guix blog 01/03/2026: 64-bit Hurd, GCC 14, `hurd64-qcow2`, `kernel-arguments ("noide")`.
- `~/hurd-fork/vm/README-64bit:99-116` for network/disk.
