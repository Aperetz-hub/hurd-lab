# `upstream/` — patches prepared for the GNU/Hurd project

Nothing in this directory has been sent anywhere. These files are a
preparation for the repository owner to review and submit. Per the session
rules, no mail, no upstream push and no bug report was made from this
machine.

Each subdirectory is one **independent** submission: a different destination
project, or a different logical change that must not be bundled with another.
Every directory contains the patches in `git format-patch` output and a
`COVER-NOTE.md` with the problem, the cause, the change, the exact commands
used to test it and the exact results, and the known limits.

## Where the patches are meant to go

| directory | target project | patches | state |
|---|---|---|---|
| `gnumach-smp-ap-startup/` | GNU Mach (`hurd/gnumach`) | 3 | applies to current `master`, **built and booted** |
| `gnumach-maxcpus-option/` | GNU Mach (`hurd/gnumach`) | 1 | applies to current `master`, **built** |
| `hurd-rumpnet-bpf-hdr/` | GNU Hurd (`hurd/hurd`) | 1 | applies to current `master`, **not built** |
| `hurd-login-utmp-y2038/` | GNU Hurd (`hurd/hurd`) | 1 | applies to current `master`, **built** (compiled in the Hurd guest) |
| `hurd-rumpnet-pcn/` | GNU Hurd (`hurd/hurd`) | 1 | applies to current `master`, **not built** |

"Built" and "booted" refer to what was actually executed on this machine on
18/10/2026; each `COVER-NOTE.md` gives the commands and the outputs. Where a
patch is not built, its cover note says why and repeats the limitation in the
"Known limits" section rather than hiding it.

## The contribution procedure these were prepared against

Read on 2026-10-18:

- <https://www.gnu.org/software/hurd/contributing/developer-workflows.html>,
  section "Sending Patches":
  - "The easiest way to send patches is with `git send-email`", e.g.
    `git send-email -1 --to=bug-hurd@gnu.org`;
  - `--rfc` produces `[RFC PATCH]`, re-rolls get `-v2`, `-v3`, ...;
  - for a series, `git format-patch -N --to-bug-hurd@gnu.org --cover-letter
    --cc=...`, then `git send-email *.patch`;
  - "It's oftentimes a good idea to give an overview of a patch series; for
    this you use `--cover-letter`" — the `COVER-NOTE.md` files are written to
    be that overview;
  - "It is a good idea to cc the people who wrote or last changed the part of
    code you are changing."
- <https://www.gnu.org/software/hurd/rules/savannah_group.html>: "There is no
  need to be a formal member of the GNU Hurd Savannah group in order to be
  able to contribute to the system... Simply use the mailing lists for
  sending your contributions."
- <https://www.gnu.org/software/hurd/mailing_lists.html>: `bug-hurd@gnu.org`
  is described as "Technical discussion and bug reports; main development
  list. If you want to contribute, please meet us here."
- <https://www.gnu.org/software/hurd/rules.html> lists exactly two rules
  pages (source repositories, Savannah group). Neither states a copyright
  assignment requirement, a DCO requirement, or a patch formatting rule. The
  only mention of copyright assignment anywhere on the site that was found is
  on the *Artwork* page, and it is about artwork, not code. **No code
  copyright-assignment requirement could be verified**, so none is assumed
  here; `Signed-off-by:` is however left out of every prepared patch on
  purpose, because the identity to use is the sender's and not this
  repository's.

## What is deliberately *not* here, and why

- **Nothing for the rump / NetBSD side.** `rumprt net` uses
  `rumpdev_virtio_if_vioif` and `rumpdev_pci_virtio`, but those drivers are
  *not* written by this project: they live in the NetBSD tree, under
  `sys/rump/dev/lib/libvirtio_if_vioif` and `sys/rump/dev/lib/libpci_virtio`
  (both confirmed present in the NetBSD `trunk` branch on 2026-10-18). There
  is therefore nothing of ours to submit to rump or NetBSD.
- **The `curlwp` / `%gs` crash fix is not a patch either.** The crash came
  from the *build recipe*, which compiled the rump objects with the
  architecture's `<machine/cpu.h>` (`curlwp == x86_curlwp()`, a `%gs` access)
  instead of rump's own (`sys/rump/include/machine/cpu.h`,
  `curlwp == rump_curlwp_fast()`), and without `-D_RUMPKERNEL`. The fix is an
  include-order and `-D` change in `patches/build/build-virtio.sh` of this
  repository. NetBSD's `sys/rump/Makefile.rump` already does the right thing
  (rump include first, `-D_RUMPKERNEL` defined), so this is not an upstream
  defect; it is a defect of a recipe that builds rump out of tree for a
  foreign libc. It is documented, not submitted.
- **No virtio/`vioif` patch for the Hurd side.** Making `/dev/vioif0` the
  default interface needs rump libraries packaged for the Hurd, which Debian
  does not currently build; that is a Debian packaging question, not an
  upstream `hurd/hurd` change. The rumpnet `Makefile` change that adds the
  `pcn` library is the only part that belongs upstream.
- **No SMP patch is bundled with the `vioif` material**, and the SMP series
  is only included at all because the session's SMP track produced a fix that
  was verified by measurement before it (see `docs/suivi.md`, 6th pass).

## How to send one of these (sketch, for the owner)

```sh
cd upstream/hurd-rumpnet-pcn
# review the patch, add Signed-off-by: with your own identity, then:
git send-email --to=bug-hurd@gnu.org *.patch
```

For the series in `gnumach-smp-ap-startup/`, write the `COVER-NOTE.md`
content into the `--cover-letter` patch first:

```sh
cd upstream/gnumach-smp-ap-startup
git format-patch --cover-letter --to-bug-hurd@gnu.org <base>..<tip>
git send-email *.patch
```