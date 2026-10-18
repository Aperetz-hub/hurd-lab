#!/usr/bin/env python3
"""Kernel clock probe for the Hurd SMP investigation.

Reads the kernel's own timekeeping globals straight out of guest physical memory
through the QEMU monitor, and stamps every sample with the host wall clock so the
time base is completely independent of the guest.

It replaces the address handling of the earlier kprobe.py.  That one fed the
symbol addresses straight from `nm` to the monitor's physical-memory read, but
the kernel is linked at KERNEL_MAP_BASE (0xffffffff80000000) and *loaded* at
1 MiB, so every address it read was in the wrong place.  QEMU answers such a read
with "Cannot access memory", which kprobe silently turned into a missing sample.
The translation is applied here, and checked against a self-referential symbol
before any number is reported.

Globals read (offsets are from the ELF):

  elapsed_ticks   ticks since boot, incremented only on the master CPU
  time            time of day, advanced by the master clock
  uptime          time since boot, advanced by the master clock
  machine_slot[]  per-CPU clock interrupt counters, advanced by every CPU that
                  takes a clock interrupt -- which is how the application
                  processor's own timer is measured

Usage: clock-probe.py <monitor-socket> <window-seconds> [--json out.json]
"""

import json
import re
import socket
import struct
import subprocess
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\r")

MON = sys.argv[1]
WINDOW = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
JSON_OUT = None
if "--json" in sys.argv:
    JSON_OUT = sys.argv[sys.argv.index("--json") + 1]

ELF = "/tmp/gnumach-fork-4207ebda"
KERNEL_MAP_BASE = 0xFFFFFFFF80000000

# The kernel is linked here and loaded at 1 MiB; see the module docstring.
LOAD_OFFSET = KERNEL_MAP_BASE - 0x1000000

# struct percpu / struct machine_slot, from the ELF's debug information.
PERCPU_ARRAY = 0xFFFFFFFF810A8000
PERCPU_STRIDE = 1208
MACHINE_SLOT = 0xFFFFFFFF810BE340
MACHINE_SLOT_STRIDE = 32
CPU_TICKS_OFF = 16  # integer_t cpu_ticks[3] = { user, system, idle }
STATES = ("user", "system", "idle")


def phys(vaddr):
    return vaddr - KERNEL_MAP_BASE


def hmp(cmd):
    """One HMP command, with the terminal echo and colour stripped."""
    for _ in range(10):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(20)
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
    """Read n 32-bit words at a physical address.

    The width suffix is explicit and load-bearing.  QEMU's HMP `x` command only
    honours a size of b/h/w/l/q: with `xp/4x` the 'x' is taken as a formatting
    flag and the size stays one *byte*.  A reader written for the word form
    silently reads the byte form, picks up the low byte of every 32-bit word, and
    produces numbers that look like counters.  `xp/4wx` says what it means; see
    vm/memread-check.py.
    """
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
        raise SystemExit("short read at 0x%x: wanted %d words, got %d (%r)"
                         % (paddr, n, len(vals), out))
    return vals[:n]


def s64(v):
    return v - 2 ** 64 if v >= 2 ** 63 else v


def s32(v):
    return v - 2 ** 32 if v >= 2 ** 31 else v


def q64(paddr):
    lo, hi = words(paddr, 2)
    return lo | (hi << 32)


def time_value(paddr):
    """A struct time_value64: { int64 seconds; int64 nanoseconds }."""
    sec, nsec = q64(paddr), q64(paddr + 8)
    return s64(sec) + s64(nsec) / 1e9


