#!/usr/bin/env python3
"""Pin the monitor's memory-read width, and check the readings that matter.

`xp/countx` reads *bytes*: the size suffix in QEMU's HMP `x` command is only
honoured when it is one of b/h/w/l/q, and 'x' is consumed as a formatting flag
while leaving the size at one byte.  `xp/countwx` reads 4-byte words.

The difference is silent.  A parser written for the word form will happily read
the byte form, pick up the low byte of each 32-bit word, and produce numbers that
look like counters rather than the counters.  Both this file's readers pin the
suffix explicitly, and the checks below are what would notice if one of them
regressed.

Usage: memread-check.py <monitor-socket>
"""

import re
import socket
import struct
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\r")
MON = sys.argv[1] if len(sys.argv) > 1 else "/tmp/smp2-mon"

KERNEL_MAP_BASE = 0xFFFFFFFF80000000
LOAD_BASE = 0x1000000


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


def parse(cmd, width):
    """Read with an explicit width and return the items."""
    out = hmp(cmd)
    items = []
    for line in out.splitlines():
        if ":" not in line:
            continue
        for tok in line.split(":", 1)[1].split():
            try:
                items.append(int(tok, 16))
            except ValueError:
                pass
    items = items[:width]
    if len(items) != width:
        raise SystemExit("short read: %r" % out)
    return items


def read_words(paddr, n):
    # "wx": four-byte words, explicitly.  Never "x" alone.
    return parse("xp/%dwx 0x%x" % (n, paddr), n)


def read_bytes(paddr, n):
    return parse("xp/%dbx 0x%x" % (n, paddr), n)


print("== the width suffix really is load-bearing ==")
w = read_words(0x10A8000, 2)
b = read_bytes(0x10A8000, 8)
print("  xp/2wx -> %s" % [hex(v) for v in w])
print("  xp/8bx -> %s" % [hex(v) for v in b])
if struct.pack("<2I", *w) != bytes(b):
    raise SystemExit("the word and byte readings disagree, which should not happen")
print("  the two agree byte for byte, so the suffixes mean what they say")

print("\n== check 1: percpu_array[0].self points at percpu_array[0] ==")
PERCPU_ARRAY = 0xFFFFFFFF810A8000
w = read_words(phys(PERCPU_ARRAY), 2)
self_ptr = w[0] | (w[1] << 32)
print("  percpu_array[0].self = 0x%x" % self_ptr)
if self_ptr != PERCPU_ARRAY:
    raise SystemExit("  WRONG: expected 0x%x. The address translation or the "
                     "read width is broken." % PERCPU_ARRAY)
print("  OK: the translation removes 0xffffffff80000000 and nothing else")

print("\n== check 2: uptime tracks elapsed_ticks ==")
# clock_interrupt() adds exactly 10 ms to uptime per tick, so uptime.seconds
# must equal elapsed_ticks / 100. Two variables in different parts of .bss,
# read independently: a wrong width breaks at least one of them.
ELAPSED = 0xFFFFFFFF810BE2A8 - KERNEL_MAP_BASE
UPTIME = 0xFFFFFFFF810BE2B0 - KERNEL_MAP_BASE
e = read_words(ELAPSED, 1)[0]
sec = read_words(UPTIME, 2)
uptime_sec = sec[0] | (sec[1] << 32)
print("  elapsed_ticks = %d" % e)
print("  uptime.seconds = %d" % uptime_sec)
print("  elapsed_ticks / 100 = %.3f" % (e / 100.0))
if uptime_sec != e // 100:
    raise SystemExit("  WRONG: uptime and elapsed_ticks disagree")
print("  OK: the two agree, so neither read is byte-swapped or truncated")

print("\n== check 3: the per-CPU slots identify themselves ==")
# percpu_array[i].cpu_id is set to i by init_percpu(), and percpu_array[i].self
# points at its own slot, so the index, the stride and the read width are all
# confirmed at once.  Note that machine_slot[i].is_cpu is *not* an index: the
# kernel only ever assigns TRUE to it (i386/i386at/model_dep.c:878 and
# i386/i386/smp.c:87) and uses it as a "this slot is live" flag, so a check that
# expected it to equal the processor number would be wrong.
PERCPU_ARRAY = 0xFFFFFFFF810A8000
PERCPU_STRIDE = 1208
for cpu in (0, 1):
    base = phys(PERCPU_ARRAY) + cpu * PERCPU_STRIDE
    w = read_words(base, 4)
    self_ptr = w[0] | (w[1] << 32)
    apic_id, cpu_id = w[2], w[3]
    print("  percpu_array[%d]: self=0x%x apic_id=%d cpu_id=%d"
          % (cpu, self_ptr, apic_id, cpu_id))
    if self_ptr != PERCPU_ARRAY + cpu * PERCPU_STRIDE or cpu_id != cpu:
        raise SystemExit("  WRONG: percpu_array[%d] does not identify itself" % cpu)
print("  OK: both per-CPU slots identify themselves")

print("\nall checks passed")