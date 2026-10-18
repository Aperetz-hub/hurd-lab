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

### 6th pass: re-verification of the `-smp 2` fix (18/10/2026)

**No kernel change was made in this pass.** Step 1.1 of the current session
required reproducing the hang before changing anything, and the reproduction
showed the documented state of this document was out of date: the `-smp 2` hang
is already fixed by the commit the 5th pass produced. Per the instruction to
stop and document a divergence, no further hypothesis was pursued.

**FACT.** Commands run, and their output:

```
# unfixed reference kernel, -smp 2
$ SMP=2 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-fork-master.qcow2 3
round smp2 #1 => WEDGE
round smp2 #2 => WEDGE
round smp2 #3 => WEDGE

# kernel of commit 4207ebda ("smp: do not disable the BSP local APIC while
# starting the APs"), -smp 2
$ SMP=2 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-p5-fix.qcow2 3
round smp2 #1 => BOOT   ssh: 2 98439c22ce31bbd58397eb333b515c23 /boot/gnumach-1.8-amd64-up.gz
round smp2 #2 => BOOT   ssh: 2 98439c22ce31bbd58397eb333b515c23 /boot/gnumach-1.8-amd64-up.gz
round smp2 #3 => BOOT   ssh: 2 98439c22ce31bbd58397eb333b515c23 /boot/gnumach-1.8-amd64-up.gz

# regression, same patched kernel at -smp 1
$ SMP=1 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-p5-fix.qcow2 2
round smp2 #1 => BOOT   ssh: 1 98439c22ce31bbd58397eb333b515c23 /boot/gnumach-1.8-amd64-up.gz
round smp2 #2 => BOOT   ssh: 1 98439c22ce31bbd58397eb333b515c23 /boot/gnumach-1.8-amd64-up.gz

# regression, unfixed kernel with maxcpus=1 injected (AP not started)
$ SMP=2 WAIT=85 vm/trials-smp.sh /home/ben/hurd-fork/vm/ovl-fork-master.qcow2 2 \
      maxcpus /tmp/gnumach-new
round maxcpus #1 => BOOT   ssh: 1 c81dfe4716a9216d6bf0c2047998cdf7 /boot/gnumach-1.8-amd64-up.gz
round maxcpus #2 => BOOT   ssh: 1 c81dfe4716a9216d6bf0c2047998cdf7 /boot/gnumach-1.8-amd64-up.gz
```

Kernel identities, read over SSH in the guest at boot time:

| overlay | kernel md5 | `nproc` at `-smp 2` | `nproc` at `-smp 1` |
|---|---|---|---|
| `ovl-fork-master.qcow2` | `c81dfe4716a9216d6bf0c2047998cdf7` (unfixed) | hangs 3/3 | boots, 1 CPU (unmeasured this pass) |
| `ovl-p5-fix.qcow2` | `98439c22ce31bbd58397eb333b515c23` (= commit `4207ebda`) | **boots 3/3, 2 CPUs** | boots 2/2, 1 CPU |

**Divergence from this document's earlier text.** The README `Status` section
still read "**SMP: not functional.** `-smp 2` = boot hangs", and the `Autopsy`
sections describe the hang as open. Both predate commit `4207ebda`. The README
has been corrected (see below); the autopsy sections are left as written
because they are the record of how the fault was found.

**Hypothesis status.**

| hypothesis | status | evidence this pass |
|---|---|---|
| BSP `lapic_disable()` around AP start-up starves the master CPU of its 100 Hz tick, which freezes time/timeouts/preemption and hangs `rumpdisk` | **confirmed** | removing it (commit `4207ebda`) turns 3/3 hangs into 3/3 boots with two live CPUs; reverting to the reference kernel restores 3/3 hangs |
| `-smp 1` is broken by the change | **refuted** | patched kernel boots `-smp 1` 2/2 |
| `maxcpus=1` / `nosmp` is broken by the change | **refuted** (not touched by it) | unfixed kernel still boots 2/2 with `maxcpus=1`; the commit does not touch that code path |
| the hang is caused by rumpnet/virtio rather than by AP start-up | **undecided**, untested | not addressed; the patched kernel boots to a working system with `vioif` up, which is consistent with either reading |

