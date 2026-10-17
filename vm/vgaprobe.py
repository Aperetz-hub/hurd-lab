#!/usr/bin/env python3
"""Classify the state of a headless QEMU VM with a SINGLE read of the VGA screen.

To be used at the end of a round (not continuously!): each `xp` read holds
QEMU's BQL for a few milliseconds and *desynchronizes the vCPUs* — tight
sampling skews SMP race measurements (observed in the 4th pass: 1 hang / 2
with continuous sampling vs 1 hang / 1 without).

Verdict:
    BOOT    = the `login:` prompt is present ;
    WEDGE   = the last line is `ahcisata0: 64-bit DMA` (rumpdisk hang) ;
    UNKNOWN = other (fsck, panic, ...): see the last 3 lines.

Usage:
    MON=/tmp/hurd-smp-mon python3 vm/vgaprobe.py
"""

import os
import re
import socket
import sys

MON = os.environ.get("MON", "/tmp/hurd-smp-mon")
COLS = 80


def _recv_until_prompt(sock):
    buf = b""
    while b"(qemu) " not in buf:
        data = sock.recv(1 << 16)
        if not data:
            break
        buf += data
    return buf


def snapshot():
    """Return the non-empty lines of the current VGA screen."""
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(15)
    sock.connect(MON)
    try:
        _recv_until_prompt(sock)
        sock.sendall(b"xp/4000cb 0xb8000\n")
        buf = _recv_until_prompt(sock)
    finally:
        sock.close()

    raw = bytearray()
    for tok in re.findall(rb"'((?:\\x[0-9a-fA-F]{2}|[^'])*)'", buf):
        if tok.startswith(b"\\x") and len(tok) == 4:
            raw.append(int(tok[2:], 16))
        elif len(tok) == 1:
            raw.append(tok[0])
        else:
            raw.append(0x20)

    chars = "".join(chr(b) if 32 <= b < 127 else " " for b in raw[0::2])
    return [l.strip() for l in (chars[i:i + COLS] for i in range(0, len(chars), COLS))
            if l.strip()]


def main():
    try:
        lines = snapshot()
    except Exception as exc:
        print("MONERR %s" % exc)
        return 0
    if any("login:" in l for l in lines):
        verdict = "BOOT"
    elif lines and "ahcisata0: 64-bit DMA" in lines[-1]:
        verdict = "WEDGE"
    else:
        verdict = "UNKNOWN"
    print(verdict)
    print("   last: " + " || ".join(lines[-3:])[:200])
    return 0


if __name__ == "__main__":
    sys.exit(main())
