# Cover note — `rumpnet: fix the sec/usec order of bpf_hdr on x86_64` (1 patch)

Target project: **GNU Hurd** (`hurd/hurd`).
Upstream repository: `https://git.savannah.gnu.org/cgit/hurd/hurd.git`
(mirror used for the verification: `git.sceen.net/hurd/hurd.git`, `master`
at `ccaa5374`, dated 2026-10-13).

Sending procedure: `git send-email --to=bug-hurd@gnu.org *.patch`, per
<https://www.gnu.org/software/hurd/contributing/developer-workflows.html>.

## Problem

`rumprt net` declares its own `struct bpf_hdr`, and on x86_64 the two time
fields are declared in the wrong order relative to the ABI the rump BPF
device actually writes.

## Cause

NetBSD's `struct bpf_hdr` starts with a `struct bpf_timeval bh_tstamp`, and
NetBSD's `struct bpf_timeval` is

```c
struct bpf_timeval {
	long tv_sec;
	long tv_usec;
};
```

(that declaration was read from `sys/net/bpf.h` on the NetBSD `trunk`
branch; see "Reference" below). rumpnet's local copy had `usec` first:

```c
struct bpf_hdr {
#if defined (__x86_64__)
  int64_t usec;
  int64_t sec;
#else
  int32_t usec;
  int32_t sec;
#endif
```

so any reader of the timestamp gets the microseconds where the seconds are
expected, and the seconds where the microseconds are expected.

`bh_caplen`, `bh_datalen` and `bh_hdrlen` are **not** affected: they follow
the timestamp in both layouts, so the offsets the translator relies on do
not move. This is why the bug has gone unnoticed — nothing in the Hurd reads
the timestamp back today. It becomes wrong the moment anything does.

## Change

Swap the two fields so the declaration matches the ABI. Four lines, no logic
change.

## How it was tested

```
# clean clone of upstream master
git worktree add --detach prep-hurd-bpf origin/master       # ccaa5374
cd prep-hurd-bpf
git apply 0001-rumpnet-fix-the-sec-usec-order-of-bpf_hdr-on-x86_64.patch
```

Result: the patch applies with no fuzz and no rejected hunk against upstream
`master` (which already contains `2d1094e1`).

Not compiled, and the reason is concrete rather than a formality: building
`rumpnet` needs the rump libraries and the NetBSD source tree
(`$HOME/nbsrc25`), and neither is present on this machine — the whole rump
side of the virtio work is documented as blocked on that in `docs/suivi.md`,
section "Virtio storage". The two errors a compile of `net-rump.c` would stop
on cannot even be reached, since the file includes rump headers that are not
installed.

## Known limits

- **Not runtime-tested.** Building `rumpnet` requires the rump libraries,
  which are not present on this machine (see `docs/suivi.md`, section "Virtio
  storage"), so this patch has not been compiled or run. It is a layout
  correction whose reference was read from NetBSD's own header.
- The patch fixes the *declaration*. If anything else in the Hurd or in a
  downstream package redefines `struct bpf_hdr`, that copy is not fixed by
  this patch.
- The same file was already patched upstream by `2d1094e1` ("rumpnet: Fix
  structure member widths for amd64"); this patch is orthogonal to that one
  and should apply alongside it (verified: it applies to a clean `master`,
  which already contains `2d1094e1`).

## Reference

`sys/net/bpf.h`, NetBSD `trunk` branch, read 2026-10-18:

```
struct bpf_hdr {
	struct bpf_timeval bh_tstamp;	/* time stamp */
	uint32_t	bh_caplen;
	uint32_t	bh_datalen;
	uint16_t	bh_hdrlen;
};
struct bpf_timeval {
	long tv_sec;
	long tv_usec;
};
```

## Before sending

- `Signed-off-by:` must be added with the sender's real name and address.
- Because it is untested at runtime, it should probably be sent together with
  a note offering to do the rump-side testing on a machine that has the
  NetBSD source tree, rather than presented as a tested fix.