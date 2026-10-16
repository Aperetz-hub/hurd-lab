#!/bin/bash
# Lance le Hurd XFCE avec fenêtre graphique locale.
# Login graphique : demo / demo (root via sudo, mdp demo). SSH : ssh -p 2222 demo@localhost
# Garder -smp 1 : le SMP reste expérimental (voir README_FORK.md).
set -e
cd "$(dirname "$0")"
IMG=$(echo debian-hurd-amd64-*.img | tr ' ' '\n' | head -n1)
[ -f "$IMG" ] || { echo "image introuvable"; exit 1; }
exec qemu-system-x86_64 -m 2048 -M q35 --enable-kvm -smp 1 \
  -drive file="$IMG",cache=writeback,format=raw \
  -net nic,model=e1000 -net user,hostfwd=tcp:127.0.0.1:2222-:22 \
  -vga std "$@"
