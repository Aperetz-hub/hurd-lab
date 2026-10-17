#!/usr/bin/env python3
"""Kernel thermometer for the Hurd SMP investigation (Pass 5).

Reads a set of kernel globals straight out of guest memory through the QEMU
monitor (HMP `x` command).  Never attaches gdb: a gdb session pauses the guest
and an aborted one leaves QEMU paused with no client.

Usage: python3 kprobe.py <monitor-socket> [interval_seconds]
"""
import socket
import subprocess
import sys
import time

import os
ELF = os.environ.get("KPROBE_ELF", "/tmp/gnumach-new")
MON = sys.argv[1]
INTERVAL = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0

SYMS = ["curr_ipl", "spl_init", "elapsed_ticks", "uptime", "last_hpc_read",
        "has_irq_specific_eoi", "timer_pin", "calibrated_ticks", "master_cpu",
        "mtime", "lapic_timer_val"]

# Addresses of the globals are taken from the kernel ELF, and re-resolved
# every run so a freshly built kernel is picked up automatically.
ADDRS = {}
out = subprocess.run(["nm", "-n", ELF], capture_output=True, text=True).stdout
for line in out.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[2] in SYMS:
        ADDRS[parts[2]] = int(parts[0], 16)


def hmp(cmd, tries=10):
    """Send one HMP command, return the output lines (prompt stripped)."""
    for _ in range(tries):
        sock = None
        try:
            sock = socket.socket(socket.AF_UNIX)
            sock.settimeout(20)
            sock.connect(MON)
            buf = b""
            while b"(qemu) " not in buf:
                chunk = sock.recv(1 << 16)
                if not chunk:
                    break
                buf += chunk
            sock.sendall(cmd.encode() + b"\n")
            buf = b""
            while b"(qemu) " not in buf:
                chunk = sock.recv(1 << 16)
                if not chunk:
                    break
                buf += chunk
            text = buf.decode(errors="replace")
            return [l for l in text.splitlines() if not l.startswith("(qemu)")]
        except Exception:
            time.sleep(0.5)
        finally:
            if sock:
                sock.close()
    return ["<monitor read failed>"]


def read(addr, size="w", count=1):
    """Read `count` words of `size` at `addr`; returns a list of ints."""
    vals = []
    for line in hmp("x/%d%sx 0x%x" % (count, size, addr)):
        if ":" in line:
            for tok in line.split(":", 1)[1].split():
                if tok.startswith("0x"):
                    try:
                        vals.append(int(tok, 16))
                    except ValueError:
                        pass
    return vals


def snapshot(tag):
    got = {}
    for name in SYMS:
        addr = ADDRS.get(name)
        if addr is None:
            continue
        if name == "uptime" or name == "mtime":
            v = read(addr, "g", 2)
            got[name] = "%d.%09d" % (v[0], v[1]) if len(v) == 2 else "?"
        elif name == "curr_ipl":
            v = read(addr, "w", 2)
            got[name] = "[cpu0=%s cpu1=%s]" % (v[0], v[1]) if len(v) == 2 else "?"
        else:
            v = read(addr, "w", 1)
            got[name] = v[0] if v else "?"
    print("--- %s ---" % tag)
    for name in SYMS:
        if name in got:
            print("  %-24s %s" % (name, got[name]))
    return got


if __name__ == "__main__":
    print("symbols:", {k: hex(v) for k, v in sorted(ADDRS.items())})
    a = snapshot("T+0")
    if INTERVAL > 0:
        time.sleep(INTERVAL)
        b = snapshot("T+%.0fs" % INTERVAL)
        print("--- deltas over %.0fs ---" % INTERVAL)
        for key in ("elapsed_ticks", "uptime", "last_hpc_read"):
            if key in a and key in b:
                try:
                    if key == "uptime":
                        fa = a[key].split(".")
                        fb = b[key].split(".")
                        d = (int(fb[0]) - int(fa[0])) + (int(fb[1]) - int(fa[1])) / 1e9
                    else:
                        d = b[key] - a[key]
                    print("  %-24s %s" % (key, d))
                except Exception as exc:
                    print("  %-24s <%s>" % (key, exc))