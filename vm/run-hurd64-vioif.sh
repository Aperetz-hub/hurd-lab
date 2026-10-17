#!/bin/bash
# vioif (rumpnet) + virtio-blk (storage) test — pure console, on a
# **disposable overlay**.  Recreated at each launch (backing hurd-dev.qcow2):
# the experiments (network switch eth0 -> vioif0, secondary virtio disk)
# never touch the dev disk.
#
# Environment variables:
#   BLK=1        attach a secondary virtio-blk disk (id vd0 -> ld0)
#   BLKIMG=...   disk image (default /tmp/test-virtio.img, created if absent)
#   BLKSIZE=64M  size at creation
#   MON=...      monitor socket (default /tmp/hurd-vioif-mon)
#
# Usage: ./run-hurd64-vioif.sh [base.qcow2]
set -e
cd "$(dirname "$0")"
BASE=${1:-hurd-dev.qcow2}
OVL="$(dirname "$BASE")/hurd-vioif-test.qcow2"
[ -f "$BASE" ] || { echo "base not found: $BASE"; exit 1; }
rm -f "$OVL"
qemu-img create -f qcow2 -F qcow2 -b "$BASE" "$OVL" >/dev/null
echo "disposable overlay: $OVL (backing $BASE)"
MON=${MON:-/tmp/hurd-vioif-mon}
BLK=${BLK:-0}
BLKIMG=${BLKIMG:-/tmp/test-virtio.img}
BLKSIZE=${BLKSIZE:-64M}
rm -f "$MON"
ARGS=(-m 4096 -M q35 --enable-kvm -smp 1
  -drive file="$OVL",cache=writeback,format=qcow2
  -device e1000,netdev=e0 -netdev user,id=e0,hostfwd=tcp:127.0.0.1:2222-:22
  -device virtio-net-pci,netdev=v1 -netdev user,id=v1,hostfwd=tcp:127.0.0.1:2223-:22
  -display none -serial file:/tmp/hurd-vioif-serial.log
  -monitor unix:"$MON",server,nowait)
if [ "$BLK" = "1" ]; then
	[ -f "$BLKIMG" ] || qemu-img create -f raw "$BLKIMG" "$BLKSIZE" >/dev/null
	ARGS+=(-drive file="$BLKIMG",if=none,id=vd0,format=raw
	       -device virtio-blk-pci,drive=vd0)
	echo "secondary virtio-blk disk: $BLKIMG ($BLKSIZE)"
fi
echo "smp=1 blk=$BLK mon=$MON"
exec qemu-system-x86_64 "${ARGS[@]}"
