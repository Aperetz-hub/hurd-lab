#!/usr/bin/env python3
"""Classe l'etat d'une VM QEMU headless par UNE seule lecture de l'ecran VGA.

A utiliser en fin de manche (pas en continu !) : chaque lecture `xp` tient le
BQL de QEMU quelques millisecondes et *desynchronise les vCPU* — un
echantillonnage serre fausse les mesures des courses SMP (constate en 4e
passe : 1 gel / 2 avec echantillonnage continu contre 1 gel / 1 sans).

Verdict :
    BOOT    = le prompt `login:` est present ;
    WEDGE   = la derniere ligne est `ahcisata0: 64-bit DMA` (gel rumpdisk) ;
    UNKNOWN = autre (fsck, panic, ...) : voir les 3 dernieres lignes.

Usage :
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
    """Retourne les lignes non vides de l'ecran VGA courant."""
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
