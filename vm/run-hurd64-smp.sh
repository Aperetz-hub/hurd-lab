#!/bin/bash
# Test -smp 2 (console pure / headless) — ATTENDU : ÉCHEC.
#   * `-smp 2` = soft-wedge userspace / réseau KO (races SMP amont, cf. docs/suivi.md).
#   * l'option de boot `maxcpus=N`/`nosmp` ne contourne pas le problème.
#
# SÉCURITÉ DISQUE : démarre sur un **overlay temporaire** (backing hurd-dev.qcow2)
# recréé à chaque lancement -> le disque de dev n'est jamais modifié.
#
# Usage: ./run-hurd64-smp.sh [base.qcow2]
set -e
cd "$(dirname "$0")"
BASE=${1:-hurd-dev.qcow2}
OVL="$(dirname "$BASE")/hurd-smp-test.qcow2"
[ -f "$BASE" ] || { echo "base introuvable: $BASE"; exit 1; }
rm -f "$OVL"
qemu-img create -f qcow2 -F qcow2 -b "$BASE" "$OVL" >/dev/null
echo "overlay temporaire: $OVL (backing $BASE)"
MON=${MON:-/tmp/hurd-smp-mon}
rm -f "$MON"
exec qemu-system-x86_64 -m 4096 -M q35 --enable-kvm -smp 2 \
  -drive file="$OVL",cache=writeback,format=qcow2 \
  -device e1000,netdev=e0 -netdev user,id=e0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=v1 -netdev user,id=v1,hostfwd=tcp:127.0.0.1:2223-:22 \
  -display none -serial file:/tmp/hurd-smp-serial.log \
  -monitor unix:"$MON",server,nowait
