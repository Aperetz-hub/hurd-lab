#!/usr/bin/env python3
"""Does the application processor take its own LAPIC timer interrupts?

The question was left open by the SMP stress pass: on the fork kernel at
`-smp 2`, CPU 0 takes the master clock through the IO-APIC, and it was never
established whether CPU 1 gets a periodic interrupt of its own.

The measurement that settles it is `machine_slot[cpu].cpu_ticks[]`.  It is
incremented inside `clock_interrupt()` (kern/mach_clock.c:239), before the
`if (my_cpu == master_cpu)` guard that protects `elapsed_ticks`, so it counts
clock interrupts on *every* processor that takes one -- including an application
processor that has nothing else observable going on.  Nothing else in the kernel
can move it: it is not written anywhere else, and a CPU that took no clock
interrupt would leave it unchanged however long it sat there.

Two supporting readings, neither of which is the proof:

  * `info lapic <apic-id>` shows each vCPU's local APIC.  Under KVM that is
    QEMU's shadow of the in-kernel LAPIC, so it is corroboration, not evidence.
  * the kernel's own `calibrated_ticks`, which `lapic_enable_timer()` loads into
    `init_count`.  Matching it against a CPU's `initial_count` shows *which* CPU
    the LAPIC timer programming path ran on, because that function is only
    called for a non-master CPU.

What this deliberately does not do: sample RIP and count how often it lands in
the interrupt path.  It was tried, and it is far too weak.  The interrupt handler
occupies a few tens of microseconds out of every period, and one monitor round
trip costs about two milliseconds, so the sampling rate is two orders of
magnitude coarser than the event.  At 150 samples per CPU it caught the
master CPU at zero as well, which is a CPU known to tick at 200 Hz -- so the
method reports nothing about either CPU and says nothing about either.

Usage: ap-timer-probe.py <monitor-socket> [window-seconds] [kernel-elf]
"""

import json
import re
import socket
import subprocess
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\r")

MON = sys.argv[1] if len(sys.argv) > 1 else "/tmp/smp2-mon"
WINDOW = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
ELF = sys.argv[3] if len(sys.argv) > 3 else "/tmp/gnumach-fork-4207ebda"

KERNEL_MAP_BASE = 0xFFFFFFFF80000000
STATES = ("user", "system", "idle")

# From the ELF's debug information.
MACHINE_SLOT = 0xFFFFFFFF810BE340
MACHINE_SLOT_STRIDE = 32
CPU_TICKS_OFF = 16  # integer_t cpu_ticks[3] = { user, system, idle }


def phys(vaddr):
    return vaddr - KERNEL_MAP_BASE


def hmp(cmd):
    for _ in range(10):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(30)
            s.connect(MON)
            buf = b""
            while b"(qemu) " not in buf:
                c = s.recv(1 << 16)
                if not c:
                    break
                buf += c
            s.sendall(cmd.encode() + b"\n")
            buf = b""
            while b"(qemu) " not in buf:
                c = s.recv(1 << 16)
                if not c:
                    break
                buf += c
            s.close()
            return ANSI.sub("", buf.decode(errors="replace"))
        except Exception:
            time.sleep(0.4)
    raise SystemExit("monitor read failed")


def words(paddr, n):
    # The width suffix is explicit and load-bearing: QEMU's HMP `x` only honours
    # b/h/w/l/q, so "xp/4x" reads four *bytes*.  See vm/memread-check.py.
    out = hmp("xp/%dwx 0x%x" % (n, paddr))
    vals = []
    for line in out.splitlines():
        if ":" not in line:
            continue
        for tok in line.split(":", 1)[1].split():
            try:
                vals.append(int(tok, 16) & 0xFFFFFFFF)
            except ValueError:
                pass
    if len(vals) < n:
        raise SystemExit("short read at 0x%x: wanted %d, got %d (%r)"
                         % (paddr, n, len(vals), out))
    return vals[:n]


def s32(v):
    return v - 2 ** 32 if v >= 2 ** 31 else v


