#!/bin/bash
# Launch the dev Debian/Hurd amd64 64-bit VM in **pure console / headless**
# mode (no X11: no Xorg, no xdm, no XFCE).
#
#   * AUTONOMOUS working disk (default: hurd-dev.qcow2, no backing file) —
#     never boot the raw base image `debian-hurd-amd64-*.img`;
#   * `-smp 1` (SMP remains experimental, cf. run-hurd64-smp.sh);
#   * `-m 4G`: required for the rump vioif driver to attach
#     (with 2 GB: "virtqueue 0 for rx0 allocation failed").
#
# Network: e1000/netdde -> ssh localhost:2222 ; virtio-net (vioif) -> 2223.
# Console: VGA text, capturable via the QEMU monitor (`screendump`),
#          unix socket /tmp/hurd-dev-mon (see docs/suivi.md).
#
# Usage: ./run-hurd64.sh [img.qcow2]
set -e
cd "$(dirname "$0")"
IMG=${1:-hurd-dev.qcow2}
case "$IMG" in
  *debian-hurd*.img)
    echo "REFUSED: do not write to the base image; use a qcow2 copy" >&2
    exit 1;;
esac
[ -f "$IMG" ] || { echo "image not found: $IMG (see README, 'qemu-img convert')"; exit 1; }
MON=${MON:-/tmp/hurd-dev-mon}
rm -f "$MON"
exec qemu-system-x86_64 -m 4096 -M q35 --enable-kvm -smp 1 \
  -drive file="$IMG",cache=writeback,format=qcow2 \
  -device e1000,netdev=e0 -netdev user,id=e0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=v1 -netdev user,id=v1,hostfwd=tcp:127.0.0.1:2223-:22 \
  -display none -serial file:/tmp/hurd-serial.log \
  -monitor unix:"$MON",server,nowait
