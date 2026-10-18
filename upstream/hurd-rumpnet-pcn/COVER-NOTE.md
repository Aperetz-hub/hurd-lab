# Cover note — `rumpnet: link the pcn driver (AMD PCnet) too, not only wm` (1 patch)

Target project: **GNU Hurd** (`hurd/hurd`).
Upstream repository: `https://git.savannah.gnu.org/cgit/hurd/hurd.git`
(mirror used for the verification: `git.sceen.net/hurd/hurd.git`, `master`
at `ccaa5374`, dated 2026-10-13).

Sending procedure: `git send-email --to=bug-hurd@gnu.org *.patch`, per
<https://www.gnu.org/software/hurd/contributing/developer-workflows.html>.

## Problem

`rumprt net` can attach a `wm` network device and nothing else, even though
the rump kernel tree ships the AMD PCnet (`pcn`) driver as well. A user with
a PCnet NIC — which is what QEMU's `-device pcnet` gives, and what several
Hurd test setups use — has no working network.

## Cause

`rumpnet/Makefile` lists exactly one PCI interface driver in `RUMPLIBS`:

```make
RUMPLIBS = ... rumpdev_pci_if_wm ...
```

The rump kernel tree does provide the second one: `sys/rump/dev/lib/` contains
`libpci_if_wm`, `libpci_if_pcn`, `libpci_if_iwn` and `libpci_virtio`
(verified against the NetBSD `trunk` branch, see "Reference"), which build as
the rump libraries `rumpdev_pci_if_wm`, `rumpdev_pci_if_pcn`,
`rumpdev_pci_if_iwn` and `rumpdev_pci_virtio`.

Nothing in `rumpnet` hardcodes an interface name: the `if_*` drivers attach
through the rump autoconf machinery, so linking the extra library is all that
is needed.

## Change

Add `rumpdev_pci_if_pcn` to `RUMPLIBS`, after `rumpdev_pci_if_wm` so that
`wm` stays the default. One line.

## How it was tested

```
# clean clone of upstream master
git worktree add --detach prep-hurd-pcn origin/master        # ccaa5374
cd prep-hurd-pcn
git apply 0001-rumpnet-link-the-pcn-driver-AMD-PCnet-too-not-only-w.patch
```

Result: the patch applies with no fuzz and no rejected hunk against upstream
`master`.

Not built, for the same reason as the sibling rumpnet patch: the rump
libraries and the NetBSD source tree are absent from this machine, so
`make` in `rumpnet/` cannot even be configured here. In particular the claim
that linking `rumpdev_pci_if_pcn` is sufficient has **not** been
demonstrated by a build on a clean tree.

A QEMU `-smp 2` / `-device pcnet` boot with the resulting translator has been
run on this fork and the interface came up; see `docs/suivi.md` of this
repository. That test was made with the fork build, not with a clean upstream
tree plus this patch, so it is not a verification of the patch as submitted.

## Known limits

- **Not built or run here.** Building `rumpnet` needs the rump libraries,
  which are not present on this machine (`docs/suivi.md`, "Virtio storage"
  section: the NetBSD source tree the recipe needs is absent). So the
  statement "linking the library is all that is needed" is derived from
  reading `rumpnet`'s attach path and from the rump autoconf mechanism, not
  from a measured run on a clean tree.
- Only `pcn` is added. `iwn` (Intel wireless) and `virtio` are available in
  the same place and are left out on purpose, because the virtio case also
  needs `-lrumplib`-style packaging work and because the virtio bring-up on
  Hurd is still WIP.
- This is a link-time change only. It does not add a driver to Debian's
  package, and it does not change any behaviour for a machine that has a `wm`
  NIC.
- Upstream's contributing page currently lists "Implement a `rumpnet`. A
  first version was committed, with just the wm support is included" as an
  open TODO item, so this patch is a small step in a direction the project
  already lists.

## Reference

Contents of `sys/rump/dev/lib` on the NetBSD `trunk` branch, read
2026-10-18: `libpci_if_iwn`, `libpci_if_pcn`, `libpci_if_wm`,
`libpci_virtio`, `libvirtio_if_vioif`, `libvirtio_ld`, `libvirtio_viornd`,
`libvirtio_vioscsi`, among others.

## Before sending

- `Signed-off-by:` must be added with the sender's real name and address.
- Because it is not built here, the mail should either offer to test on a
  machine with the NetBSD tree, or be sent together with the two other
  rumpnet-related items so the reviewer can judge the whole picture.