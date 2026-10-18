#!/usr/bin/env python3
"""Read the guest's VGA text console out of guest memory.

The Hurd guest's console is the VGA text buffer at physical 0xb8000, not the
serial line, so the boot messages the kernel prints -- including the AP startup
ones -- are only visible by reading that buffer.  The QEMU monitor can read guest
physical memory, so the buffer can be decoded from the outside with no guest
agent at all, which is what this does.

The monitor caps how much one `xp` returns, so the buffer is read in chunks.

Usage: vga-console.py <monitor-socket> [--grep PATTERN]
"""

import re
import socket
import sys
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\r")

MON = sys.argv[1]
GREP = None
if "--grep" in sys.argv:
    GREP = sys.argv[sys.argv.index("--grep") + 1]

VGA_BASE = 0xB8000
COLS, ROWS = 80, 25
CELL = 2
CHUNK = 0x100  # bytes per monitor read; the monitor truncates above this


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


def read_chunk(paddr, n):
    out = hmp("xp/%dbx 0x%x" % (n, paddr))
    vals = []
    for line in out.splitlines():
        if ":" not in line:
            continue
        for tok in line.split(":", 1)[1].split():
            try:
                vals.append(int(tok, 16) & 0xFF)
            except ValueError:
                pass
    return bytes(vals)


total = COLS * ROWS * CELL
buf = bytearray()
addr = VGA_BASE
while len(buf) < total:
    want = min(CHUNK, total - len(buf))
    got = read_chunk(addr, want)
    if not got:
        break
    buf += got
    addr += len(got)

rows = []
for r in range(ROWS):
    line = bytes(buf[r * COLS * CELL:(r + 1) * COLS * CELL])
    rows.append(bytes(line[0::2]).decode("latin1").rstrip())

if GREP:
    for i, line in enumerate(rows):
        if GREP.lower() in line.lower():
            print("%2d | %s" % (i, line))
else:
    for i, line in enumerate(rows):
        if line:
            print("%2d | %s" % (i, line))