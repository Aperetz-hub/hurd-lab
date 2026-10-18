# hurd-lab — GNU/Hurd x86_64 fork: SMP + rumpnet virtio

Experimental fork of GNU/Hurd (64-bit) with two goals:

1. **gnumach**: stop hanging the boot when APs fail to start
   (clean degradation to single-CPU instead of `while (1)`).
2. **rumpnet**: add **virtio-net** (`vioif`) network support — drivers
   `rumpdev_virtio_if_vioif` / `rumpdev_pci_virtio`, absent from the Debian
   packages — then make it the default network interface.

## Status (17/10/2026)

> **Pure console / headless mode — no X11.** This project **never** runs
> with Xorg, `xdm` or XFCE: everything happens in a text console + SSH. Do
> not install `xfce4`/`xdm`/Xorg on the VM.

- **virtio-net (`vioif`): functional at boot.**
  - `curlwp`/`%gs` crash fixed (interposition of rump's `<machine/cpu.h>` +
    `-D_RUMPKERNEL`, see `patches/build/`).
  - The VM boots on `/dev/vioif0` (MAC `52:54:00:12:34:58`): `ping` gateway
    + `8.8.8.8`, DNS, HTTPS (`wget`), SSH (host hostfwd `2223`) — OK.
  - Integrated **at boot**: `scripts/start-rumpnet.sh` (rumpnet translator +
    devnode `/dev/vioif0`, called as `pre-up`) + `scripts/interfaces.vioif`;
    `eth0`/netdde kept as fallback (not auto).
  - **`-m 4G` is mandatory**: with 2 GB the driver does not attach
    (`virtio0: virtqueue 0 for rx0 allocation failed, error code 1`).
  - **Do not hot-switch**: `ifconfig /dev/eth0 down` with rumpnet active
    freezes the machine (IRQ 11 unacknowledged, network + console dead,
    reboot required) → `scripts/net-switch-vioif.sh` is **locked**.
  - `glibc ≥ 2.43` required by the rump binaries.
- **SMP: fixed, still experimental.** `-smp 2` used to hang in the AHCI
  attach of the **`rumpdisk`** translator (`ahcisata0: 64-bit DMA`). Root
  cause: `start_other_cpus()` disabled the BSP's own local APIC while the APs
  were coming up, so the master CPU stopped receiving its 100 Hz tick; since
  `clock_interrupt()` advances time, timeouts and preemption only on the
  master CPU, the whole system clock froze and `rumpdisk` waited forever.
  Commit `4207ebda` (`fork/x86_64`) removes that window. Verified 18/10/2026:
  `-smp 2` boots **3/3** with `nproc = 2`, `-smp 1` boots 2/2, and
  `maxcpus=1` on the unfixed kernel still boots 2/2. Full autopsy and the
  measurements behind it: `docs/suivi.md` § *Autopsy of the `-smp 2` hang*
  and § *5th/6th pass*. **Not yet shown**: that the AP does useful work — no
  MP workload has been run, so "fixed" means "boots and stays usable with two
  CPUs", not "SMP is correct".

> **History.** The dev VM disk was corrupted on 17/10
> (re-extracting the base image while an overlay was in service) then
> rebuilt as an autonomous copy — see `docs/suivi.md` § *INCIDENT*.

Details, test methods and diagnostics: `docs/suivi.md` and the commit
messages.

## Contents

- `src/` — *not versioned here*: full clones of
  [`hurd/gnumach`](https://git.savannah.gnu.org/cgit/hurd/gnumach.git),
  [`hurd/hurd`](https://git.savannah.gnu.org/cgit/hurd/hurd.git) and
  [`hurd/mig`](https://git.savannah.gnu.org/cgit/hurd/mig.git).
  Work happens on `fork/x86_64` branches (pushed as-is to GitHub,
  see below).
- `vm/` — QEMU scripts (`-M q35`; e1000 + pcnet + virtio-net; SSH
  localhost:2222 e1000 / :2223 virtio).
- `patches/` — consolidated patches + rump build tooling (virtio) and the
  SMP test kernel (`gnumach-smp-test`, ELF with `debug_info`).
- `scripts/` — vioif boot integration: `start-rumpnet.sh`,
  `interfaces.vioif`, `net-switch-vioif.sh`.
- `riscv64/` — **experimental**: bare-metal `rv64gc` kernel skeleton for the
  QEMU `virt` board (OpenSBI + SBI console). Blueprint; not built yet.
- `docs/` — follow-up notes (`suivi.md`), plus `roadmap-riscv.md` for the
  experimental `riscv64/` track above.

## Published branches

- `main` — this project (docs, scripts, patches).
- `fork/gnumach-x86_64` — gnumach history + SMP commits.
- `fork/hurd-x86_64` — hurd history + commits (bpf_hdr, login utmp, pcn…).

## Quick start (Debian host)

```
sudo apt install qemu-system-x86 qemu-utils
# official Debian hurd-amd64 image (base, NEVER modified in place):
# https://cdimage.debian.org/cdimage/ports/latest/hurd-amd64/debian-hurd.img.tar.gz

# 1) create an AUTONOMOUS working copy (no backing file):
qemu-img convert -O qcow2 debian-hurd-amd64-*.img hurd-dev.qcow2

# 2) launch (pure console / headless, -m 4G, ssh localhost:2222):
./vm/run-hurd64.sh            # dev VM (autonomous disk hurd-dev.qcow2)
./vm/run-hurd64-smp.sh        # -smp 2 test on a disposable overlay — expected: failure
                              #   GDB=1 adds -gdb tcp::1234 ; STOP=1 adds -S ;
                              #   FRESH=0 reuses the overlay (kernel already installed)
./vm/run-hurd64-vioif.sh      # vioif test (network switch) on a disposable overlay
                              #   BLK=1 attaches a secondary virtio-blk disk (vd0)
```

> **Disk safety.** Never boot the base image `debian-hurd-amd64-*.img` in
> write mode, and never re-extract/overwrite it while an overlay references
> it as its *backing file* (this is what corrupted the disk on 17/10).
> For any risky experiment (network switch, SMP), use a disposable overlay
> (`hurd-*-test.qcow2`), or `qemu-img snapshot` first.

## VM images

Deliberately **excluded** from the repository (several GB): base Debian
image `debian-hurd-amd64-*.img` (read-only, do not modify) + autonomous
working copy `hurd-dev.qcow2` (see `.gitignore`).