**Not verified in this pass.** The AP was observed to exist and run
(`nproc = 2`), but nothing here checks that it does useful work: no MP workload
was run, and no second CPU scheduling latency was measured. "Fix" here means
"the system boots with two CPUs and stays usable across a boot + SSH session",
not "SMP is correct".

**SPECULATION.** None offered for the remaining gap; closing it needs the
undecided test above, not an argument.

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

## Removal of the experimental component workspace (18/10/2026)

**FACT.** A second expansion path was deleted from the repository on
18/10/2026 with `git rm -r -f`: 17 tracked files removed, history not
rewritten. It was a workspace holding a translator-shaped component written in
a memory-safe systems language, with its foreign-function-interface bindings
generated from the Mach headers by a header-parsing tool. The last two
commits touching it are `b108fb5bb` (scaffolding) and `570531c15` (validated
on the host); the tree is recoverable with `git show 570531c15`.

What was in it, and why it is gone as stated:

| removed | reason |
|---|---|
| the bindings crate | machine-generated from the Mach headers. Never used by anything in this repository. |
| the example component | translator-shaped, host-only; never built for, or run in, the Hurd guest. |
| the workspace manifest, the toolchain pin and the build configuration | the workspace itself |

**FACT.** Layout verification against the real Mach headers was attempted on
18/10/2026 and produced a *negative* result worth recording, because it is the
reason the track is not worth carrying. Measured in the guest (Debian GNU/Hurd,
`libc0.3-dev 2:1.8+git20260224-4`, `gcc` on the guest) with
`printf '#include <mach/message.h>...' && gcc -o /tmp/sz /tmp/sz.c && /tmp/sz`:

```
sizeof(mach_msg_header_t)=32 align=8
  msgh_bits   @0  size=4
  msgh_size   @4  size=4
  msgh_remote @8  size=4
  msgh_local  @16 size=4
  msgh_seqno  @24 size=4
  msgh_id     @28 size=4
mach_port_t=4 natural_t=4 integer_t=4 mach_msg_size_t=4 mach_msg_id_t=4
mach_port_right_t=4 mach_port_seqno_t=4 mach_msg_type_t=8
mach_msg_type_name_t=4 mach_msg_option_t=4 mach_msg_timeout_t=4
```

The committed example component used a 24-byte, 6-field `RawHeader` with the
field order `size, remote, local, bits, size_rounded, id`, and carried a
`msgh_size_rounded` field that does not exist in the real
`mach_msg_header_t`. The header-parsing tool run against the same headers
emitted the 32-byte layout above and confirmed it with compile-time layout
assertions, so the hand-written header was wrong and would have misparsed every
message.

**FACT.** `libc0.3-dev 2:1.8+git20260224-4` ships a header set that does not
compile as a whole: on the guest, `gcc -c` on `#include <mach/mach.h>` fails
with

```
/usr/include/mach/mach_host.h:538:9: error: unknown type name
'processor_name_array_t'; did you mean 'processor_set_name_array_t'?
```

(the same failure is produced by clang on the host against the extracted
headers). `<mach/message.h>`, `<mach/mach_types.h>`, `<mach/port.h>`,
`<mach/kern_return.h>`, `<mach/mach_traps.h>` and `<mach/mig_errors.h>` each
compile standalone in the guest.

