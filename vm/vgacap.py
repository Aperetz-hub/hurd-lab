#!/usr/bin/env python3
"""Continuous capture of a headless QEMU VM's VGA text screen.

Reads the VGA text buffer (0xb8000, 80x25, byte/attribute pairs) via the
HMP monitor (`xp/4000cb 0xb8000`) **without freezing the VM** and writes to
$OUT only the screens that change.

Why: in pure console mode, `-serial file:` stays empty (the gnumach kernel
and the Hurd write to the VGA). This script therefore provides the full
boot log, including kernel messages that scroll by before the
console-client takes over.

Usage:
    SOCK=/tmp/hurd-smp-mon OUT=/tmp/bootlog.txt DUR=300 python3 vm/vgacap.py

Variables:
    SOCK  QEMU monitor socket (default /tmp/hurd-smp-mon)
    OUT   output file (default /tmp/vgacap.log)
    DUR   capture duration in seconds (default 300)
"""

import os
import re
import socket
import sys
import time

SOCK = os.environ.get("SOCK", "/tmp/hurd-smp-mon")
OUT = os.environ.get("OUT", "/tmp/vgacap.log")
DUR = float(os.environ.get("DUR", "300"))
COLS = 80


def _recv_until_prompt(sock):
    """Read until the next `(qemu) ` prompt (or EOF)."""
    buf = b""
    while b"(qemu) " not in buf:
        data = sock.recv(1 << 16)
        if not data:
            break
        buf += data
    return buf


def snapshot():
    """Return the current VGA screen as text."""
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(10)
    sock.connect(SOCK)
    try:
        _recv_until_prompt(sock)        # drain banner + previous prompt
        sock.sendall(b"xp/4000cb 0xb8000\n")
        buf = _recv_until_prompt(sock)
    finally:
        sock.close()

    # HMP returns bytes as 'x' or '\xNN': rebuild the raw content.
    raw = bytearray()
    for tok in re.findall(rb"'((?:\\x[0-9a-fA-F]{2}|[^'])*)'", buf):
        if tok.startswith(b"\\x") and len(tok) == 4:
            raw.append(int(tok[2:], 16))
        elif len(tok) == 1:
            raw.append(tok[0])
        else:
            raw.append(0x20)            # unexpected token: space

    # character = even byte, attribute = odd byte
    chars = "".join(chr(b) if 32 <= b < 127 else " " for b in raw[0::2])
    rows = (chars[i:i + COLS].rstrip() for i in range(0, len(chars), COLS))
    return "\n".join(rows).rstrip("\n")


def main():
    t0 = time.time()
    last = None
    with open(OUT, "w", encoding="utf-8") as out:
        out.write("### vgacap start %s sock=%s\n"
                  % (time.strftime("%H:%M:%S"), SOCK))
        while time.time() - t0 < DUR:
            try:
                cur = snapshot()
                if cur != last:
                    out.write("\n===== t=%.1fs =====\n%s\n"
                              % (time.time() - t0, cur))
                    out.flush()
                    last = cur
            except Exception as exc:    # VM stopped, socket closed, ...
                out.write("### %s\n" % exc)
                out.flush()
            # tight sampling during boot, more relaxed afterwards
            time.sleep(0.4 if time.time() - t0 < 90 else 1.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
