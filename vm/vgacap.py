#!/usr/bin/env python3
"""Capture continue de l'ecran texte VGA d'une VM QEMU headless.

Lit le buffer texte VGA (0xb8000, 80x25, paires octet/attribut) via le
moniteur HMP (`xp/4000cb 0xb8000`) **sans figer la VM** et n'ecrit dans $OUT
que les ecrans qui changent.

Pourquoi : en console pure, `-serial file:` reste vide (le noyau gnumach et
le Hurd ecrivent sur la VGA). Ce script donne donc le log de boot complet,
y compris les messages noyau qui defilent avant que le console-client ne
prenne la main.

Usage :
    SOCK=/tmp/hurd-smp-mon OUT=/tmp/bootlog.txt DUR=300 python3 vm/vgacap.py

Variables :
    SOCK  socket du moniteur QEMU (defaut /tmp/hurd-smp-mon)
    OUT   fichier de sortie (defaut /tmp/vgacap.log)
    DUR   duree de capture en secondes (defaut 300)
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
    """Lit jusqu'au prochain prompt `(qemu) ` (ou EOF)."""
    buf = b""
    while b"(qemu) " not in buf:
        data = sock.recv(1 << 16)
        if not data:
            break
        buf += data
    return buf


def snapshot():
    """Retourne l'ecran VGA courant sous forme de texte."""
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(10)
    sock.connect(SOCK)
    try:
        _recv_until_prompt(sock)        # vider banniere + prompt precedent
        sock.sendall(b"xp/4000cb 0xb8000\n")
        buf = _recv_until_prompt(sock)
    finally:
        sock.close()

    # HMP rend les octets sous forme 'x' ou '\xNN' : on reconstitue le brut.
    raw = bytearray()
    for tok in re.findall(rb"'((?:\\x[0-9a-fA-F]{2}|[^'])*)'", buf):
        if tok.startswith(b"\\x") and len(tok) == 4:
            raw.append(int(tok[2:], 16))
        elif len(tok) == 1:
            raw.append(tok[0])
        else:
            raw.append(0x20)            # jeton inattendu : espace

    # caractere = octet pair, attribut = octet impair
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
            except Exception as exc:    # VM arretee, socket fermee, ...
                out.write("### %s\n" % exc)
                out.flush()
            # echantillonnage serre pendant le boot, plus relache ensuite
            time.sleep(0.4 if time.time() - t0 < 90 else 1.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
