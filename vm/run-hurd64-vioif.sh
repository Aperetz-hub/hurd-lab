#!/bin/bash
# Test vioif (rumpnet) — console pure, sur un **overlay jetable**.
# Recréé à chaque lancement (backing hurd-dev.qcow2) : les essais de bascule
# réseau eth0 -> vioif0 (susceptibles de faire tomber/crasher pfinet) ne
# touchent jamais le disque de dev.
#
# Usage: ./run-hurd64-vioif.sh [base.qcow2]
set -e
cd "$(dirname "$0")"
BASE=${1:-hurd-dev.qcow2}
OVL="$(dirname "$BASE")/hurd-vioif-test.qcow2"
[ -f "$BASE" ] || { echo "base introuvable: $BASE"; exit 1; }
rm -f "$OVL"
qemu-img create -f qcow2 -F qcow2 -b "$BASE" "$OVL" >/dev/null
echo "overlay jetable: $OVL (backing $BASE)"
MON=${MON:-/tmp/hurd-vioif-mon}
rm -f "$MON"
exec qemu-system-x86_64 -m 4096 -M q35 --enable-kvm -smp 1 \
  -drive file="$OVL",cache=writeback,format=qcow2 \
  -device e1000,netdev=e0 -netdev user,id=e0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=v1 -netdev user,id=v1,hostfwd=tcp:127.0.0.1:2223-:22 \
  -display none -serial file:/tmp/hurd-vioif-serial.log \
  -monitor unix:"$MON",server,nowait
