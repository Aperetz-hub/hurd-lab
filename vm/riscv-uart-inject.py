#!/usr/bin/env python3
"""Feed a byte to a guest that is waiting for one on its serial line.

The RISC-V kernel's PLIC self test needs a byte to arrive in the UART's receive
register, because that is what makes the 16550 assert its interrupt line, and
that is what makes the PLIC raise the supervisor external interrupt.  The guest
cannot produce one itself: it writes to the serial port, it never reads from it,
so there is nothing it can send itself.

So the host sends it, over the QEMU serial chardev.  QEMU is the server of the
unix socket and this script is the client, so QEMU starts without waiting for a
connection.

The byte is not sent on a timer.  The guest prints a trigger line, and the
trigger is what releases the byte, which makes the exchange deterministic: the
test cannot race the injector, and a guest that dies before asking gets a clean
"the trigger never came" rather than a hang.

Each --send pair fires once per *occurrence* of its trigger:

    --send "PLIC-INJECT-1=Z" --send "PLIC-TRIAL=Y"

PLIC-INJECT-1 appears once.  PLIC-TRIAL appears once per candidate context the
kernel's search tries, and the search cannot know in advance how many that will
be, which is why the pair has to be repeatable rather than one-shot.

Everything QEMU writes on the line is appended to --log, so the caller's log is
the guest's console output with the injected bytes marked:

    <-- injected Z

Exit status: 0 when the line closed cleanly, 1 on a usage error, 2 when the socket
never appeared or the guest stopped without any trigger being seen.
"""

import argparse
import os
import socket
import sys
import time

# The stream is kept for matching and trimmed at this size.  It only has to be
# large enough that a trigger printed a moment ago is still in it; the trim
# keeps the most recent part, which is the only part that can match.
STREAM_LIMIT = 1 << 20


def parse_send(specs):
    """Turn the --send arguments into (trigger, byte) pairs."""
    pending = []
    for spec in specs:
        if "=" not in spec:
            raise ValueError(f"bad --send {spec!r}, want TRIGGER=BYTE")
        trigger, byte = spec.split("=", 1)
        if not trigger:
            raise ValueError(f"bad --send {spec!r}, the trigger is empty")
        if len(byte.encode()) != 1:
            raise ValueError(f"bad --send {spec!r}, {byte!r} is not a single byte")
        pending.append((trigger, byte))
    return pending


def connect(path, timeout):
    """Connect once QEMU has created the socket."""
    deadline = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() > deadline:
            return None
        time.sleep(0.05)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    return sock


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--socket", required=True,
                        help="QEMU serial unix socket to connect to")
    parser.add_argument("--log", required=True,
                        help="file to append the guest console output to")
    parser.add_argument("--send", action="append", default=[], metavar="TRIGGER=BYTE",
                        help="send BYTE every time TRIGGER appears in the guest "
                             "output; may be repeated")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="seconds to wait for QEMU to create the socket")
    args = parser.parse_args()

    try:
        pending = parse_send(args.send)
    except ValueError as exc:
        print(f"riscv-uart-inject: {exc}", file=sys.stderr)
        return 1

    if not pending:
        print("riscv-uart-inject: no --send pairs, nothing to do", file=sys.stderr)
        return 1

    sock = connect(args.socket, args.timeout)
    if sock is None:
        print(f"riscv-uart-inject: {args.socket} never appeared", file=sys.stderr)
        return 2

    # `searched` is how far into the stream occurrences have already been counted,
    # and `stream[0:searched]` is what has been seen.  An occurrence is counted
    # when its last byte is past `searched`, and the search starts `len(needle)-1`
    # bytes early so a trigger split across two reads is still found exactly once.
    stream = b""
    searched = 0
    fired_any = False

    with open(args.log, "wb", buffering=0) as log:
        while True:
            try:
                data = sock.recv(4096)
            except (ConnectionResetError, OSError):
                break
            if not data:
                break
            log.write(data)
            stream += data

            for trigger, byte in pending:
                needle = trigger.encode()
                at = stream.find(needle, max(0, searched - len(needle) + 1))
                while at != -1:
                    if at + len(needle) > searched:
                        log.write(f"<-- injected {byte}\n".encode())
                        try:
                            sock.sendall(byte.encode())
                        except OSError:
                            print("riscv-uart-inject: the line went away while "
                                  "injecting", file=sys.stderr)
                            return 2
                        fired_any = True
                    at = stream.find(needle, at + len(needle))

            if len(stream) > STREAM_LIMIT:
                drop = len(stream) - STREAM_LIMIT
                stream = stream[drop:]
                searched -= drop

            searched = len(stream)

    if not fired_any:
        print("riscv-uart-inject: the line closed without any trigger firing",
              file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())