**PLAUSIBLE HYPOTHESIS.** The `msgh_size_rounded` field, the 24-byte header and
the field re-ordering look like a header written against a *different* Mach ABI
(upstream gnumach's own `<mach/message.h>` layout, or the pre-64-bit one), not
like an invented guess. This is not established: it was not traced to a source.

**SPECULATION.** No claim is made here about whether a memory-safe Hurd
translator is a good idea. The work only reached "a host-side skeleton that had
never spoken to Mach"; that is not evidence for or against the approach.

**FACT.** The documentation that referred to the removed workspace was
rewritten rather than deleted, so the analysis above stays readable:
the roadmap file was renamed to `docs/roadmap-riscv.md`, its sections about
the removed workspace were dropped, and the RISC-V material was renumbered.

## Upstream submission preparation (18/10/2026)

Nothing was sent outside this repository. No mail, no upstream push, no bug
report. `upstream/` holds patches in `git format-patch` output, one directory
per submission, each with a `COVER-NOTE.md`.

### Contribution procedure — what was actually verified, and where

| rule | source (read 2026-10-18) |
|---|---|
| `git send-email -1 --to=bug-hurd@gnu.org` is "the easiest way to send patches" | `gnu.org/software/hurd/contributing/developer-workflows.html`, section "Sending Patches" |
| `--rfc` gives `[RFC PATCH]`; a re-roll gets `-v2`, `-v3`, ... | same page |
| a series should use `git format-patch --cover-letter`, then `git send-email *.patch` | same page |
| "It's oftentimes a good idea to give an overview of a patch series" | same page |
| "It is a good idea to cc the people who wrote or last changed the part of code you are changing." | same page |
| no Savannah membership is required to contribute; use the mailing lists | `gnu.org/software/hurd/rules/savannah_group.html` |
| `bug-hurd@gnu.org` is "Technical discussion and bug reports; main development list. If you want to contribute, please meet us here." | `gnu.org/software/hurd/mailing_lists.html` |

**No copyright-assignment requirement could be verified.** The site's `Rules`
section (`gnu.org/software/hurd/rules.html`) lists exactly two pages, source
repositories and the Savannah group, and neither states one. The only
copyright-assignment mention found anywhere on the site is on the *Artwork*
page, and it concerns artwork, not code. No DCO rule and no patch-format rule
beyond `git format-patch` was found either.

The three source trees on this machine (`src/gnumach`, `src/hurd`,
`src/mig`) contain **no** `HACKING`, `CONTRIBUTING` or `HACKING`-like file
(checked with `find src -maxdepth 2 -iname 'HACKING*' -o -iname 'CONTRIBUT*'`
— no output). Their `README` files only point at `bug-hurd@gnu.org` and the
Savannah bug tracker.

The four repositories named by the site's own pages all answer HTTP 200:
`hurd/hurd.git`, `hurd/gnumach.git`, `hurd/mig.git`, `hurd/incubator.git`
(checked with `curl -sL -o /dev/null -w '%{http_code}'`).

### FACT — inventory, and where each candidate stands

Upstream tips used for every "applies cleanly" statement below, fetched this
session with `git fetch origin`:

| project | mirror used | tip | date |
|---|---|---|---|
| `hurd/gnumach` | `git.sceen.net/hurd/gnumach.git` | `79e98e8b` "Fix copyrights" | 2026-10-12 |
| `hurd/hurd` | `git.sceen.net/hurd/hurd.git` | `ccaa5374` "ext2fs: Write JBD2 revoke records for freed blocks" | 2026-10-13 |

| candidate | origin commit | destination | applies on current master? | built? | run? |
|---|---|---|---|---|---|
| maxcpus/N + nosmp boot option | `a4237652` | gnumach | yes, alone | yes | option exercised on the fork (2/2 BOOT, `nproc=1`) |
| do not disable the BSP LAPIC | `4207ebda` | gnumach | yes, alone | yes | yes, see below |
| do not hang the boot on AP failure | `99a21a39` | gnumach | yes, **after** `99a21a39`'s predecessor — see dependency note | yes | yes, same kernel |
| propagate the degradation | `c11e6cea` | gnumach | only after `99a21a39` (verified: fails alone, applies after) | yes | yes, same kernel |
| `bpf_hdr` sec/usec order | `1e025470` | hurd | yes, alone | **no** | no |
| `login` utmp Y2038 saturation | `b6ec77a4` | hurd | yes, alone | yes (in the guest) | yes (the hunk, run) |
| rumpnet links `pcn` too | `515defcb` | hurd | yes, alone | **no** | no |

The dependency is real and was measured, not guessed: `git apply --check`
of `c11e6cea` against a clean `origin/master` fails
(`i386/i386/mp_desc.c:323`), and the same patch applies immediately after
`99a21a39`. In the exported series the order is `4207ebda`, `99a21a39`,
`c11e6cea`, which is also verified to apply in that order.

The exported patches were re-checked with `git am` (not `git apply`) on
fresh worktrees of the two upstream tips: the 3-patch gnumach series, the
1-patch gnumach series and the three 1-patch hurd series all apply cleanly.

### FACT — the gnumach SMP series, built and booted on upstream master

Both kernels were built **from the same upstream commit `79e98e8b`** on this
machine, with the same commands, so the pair is a real before/after:

```
autoreconf -i
./configure --host=x86_64-pc-gnu --enable-ncpus=2
make gnumach MIG=$HOME/.local/mig/bin/mig      # make exit=0, both times
```

(`mig` had to be built first: `src/mig` at `cb48044`, installed in
`$HOME/.local/mig`, built with
`make TARGET_CPPFLAGS="-I/home/ben/hurd-fork/hurd-headers/x86_64-gnu -I/home/ben/hurd-fork/hurd-headers/usr"`
because its `cpu.symo` rule preprocesses a file that includes
`<mach/message.h>`.)

Both kernels were installed into **separate disposable overlays**
(`ovl-up-gnumach.qcow2` and `ovl-up-base.qcow2`, each a fresh copy made by
`vm/install-kernel.sh`; the fork master overlay and the dev disk were not
touched) and run with `vm/trials-smp.sh` under `vm/run-hurd64-smp.sh`
(`-m 4096`, QEMU q35, KVM):

| kernel | `-smp 2` | `-smp 1` |
|---|---|---|
| `origin/master` `79e98e8b`, unpatched (gz md5 `da4aae72…`) | **0/3 BOOT**, 3 × WEDGE | 2/2 BOOT, `nproc=1` |
| `origin/master` + the 3 patches (gz md5 `c6a15338…`) | **3/3 BOOT**, `nproc=2` | 2/2 BOOT, `nproc=1` |

On every BOOT round the harness reads `nproc` and
`md5sum /boot/gnumach-1.8-amd64-up.gz` over SSH inside the guest, and the md5
matched the uploaded kernel in every case, so the kernel that booted is the
one that was built from the patched tree.

### FACT — the hurd `login` patch, compiled and run in the guest

The whole `utils/login.c` cannot be compiled on this guest, because the
installed Hurd `libc0.3-dev 2.43-7` header set does not compile as a whole:
`<hurd.h>` pulls in `/usr/include/mach/mach_host.h`, which fails with

```
/usr/include/mach/mach_host.h:538:9: error: unknown type name
'processor_name_array_t'; did you mean 'processor_set_name_array_t'?
/usr/include/mach/mach_host.h:1092:9: error: unknown type name
'processor_name_array_t'; did you mean 'processor_set_name_array_t'?
```

This is pre-existing and not caused by the patch: compiling the **unpatched**
`utils/login.c` from upstream `master` with the identical command line gives
the same 2 errors and nothing else.

The patched hunk was therefore isolated against the guest's real types,
compiled with `-Wall -Wextra` (no warning) and executed:

```
INT32_MAX = 2147483647
tv_sec=1700000000  old=1700000000  new=1700000000
tv_sec=2147483648  old=-2147483648  new=2147483647
tv_sec=4102444800  old=-192522496  new=2147483647
```

Measured on the same guest, which is what makes the premise true:

```
sizeof(utmp.ut_tv)     = 8      (two 32-bit fields)
sizeof(struct timeval) = 16     (two 64-bit fields)
```

### FACT — what is on the rump/NetBSD side

There is **nothing to submit to rump or NetBSD**. Verified this session:

- the NetBSD tree on this machine (`$HOME/nbsrc25`, the tree
  `patches/build/build-virtio.sh` needs) **does not exist**; no `sys/rump`
  directory exists anywhere under `/home/ben`;
- `libvirtio_if_vioif` and `libpci_virtio` are NetBSD's own files, not
  ours: the contents of `sys/rump/dev/lib` on the NetBSD `trunk` branch
  include `libpci_if_iwn`, `libpci_if_pcn`, `libpci_if_wm`, `libpci_virtio`,
  `libvirtio_if_vioif`, `libvirtio_ld`, `libvirtio_viornd`,
  `libvirtio_vioscsi`;
- the `curlwp` / `%gs` crash was a defect of *our build recipe*, not of rump:
  NetBSD's `sys/rump/Makefile.rump` already puts the rump include directory
  first and defines `-D_RUMPKERNEL`, which is what makes
  `curlwp == rump_curlwp_fast()` instead of the `%gs`-based
  `x86_curlwp()`. The fix lives in `patches/build/build-virtio.sh` of this
  repository and is documented, not submitted.

So the two rumpnet patches here are both **Hurd** changes (`rumpnet/Makefile`,
`rumpnet/net-rump.c` are in `hurd/hurd`), and they are kept in separate
directories because they are unrelated fixes.

### PLAUSIBLE HYPOTHESIS

The `bpf_hdr` sec/usec swap is very likely a real defect rather than a
cosmetic one, because NetBSD's `sys/net/bpf.h` was read directly and declares
`struct bpf_timeval { long tv_sec; long tv_usec; }` as the first member of
`struct bpf_hdr`. What has **not** been established is that anything ever
reads that timestamp back on the Hurd; nothing in the tree does today, which
is why the bug is latent rather than observable.

### SPECULATION

Whether upstream would prefer the `maxcpus`/`nosmp` option as a kernel
boot-line option, as a `configure` option, or not at all, is not something
this session can determine. It is offered as a boot-line option because that
is what was built and measured here.

### Left for the owner

- add `Signed-off-by:` with a real identity to all six patches (deliberately
  omitted here: the identity is the sender's, not this repository's);
- review the two `rumpnet` patches knowing they were not built;
- decide the submission order and whether to send the three gnumach SMP
  patches as one series or patch 1 alone;
- actually send them. This session did not.

## SMP + vioif0 MP stress test (18/10/2026)

This closes the two items the 6th pass left open: whether `-smp 2` with
`vioif0` boots and passes traffic, and whether both CPUs do concurrent work
under load without wedging `rumpdisk`, `rumpnet` or the master clock.

### FACT — the setup

- The kernel under test was built from the fork branch `fork/x86_64` at
  `4207ebda` ("smp: do not disable the BSP local APIC while starting the APs"),
  with `--host=x86_64-pc-gnu --enable-ncpus=2`. Its compressed image has
  md5 `ef48c06764b054f35cd1d06dfc495828`; the ELF with symbols, used by
  `vm/kprobe.py`, is `/tmp/gnumach-fork-4207ebda`.
- The guest is a **full copy** of the dev disk
  (`cp hurd-dev.qcow2 hurd-smp-vioif-test.qcow2`), so the dev disk was never
  opened for writing and never even used as a backing file. Its mtime is
  unchanged (17/10 13:51), as is `ovl-fork-master.qcow2` (17/10 22:50) and
  `debian-hurd-amd64-20260314.img` (24/03/2026).
- The guest already carried the `vioif0` configuration: `/etc/network/interfaces`
  with `auto /dev/vioif0` and `pre-up /usr/local/sbin/start-rumpnet.sh`, the
  `rumpnet-vioif-test` translator and the `devnode` on `/dev/vioif0`.
- QEMU: `-m 4096 -M q35 --enable-kvm -smp 2`, with an e1000 (SSH on 2222) and
  a `virtio-net-pci` (SSH on 2223). The guest is reached over the virtio
  device, which is the one `vioif0` runs on.

**FACT.** The guest at `-smp 2`:

```
$ ssh -p 2223 demo@127.0.0.1 'nproc; sudo -n md5sum /boot/gnumach-1.8-amd64-up.gz'
2
ef48c06764b054f35cd1d06dfc495828  /boot/gnumach-1.8-amd64-up.gz
$ ssh -p 2223 demo@127.0.0.1 'ifconfig /dev/vioif0'
/dev/vioif0 (2):
  inet address  10.0.2.15
  netmask       255.255.255.0
  ...
  flags UP BROADCAST RUNNING ALLMULTI MULTICAST
  hardware addr 52:54:00:12:34:57
```

So `-smp 2` **and** `vioif0` boot together on the patched kernel. Before the
stress, traffic over `vioif0` was checked on its own: 3/3 packets to the
gateway `10.0.2.2` and 3/3 to `8.8.8.8`, 0 % loss.

### FACT — the workload

`vm/smp-stress-run.sh` boots the disposable overlay, checks the guest, and
runs `vm/smp-stress.sh` inside it for 60 s. Four things run at once:

| component | what it does |
|---|---|
| worker A | a CPU-bound loop (`vm/smp-stress-burn.c`) on the master processor set |
| worker B | the same program on the **slave** processor set, via `sudo /sbin/smp` |
| disk I/O | 60 rounds of write 256 KiB + `fsync` + read back, on the rumpdisk-backed root filesystem |
| network | `ping` to the gateway every 0.2 s and to `8.8.8.8` every 0.5 s, over `vioif0` |

The slave worker is the interesting one: `/sbin/smp` is setuid root and
assigns the task to the slave processor set, so worker B really runs on the
second CPU rather than merely being eligible for it.

### FACT — the results (60 s window, `-smp 2`, `vioif0` up)

Two independent runs, the second with the improved clock sampler:

| measurement | run 1 | run 2 |
|---|---|---|
| worker A (master) | 11 060 500 000 iterations in 60.00 s | 11 300 290 000 iterations in 60.00 s |
| worker B (slave) | 11 778 440 000 iterations in 60.00 s | 11 812 380 000 iterations in 60.00 s |
| rumpdisk write+sync+read rounds | 60 | 60 |
| ping to the gateway | 283 sent, 282 received, **0 % loss** | 287 sent, 286 received, **0 % loss** |
| ping to `8.8.8.8` | 117 sent, 117 received, **0 % loss** | 118 sent, 118 received, **0 % loss** |
| load average at the end | 2.12 2.04 1.68 | 1.97 2.04 1.65 |

Both workers ran the full 60 s and both did substantial work. The slave did
about 5 % *more* iterations than the master, in both runs; the master also
carries the kernel-side work of the stress (the rumpnet translator, `pfinet`,
the ext2fs), which is the obvious candidate for the difference, but that was
not measured.

### FACT — the clock did not freeze

The kernel clock was sampled from the outside, through the QEMU monitor, with
`vm/kprobe.py`, which reads the kernel globals straight out of guest memory.
Every sample is stamped with the **host** wall clock, which is completely
independent of the guest, so the difference between two samples is a
reliable time base.

```
label      host time             ticks     uptime
before     1792316254.110         2246      22.46
during-1   1792316264.196         3256      32.56
during-2   1792316274.296         4272      42.72
during-3   1792316284.477         5284      52.84
during-4   1792316294.595         6298      62.99
during-5   1792316304.738         7312      73.12
during-6   1792316314.867         8322      83.22
after      1792316315.250         8361      83.61

WHOLE WINDOW: dhost=61.140 s  dticks=6115  duptime=61.15 s
  tick rate  = 100.017 ticks/s  (nominal 100)
  uptime/host= 1.00017
  ticks per uptime second = 100.0000
```

Per interval, the tick rate was 100.148, 99.904, 99.982, 100.526, 99.922
and 99.858 ticks/s. There is no interval in which the clock stopped.

**FACT.** A second, independent trace was taken *inside* the guest, by
`vm/smp-stress-clock.c`, which samples three clocks in a tight loop with no
forks: the C library's `CLOCK_MONOTONIC`, `CLOCK_REALTIME`, and the kernel's
own `/proc/uptime`. Over 551 samples at 9.1 Hz:

```
CLOCK_MONOTONIC span = 60.5750 s
CLOCK_REALTIME  span = 60.5750 s
kernel uptime   span = 60.5700 s
uptime/monotonic (aggregate) = 0.999917
the kernel uptime never stopped advancing
```

**FACT.** The first version of the in-guest sampler was a shell loop that
ran `date` and `cat /proc/uptime` once a second. Its trace was **not** used,
because it was measuring its own overhead: a fork and an exec under that load
cost tens of milliseconds, and the resulting ratios swung between 0.54 and
1.14. The per-sample scatter that remains in the fork-free trace (0.75 to
1.30 between consecutive samples) has the same origin, since the sampler
reads the clocks *around* the `/proc/uptime` read and a slow read shifts the
pairing; the aggregate over 551 samples is the meaningful figure.

### PLAUSIBLE HYPOTHESIS

- The slave worker doing ~5 % more work than the master is consistent with
  the master carrying the kernel-side work of the stress (rumpnet, `pfinet`,
  the ext2fs writes) on top of its own burner. This was not measured: the
  per-CPU time was not broken out.
- The absence of any packet loss is consistent with the rumpnet path keeping
  up with the offered load, but the offered load was 5 packets per second to
  the gateway. A much heavier network load was not tried.

### SPECULATION

Nothing is claimed about the correctness of the Hurd userland under SMP. Two
CPUs ran concurrent work and the clock held; that is not the same as the
userland being race-free. The same caveat as before applies: no MP workload
of the user's own was run, and the AP was not exercised beyond this.

### What is still open

- Whether a *heavy* network load (saturating the virtio queues) keeps
  `rumpnet` and the master clock healthy; 5 packets per second does not.
- Whether the slave CPU keeps its 100 Hz tick of its own, or whether the
  tick is genuinely master-only as the design implies.
- Whether the ext2fs/rumpdisk path is safe under a much larger write
  volume than 15 MiB per minute.

## RISC-V skeleton: milestone 3, physical memory and the Sv39 MMU (18/10/2026)

Follows the milestone 1 and 2 entries above; the toolchain, the SBI
convention and the trap vector are described there and are not repeated.

Milestone 3 is: a physical page allocator, Sv39 page tables with per-section
permissions, the MMU switched on, and the five supervisor timer interrupts
still completing with the MMU live. The success criterion is met.

### FACT — the drift-free rearm, measured against the old scheme

The old rearm scheduled the next interrupt as `rdtime() + interval`, so every
tick was late by however long the handler took, and the lateness accumulated.
The new rearm advances the deadline by exactly one interval:

```c
if (next_deadline + interval <= now)
        next_deadline = now + interval;   /* fell too far behind */
else
        next_deadline += interval;
```

Both schemes were built from the same tree and measured the same way: the
average period over the five ticks, with an interval of 100000 ticks.

| scheme | average period over 5 ticks, three runs |
|---|---|
| `now + interval` (old, drifting) | 100989, 100824, 101046 — always *above* the target |
| `next_deadline += interval` (new) | 99214, 99226, 99730 — at or below, converging to it |

The boot log also prints the offset of the deadline schedule, which shows
the schedule itself is exact: the offsets advance by exactly 100000 every
tick (`+96360`, `+196360`, `+296360`, `+396360`). The constant part is the
lateness of the first tick, which is large because the MMU switch-on
happens between the arming and the first interrupt.

### FACT — the physical page allocator

`src/pmm.c` hands out 4 KiB pages from a fixed pool of 64 pages that the
linker script reserves between `_kernel_end` and `_pmm_pool_end`. Every page
is handed out 4 KiB aligned and zeroed.

The kernel takes one page and hands it straight back, and says so:

```
pmm: pool 64 pages of 4 KiB
pmm: allocated page at 0x80214000, aligned=0x1, free now 63
```

The pool size is fixed in two places — `PMM_POOL_PAGES` in `include/pmm.h`
and `PMM_POOL_PAGES` in the linker script — so it cannot be checked with a
`_Static_assert`: the linker symbols are not constant expressions in C. The
kernel therefore reports the size the script actually reserved, and a
mismatch is visible at boot.

### FACT — the Sv39 page tables and the MMU

The linker script now pads the section boundaries to 4 KiB page boundaries,
which is what makes strict per-section permissions possible:

```
$ riscv64-linux-gnu-nm build/hurd-riscv64-boot.elf | grep -E '__text_start|__text_end|__rodata_start|__rodata_end|__data_start|_kernel_end|_pmm_pool_end'
0000000080200000 T __text_start
0000000080202000 R __rodata_end
0000000080202000 R __rodata_start
0000000080202000 R __text_end
0000000080203000 D __data_start
0000000080203000 D __rodata_end
0000000080214000 B __stack_top
0000000080214000 B _kernel_end
0000000080254000 B _pmm_pool_end
```

`src/vm.c` builds a three-level Sv39 table and switches the MMU on with
`satp = (8 << 60) | root_ppn` followed by `sfence.vma`:

```
vm: root table ppn 0x80215, 85 pages mapped, 5 pool pages used
vm: Sv39 active, satp 0x8000000000080215
```

The mapping is an identity mapping, because the kernel keeps running at its
physical address; what the tables buy is permissions:

| region | permissions |
|---|---|
| `[__text_start, __text_end)` | `R-X` |
| `[__rodata_start, __rodata_end)` | `R--` |
| `[__data_start, _kernel_end)` | `RW-` |
| `[_kernel_end, _pmm_pool_end)` | `RW-` |
| `[0x1000_0000, 0x1000_1000)`, the UART | `RW-` |

**FACT.** `.data` and `.bss` are deliberately not separated by a page gap.
They hold the small-data objects that the RISC-V `gp` convention addresses
relative to `__global_pointer$`, and keeping them adjacent preserves that.
The assembler resolves the distance and refuses to relax an access that does
not fit, so a mistake there is a link error rather than a wrong address.

### FACT — the whole milestone-3 boot

QEMU 10.0.13 `virt`, `-m 256M`, `-bios default`:

```
hurd-riscv64: console up (rv64gc, QEMU virt)
SBI hart id 0x0
!
SBI legacy_putc works: yes
SBI spec version 2.0 (error 0, value 0x2000000)
SBI impl id 0x1 (error 0)
SBI impl version 0x10006 (error 0)
SBI extension probe base eid 0x10 -> 1 (error 0)
SBI extension probe legacy console eid 0x1 -> 1 (error 0)
SBI extension probe time eid 0x54494d45 -> 1 (error 0)
SBI wall clock 0 (error 0)
hurd-riscv64: milestone 1 reached
pmm: pool 64 pages of 4 KiB
pmm: allocated page at 0x80214000, aligned=0x1, free now 63
firmware stvec was 0x80200000
installed stvec 0x80201d20
hurd-riscv64: milestone 2 reached
timer armed by stimecmp CSR at 911911 (SBI sbi_timer_set_stimecmp error -2)
vm: root table ppn 0x80215, 85 pages mapped, 5 pool pages used
vm: Sv39 active, satp 0x8000000000080215
timer tick 1 at 915551
timer tick 2 at 1012955 (period 97404 ticks, deadline +96360)
timer tick 3 at 1112692 (period 98570 ticks, deadline +196360)
timer tick 4 at 1215265 (period 99904 ticks, deadline +296360)
timer tick 5 at 1312438 (period 99221 ticks, deadline +396360)
hurd-riscv64: milestone 3 reached, halting
```

The UART output and the five interrupts both complete *after* the MMU is on,
which is the point: the identity mapping, the permissions and the explicit
UART mapping are all exercised by the lines above.

### FACT — the self tests

`make selftest` builds two variants and checks both fault paths.

```
  selftest exception: PASS
    selftest: raising an illegal instruction
    selftest: fault as expected, cause 2, sepc 0x802012f6
    selftest: PASS
  selftest rodata: PASS
    selftest: storing to .rodata at 0x80202000
    selftest: fault as expected, cause 15, sepc 0x80201354
    selftest: PASS
```

The second one is the one that matters for this milestone: a store to the
read-only data raises a **store/AMO page fault, `scause = 15`**, which is
only possible if the `R--` permission on that page is real. Had the
permission been wrong, the store would have succeeded and the test would
have failed by reaching the code after it.

`make verify`, which boots at `-smp` 1 to 4, three runs each, and requires
both the milestone 2 and the milestone 3 banner exactly once per run:

```
  -smp 1 : BOOT, banner x1 on 3/3 runs
  -smp 2 : BOOT, banner x1 on 3/3 runs
  -smp 3 : BOOT, banner x1 on 3/3 runs
  -smp 4 : BOOT, banner x1 on 3/3 runs
```

### PLAUSIBLE HYPOTHESIS — recorded, not relied on

- The excess of the old scheme over the 100000-tick interval is attributed
  to the handler latency, on the grounds that the rearm was computed from
  the time read at the top of the handler. The measurement is consistent with
  that, but the latency itself was not measured separately.
- The first interrupt is late by about 3640 ticks in the log above (armed at
  911911, first interrupt at 915551). That is attributed to the MMU switch-on
  happening between the arming and the first interrupt, since the boot log
  shows the page table construction and the console output in between. It
  was not measured directly, and the attribution is an explanation, not an
  observation.

### SPECULATION

Nothing is claimed about user space, about mapping the rest of DRAM, about
the device tree, or about starting further harts. None of it is written.

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
