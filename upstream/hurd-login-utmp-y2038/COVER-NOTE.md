# Cover note — `login: saturate the utmp time conversion instead of truncating it (Y2038)` (1 patch)

Target project: **GNU Hurd** (`hurd/hurd`).
Upstream repository: `https://git.savannah.gnu.org/cgit/hurd/hurd.git`
(mirror used for the verification: `git.sceen.net/hurd/hurd.git`, `master`
at `ccaa5374` "ext2fs: Write JBD2 revoke records for freed blocks", dated
2026-10-13).

Sending procedure: `git send-email --to=bug-hurd@gnu.org *.patch`, per
<https://www.gnu.org/software/hurd/contributing/developer-workflows.html>.

## Problem

On a 64-bit Hurd, `login` writes a wrapped (negative) timestamp into the
utmp database for any login after 2038-01-19, so `lastlog` and anything
else reading `utmp.ut_tv` reports a date before 1970.

## Cause

glibc declares `struct utmp::ut_tv` as a pair of **32-bit** fields so that
the on-disk utmp format stays identical between 32-bit and 64-bit, while
`struct timeval::tv_sec` is a `time_t`. Commit `69905110` ("x86_64: utmp
uses int32_t to store time so use a temporary variable", Flavio Cruz,
2023-12-30) already fixed the real bug that was there before it — copying a
64-bit `struct timeval` into the smaller `utmp.ut_tv` — by assigning field by
field. The assignment it introduced is still a silent truncation:

```c
  utmp.ut_tv.tv_sec = current_time.tv_sec;
```

## Change

Saturate at `INT32_MAX` rather than wrapping, keeping the field-by-field
assignment so the size mismatch can never be reintroduced. `<stdint.h>` is
included for `INT32_MAX`. Nothing else is touched.

## How it was tested

```
# clean clone of upstream master
git worktree add --detach prep-hurd-utmp origin/master     # ccaa5374
cd prep-hurd-utmp
git apply 0001-login-saturate-the-utmp-time-conversion-instead-of-t.patch
```

Result: the patch applies with no fuzz and no rejected hunk.

The guest itself could not be used to compile the whole file: the Hurd
`libc0.3-dev` header set shipped by this distribution does not compile as a
whole, and `<hurd.h>` pulls in `<mach/mach_host.h>` which fails with

```
/usr/include/mach/mach_host.h:538:9: error: unknown type name
'processor_name_array_t'; did you mean 'processor_set_name_array_t'?
```

This is **pre-existing and unrelated to the patch**: compiling the unpatched
`utils/login.c` from upstream `master` with the identical command line
produces exactly the same two errors and nothing else.

```
# in the guest (Debian GNU/Hurd, glibc 2.43, gcc 15.2.0), both files:
gcc -D_GNU_SOURCE -c -I. -Ilibshouldbeinlibc -o /tmp/login.o utils/login.c
    -> 2 errors, both /usr/include/mach/mach_host.h
gcc -D_GNU_SOURCE -c -I. -Ilibshouldbeinlibc -o /tmp/login-master.o login-master.c
    -> 2 errors, both /usr/include/mach/mach_host.h
```

So the patch introduces no new diagnostic. The hunk itself was then
verified against the guest's real types, compiled with `-Wall -Wextra`
(no warning) and run:

```
INT32_MAX = 2147483647
tv_sec=1700000000  old=1700000000  new=1700000000
tv_sec=2147483648  old=-2147483648  new=2147483647
tv_sec=4102444800  old=-192522496  new=2147483647
```

i.e. identical output before 2038, saturation instead of wrap-around after.
The premise was measured on the same machine:

```
sizeof(utmp.ut_tv)     = 8      (two 32-bit fields)
sizeof(struct timeval) = 16     (two 64-bit fields)
sizeof(ut_tv.tv_sec)   = 4
sizeof(timeval.tv_sec) = 8
```

Behaviour before 2038-01-19 is unchanged by construction: the saturation
branch is only taken when `current_time.tv_sec > INT32_MAX`, and
`tv_usec` is untouched.

## Known limits

- The saturation is a one-line policy choice: a 64-bit utmp format does not
  exist in glibc, so clamping is the only alternative to wrapping. If glibc
  ever grows one, this patch should be revisited.
- The patch only affects `add_utmp_entry()` in `utils/login.c`. Other writers
  of utmp records in the tree have not been audited for the same truncation.
- No test in the Hurd test suite covers `login`'s utmp handling, so there is
  no regression test to run.

## Before sending

- `Signed-off-by:` must be added with the sender's real name and address.
- This builds directly on `69905110`, so it should be posted as a short
  follow-up rather than a standalone rework; mentioning `69905110` in the
  mail body will help the reviewer.
- As with the sibling patches, the commit message was translated from French
  to English for this submission.