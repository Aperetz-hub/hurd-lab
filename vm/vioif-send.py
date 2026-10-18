#!/usr/bin/env python3
"""Guest side of the vioif0 throughput test: push bytes at the host.

Runs inside the Hurd guest and sends a fixed number of megabytes to a listener on
the host, over /dev/vioif0.  The socket is bound to the vioif0 address so the
traffic cannot take the loopback or the (unconfigured) e1000 by accident: the
point of the measurement is the virtio device and the rumpnet translator behind
it.

Raw TCP rather than SSH.  An scp of the same volume would put the same bytes
through the same device with a cipher and a MAC on top, and the result would be a
property of the cipher as much as of the device.  Both figures are worth having,
so the orchestrator runs this one and an scp of the same size, and reports them
separately.

Writes a small JSON summary where the host can collect it, and prints the same
thing on stdout.

Usage (in the guest): vioif-send.py <mib> <host> <port> <outfile> [bind-addr]
"""

import json
import socket
import sys
import time

MIB = int(sys.argv[1]) if len(sys.argv) > 1 else 100
HOST = sys.argv[2] if len(sys.argv) > 2 else "10.0.2.2"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 9999
OUT = sys.argv[4] if len(sys.argv) > 4 else "/tmp/vioif-send.json"
BIND = sys.argv[5] if len(sys.argv) > 5 else "10.0.2.15"

BLOCK = 1 << 20
TOTAL = MIB << 20

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# TCP_NODELAY matters here: the payload is one long run of bytes and Nagle would
# turn it into a series of small delayed writes, which measures the delayed-ACK
# interaction rather than the device.
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.bind((BIND, 0))
s.connect((HOST, PORT))

payload = bytes(BLOCK)
sent = 0
# One second per mark, so a stall shows up as a gap in the trace rather than only
# in the average.
marks = []

t0 = time.time()
next_mark = t0 + 1.0
while sent < TOTAL:
    n = s.send(payload if sent + BLOCK <= TOTAL else payload[: TOTAL - sent])
    if n == 0:
        break
    sent += n
    now = time.time()
    if now >= next_mark:
        marks.append({"t": round(now - t0, 3), "mib": round(sent / (1 << 20), 3)})
        next_mark += 1.0

dt = time.time() - t0
s.close()

result = {
    "bytes_sent": sent,
    "bytes_expected": TOTAL,
    "complete": sent == TOTAL,
    "seconds": round(dt, 4),
    "mib_per_s": round(sent / (1 << 20) / dt, 4) if dt > 0 else 0.0,
    "host": HOST,
    "port": PORT,
    "bind": BIND,
    "marks": marks,
}

with open(OUT, "w") as fh:
    json.dump(result, fh, indent=2)

print("sent %d bytes (%d MiB) in %.3f s = %.3f MiB/s complete=%s"
      % (sent, MIB, dt, result["mib_per_s"], result["complete"]))
sys.exit(0 if result["complete"] else 1)