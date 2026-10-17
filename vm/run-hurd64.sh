#!/bin/bash
# Lance la VM dev Debian/Hurd amd64 64-bit en mode **console pure / headless**
# (aucun X11 : ni Xorg, ni xdm, ni XFCE).
#
#   * disque de travail AUTONOME (défaut: hurd-dev.qcow2, sans backing file) —
#     ne jamais démarrer l'image de base brute `debian-hurd-amd64-*.img` ;
#   * `-smp 1` (le SMP reste expérimental, cf. run-hurd64-smp.sh) ;
#   * `-m 4G` : nécessaire pour que le pilote rump vioif s'attache
#     (en 2 Go : « virtqueue 0 for rx0 allocation failed »).
#
# Réseau : e1000/netdde -> ssh localhost:2222 ; virtio-net (vioif) -> 2223.
# Console : VGA texte, capturable via le moniteur QEMU (`screendump`),
#           socket unix /tmp/hurd-dev-mon (voir docs/suivi.md).
#
# Usage: ./run-hurd64.sh [img.qcow2]
set -e
cd "$(dirname "$0")"
IMG=${1:-hurd-dev.qcow2}
case "$IMG" in
  *debian-hurd*.img)
    echo "REFUS: ne pas écrire dans l'image de base ; utiliser une copie qcow2" >&2
    exit 1;;
esac
[ -f "$IMG" ] || { echo "image introuvable: $IMG (voir README, 'qemu-img convert')"; exit 1; }
MON=${MON:-/tmp/hurd-dev-mon}
rm -f "$MON"
exec qemu-system-x86_64 -m 4096 -M q35 --enable-kvm -smp 1 \
  -drive file="$IMG",cache=writeback,format=qcow2 \
  -device e1000,netdev=e0 -netdev user,id=e0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=v1 -netdev user,id=v1,hostfwd=tcp:127.0.0.1:2223-:22 \
  -display none -serial file:/tmp/hurd-serial.log \
  -monitor unix:"$MON",server,nowait
