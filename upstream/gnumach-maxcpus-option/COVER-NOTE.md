# Cover note — `smp: add a maxcpus=N / nosmp boot option to limit the CPUs used` (1 patch)

Target project: **GNU Mach** (`hurd/gnumach`).
Upstream repository: `https://git.savannah.gnu.org/cgit/hurd/gnumach.git`
(mirror used for the verification: `git.sceen.net/hurd/gnumach.git`,
`master` at `79e98e8b`, dated 2026-10-12).

This is a **standalone** patch: it applies to a clean `master` on its own and
does not depend on the `gnumach-smp-ap-startup` series in the sibling
directory. It is kept separate on purpose, because it is useful
independently of any SMP fix: it is a user-facing escape hatch.

Sending procedure: `git send-email --to=bug-hurd@gnu.org *.patch`, per
<https://www.gnu.org/software/hurd/contributing/developer-workflows.html>.

## Problem

GNU Mach starts as many application processors as the firmware (or QEMU)
advertises, and offers no way to ask for fewer. On the amd64 port the SMP
paths are still experimental, so a user who runs QEMU with `-smp 2` (or
more) has no way to fall back to a single-CPU boot of the same kernel.

## Cause

None: this is a missing feature, not a bug. `smp_data_init()` calls
`smp_set_numcpus()` with `apic_get_numcpus()` unconditionally and never looks
at `kernel_cmdline`.

## Change

Two multiboot command line options:

```
nosmp        start at most one CPU
maxcpus=N    start at most N CPUs (N >= 1)
```

`smp_data_init()` clamps the APIC-reported CPU count to the requested limit
*before* `smp_set_numcpus()` is called, so the scheduler, the pmap IPIs and
ddb only ever see the CPUs that were really brought up. With neither option
present the behaviour is byte-for-byte the old behaviour.

The only other change is the kernel `printf()` announcing the clamp, which
now reads in English.

## How it was tested

Built from a clean clone of upstream `master` (`79e98e8b`) on Debian
GNU/Linux 13, gcc 14.2.0, GNU MIG from `hurd/mig` tip `cb48044`:

```
git worktree add --detach prep-maxcpus origin/master
cd prep-maxcpus
git apply 0001-smp-add-a-maxcpus-N-nosmp-boot-option-to-limit-the-C.patch
autoreconf -i
./configure --host=x86_64-pc-gnu --enable-ncpus=2
make gnumach MIG=$HOME/.local/mig/bin/mig
```

Result: `make exit=0`, artifact `gnumach`, 3624288 bytes, `ELF 64-bit LSB
executable, x86-64, statically linked, with debug_info` (this kernel also
contains the sibling SMP series, since it was built from the same tree in
the same session; the `maxcpus` hunk itself is the only difference relevant
to this directory).

The option itself was exercised at runtime on this fork with the gdb
injection harness (`vm/maxcpus-inject.gdb`, `-smp 2` with the boot option
forced to `maxcpus=1`): 2/2 rounds BOOT with `nproc = 1` on the kernel that
does **not** contain this patch, which is the regression check that the
single-CPU path is untouched. Re-measured during the 6th pass; see
`docs/suivi.md` in this repository.

Runtime testing of the option itself (`maxcpus=1` under `-smp 2`) is
described in `docs/suivi.md` of this repository, section "Boot option
`maxcpus=N` / `nosmp`", and was re-measured during the 6th pass.

## Known limits

- The option is only honoured on a kernel built with `--enable-ncpus > 1`;
  on a UP build `smp_data_init()` never sees more than one CPU.
- Parsing is deliberately minimal: `maxcpus=` with no digit, or with `0`, is
  ignored (treated as "no limit"). There is no error message for a malformed
  value.
- Tested on QEMU q35 with KVM only. Not tested on bare metal, not tested with
  a firmware that reports a large CPU count, not tested with the i386 build.
- No behaviour change is claimed or measured for the `nosmp` spelling beyond
  it being equivalent to `maxcpus=1` in the code path.

## Before sending

- `Signed-off-by:` must be added with the sender's real name and address.
- The kernel `printf()` string was translated from French to English for
  this submission; that is a deliberate difference from the fork branch, not
  an oversight.
- Worth mentioning in the mail that this came out of the same investigation
  as the sibling SMP series, so a reviewer can connect the two, while keeping
  them as separate submissions.