def symbols():
    syms = {}
    for line in subprocess.run(["nm", "-n", ELF], capture_output=True,
                              text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms.setdefault(parts[2], int(parts[0], 16))
    return syms


S = symbols()
for name in ("elapsed_ticks", "time", "uptime", "master_cpu"):
    if name not in S:
        raise SystemExit("symbol %s not found in %s" % (name, ELF))


def read_elapsed():
    return words(phys(S["elapsed_ticks"]), 1)[0]


def read_master():
    return s32(words(phys(S["master_cpu"]), 1)[0])


def read_cpu_ticks(cpu):
    base = phys(MACHINE_SLOT) + cpu * MACHINE_SLOT_STRIDE + CPU_TICKS_OFF
    return [s32(v) for v in words(base, len(STATES))]


def read_percpu(cpu):
    """Enough of struct percpu to tell the CPUs apart."""
    base = phys(PERCPU_ARRAY) + cpu * PERCPU_STRIDE
    w = words(base, 0x4B0 // 4)
    return {
        "self": w[0] | (w[1] << 32),
        "apic_id": s32(w[2]),
        "cpu_id": s32(w[3]),
        "state": s32(w[0x440 // 4]),
        "quantum": s32(w[0x458 // 4]),
        "last_quantum": s32(w[0x460 // 4]),
        "active_thread": w[0x4A8 // 4] | (w[0x4AC // 4] << 32),
    }


def snapshot():
    return {
        "host": time.time(),
        "elapsed_ticks": read_elapsed(),
        "time": time_value(phys(S["time"])),
        "uptime": time_value(phys(S["uptime"])),
        "master_cpu": read_master(),
        "cpu_ticks": {c: read_cpu_ticks(c) for c in (0, 1)},
        "percpu": {c: read_percpu(c) for c in (0, 1)},
    }


# ---------------------------------------------------------------- self-check
# percpu_array[0].self points at percpu_array[0].  If the address translation
# were wrong this would not hold, and every number below would be read from the
# wrong place without anything looking wrong.
check = read_percpu(0)
if check["self"] != PERCPU_ARRAY:
    raise SystemExit("address translation is wrong: percpu_array[0].self = 0x%x, "
                     "expected 0x%x" % (check["self"], PERCPU_ARRAY))

a = snapshot()
print("master_cpu                : %d" % a["master_cpu"])
print("elapsed_ticks at t0       : %d" % a["elapsed_ticks"])
print("uptime at t0              : %.6f s" % a["uptime"])
print("time at t0                : %.6f" % a["time"])
for c in (0, 1):
    print("cpu%d percpu               : apic_id=%d cpu_id=%d state=%d "
          "active_thread=0x%x cpu_ticks=%s"
          % (c, a["percpu"][c]["apic_id"], a["percpu"][c]["cpu_id"],
             a["percpu"][c]["state"], a["percpu"][c]["active_thread"],
             dict(zip(STATES, a["cpu_ticks"][c]))))

print("\nsampling for %.0f s ..." % WINDOW)
time.sleep(WINDOW)
b = snapshot()

host_dt = b["host"] - a["host"]
tick_dt = b["elapsed_ticks"] - a["elapsed_ticks"]
uptime_dt = b["uptime"] - a["uptime"]

print("host window               : %.3f s" % host_dt)
print("elapsed_ticks             : +%d -> %.4f Hz" % (tick_dt, tick_dt / host_dt))
print("uptime                    : +%.6f s -> %.6f Hz" % (uptime_dt, uptime_dt / host_dt))
print("uptime / host clock       : %.6f" % (uptime_dt / host_dt))
print()
print("%-6s %-8s %12s %12s %10s" % ("cpu", "state", "t0", "t1", "Hz"))
for c in (0, 1):
    for i, st in enumerate(STATES):
        d = b["cpu_ticks"][c][i] - a["cpu_ticks"][c][i]
        print("%-6s %-8s %12d %12d %10.3f"
              % ("cpu%d" % c, st, a["cpu_ticks"][c][i], b["cpu_ticks"][c][i], d / host_dt))
    tot = sum(b["cpu_ticks"][c]) - sum(a["cpu_ticks"][c])
    print("%-6s %-8s %12d %12d %10.3f"
          % ("cpu%d" % c, "TOTAL", sum(a["cpu_ticks"][c]), sum(b["cpu_ticks"][c]),
             tot / host_dt))

result = {
    "host_window_s": host_dt,
    "master_cpu": a["master_cpu"],
    "elapsed_ticks_delta": tick_dt,
    "elapsed_ticks_hz": tick_dt / host_dt,
    "uptime_delta_s": uptime_dt,
    "uptime_hz": uptime_dt / host_dt,
    "uptime_over_host": uptime_dt / host_dt,
    "cpu_ticks_hz": {
        "cpu%d" % c: (sum(b["cpu_ticks"][c]) - sum(a["cpu_ticks"][c])) / host_dt
        for c in (0, 1)
    },
}
if JSON_OUT:
    with open(JSON_OUT, "w") as fh:
        json.dump(result, fh, indent=2)
    print("\nwritten to %s" % JSON_OUT)