def sym(name):
    for line in subprocess.run(["nm", ELF], capture_output=True,
                              text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit("symbol %s not found in %s" % (name, ELF))


def cpu_ticks(cpu):
    base = phys(MACHINE_SLOT) + cpu * MACHINE_SLOT_STRIDE + CPU_TICKS_OFF
    return [s32(v) for v in words(base, len(STATES))]


# --- self-checks, so a wrong address or width cannot masquerade as a result ---
# machine_slot[0].is_cpu is TRUE for every live slot (i386/i386at/model_dep.c:878),
# and uptime.seconds tracks elapsed_ticks exactly at 100 ticks per second.
for cpu in (0, 1):
    if s32(words(phys(MACHINE_SLOT) + cpu * MACHINE_SLOT_STRIDE, 1)[0]) != 1:
        raise SystemExit("machine_slot[%d].is_cpu is not TRUE: wrong address?" % cpu)
ELAPSED = phys(sym("elapsed_ticks"))
UPTIME = phys(sym("uptime"))
t0e = words(ELAPSED, 1)[0]
t0u = words(UPTIME, 2)
t0u = (t0u[0] | (t0u[1] << 32)) & 0xFFFFFFFFFFFFFFFF
if t0u != t0e // 100:
    raise SystemExit("uptime (%d) does not match elapsed_ticks/100 (%d): wrong "
                     "address or width" % (t0u, t0e // 100))

print("=== the two CPUs, as the kernel describes them ===")
PERCPU = phys(sym("percpu_array"))
PERCPU_STRIDE = 1208
for cpu in (0, 1):
    w = words(PERCPU + cpu * PERCPU_STRIDE, 4)
    print("  cpu%d: percpu self=0x%x apic_id=%d cpu_id=%d"
          % (cpu, w[0] | (w[1] << 32), s32(w[2]), s32(w[3])))

print("\n=== the LAPIC timer programming ===")
print("  calibrated_ticks = %d" % words(phys(sym("calibrated_ticks")), 1)[0])
for cpu in (0, 1):
    for line in hmp("info lapic %d" % cpu).splitlines():
        s = line.strip()
        if re.match(r"LVTT\b", s) or s.startswith("Timer") or "dumping" in s:
            print("  cpu%d: %s" % (cpu, s))

print("\n=== clock interrupts per processor, over %.0f s ===" % WINDOW)
a = [cpu_ticks(c) for c in (0, 1)]
t0 = time.time()
time.sleep(WINDOW)
b = [cpu_ticks(c) for c in (0, 1)]
dt = time.time() - t0

print("  window %.3f s" % dt)
print("  %-6s %-8s %12s %12s %10s" % ("cpu", "state", "before", "after", "Hz"))
result = {}
for cpu in (0, 1):
    for i, state in enumerate(STATES):
        d = b[cpu][i] - a[cpu][i]
        print("  %-6s %-8s %12d %12d %10.3f"
              % ("cpu%d" % cpu, state, a[cpu][i], b[cpu][i], d / dt))
    total = sum(b[cpu]) - sum(a[cpu])
    result["cpu%d" % cpu] = total / dt
    print("  %-6s %-8s %12d %12d %10.3f"
          % ("cpu%d" % cpu, "TOTAL", sum(a[cpu]), sum(b[cpu]), total / dt))

print("\n=== master clock, for comparison ===")
e0 = words(ELAPSED, 1)[0]
t0 = time.time()
time.sleep(10)
e1 = words(ELAPSED, 1)[0]
dt2 = time.time() - t0
print("  elapsed_ticks advanced %d in %.3f s -> %.3f Hz (master CPU only)"
      % (e1 - e0, dt2, (e1 - e0) / dt2))
print("  the kernel is built for hz = %d" % words(phys(sym("hz")), 1)[0])

print("\nclock interrupt rate per CPU: %s"
      % {k: round(v, 3) for k, v in result.items()})
print("(a rate well above zero on a processor is the answer: that counter is")
print(" only incremented from clock_interrupt, so it can only move if the")
print(" processor took a clock interrupt)")