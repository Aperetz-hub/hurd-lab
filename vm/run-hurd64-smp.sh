#!/bin/bash
# Test -smp 2 (console pure / headless) sur un OVERLAY JETABLE (backing
# hurd-dev.qcow2) — le disque de dev n'est jamais modifie.
#   * `-smp 2` = gel dans rumpdisk (attache AHCI) avec l'IOAPIC emule par
#     KVM (`kernel-irqchip=on`, defaut). Avec `MACHINE=q35,kernel-irqchip=split`
#     (IOAPIC emule par QEMU) le noyau fork demarre en -smp 2 et le clavier
#     repond — cf. docs/suivi.md, « Autopsie du gel -smp 2 — 2e passe ».
#   * l'option de boot `maxcpus=N`/`nosmp` ne contourne pas le probleme.
#
# Variables d'environnement :
#   SMP=2        nb de CPU QEMU (defaut 2)
#   GDB=1        active le stub GDB de QEMU (`-s`, tcp::1234)
#   STOP=1       fige la VM a la premiere instruction (`-S`, avec GDB=1)
#   GDBPORT=1234 port du stub
#   FRESH=1      recree l'overlay (defaut) ; FRESH=0 le reutilise tel quel
#   OVL=...      chemin de l'overlay (defaut <dir base>/hurd-smp-test.qcow2)
#   MON=...      socket moniteur QEMU (defaut /tmp/hurd-smp-mon)
#   MACHINE=...  machine QEMU (defaut q35) ; ex. q35,kernel-irqchip=split
#
# Usage: ./run-hurd64-smp.sh [base.qcow2]
set -e
cd "$(dirname "$0")"
BASE=${1:-hurd-dev.qcow2}
OVL=${OVL:-"$(dirname "$BASE")/hurd-smp-test.qcow2"}
SMP=${SMP:-2}
GDB=${GDB:-0}
STOP=${STOP:-0}
GDBPORT=${GDBPORT:-1234}
FRESH=${FRESH:-1}
MON=${MON:-/tmp/hurd-smp-mon}
MACHINE=${MACHINE:-q35}
SSH_PORT=${SSH_PORT:-2222}
VIOIF_PORT=${VIOIF_PORT:-2223}
[ -f "$BASE" ] || { echo "base introuvable: $BASE"; exit 1; }
if [ "$FRESH" = "1" ]; then
	rm -f "$OVL"
	qemu-img create -f qcow2 -F qcow2 -b "$BASE" "$OVL" >/dev/null
	echo "overlay jetable recree: $OVL (backing $BASE)"
else
	[ -f "$OVL" ] || { echo "overlay absent: $OVL"; exit 1; }
	echo "overlay reutilise: $OVL"
fi
rm -f "$MON"
GDBOPTS=()
[ "$GDB" = "1" ] && GDBOPTS=(-gdb "tcp::${GDBPORT}")
[ "$STOP" = "1" ] && GDBOPTS+=(-S)
echo "smp=$SMP gdb=$GDB stop=$STOP mon=$MON"
exec qemu-system-x86_64 -m 4096 -M "$MACHINE" --enable-kvm -smp "$SMP" \
  "${GDBOPTS[@]}" \
  -drive file="$OVL",cache=writeback,format=qcow2 \
  -device e1000,netdev=e0 -netdev user,id=e0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 \
  -device virtio-net-pci,netdev=v1 -netdev user,id=v1,hostfwd=tcp:127.0.0.1:${VIOIF_PORT}-:22 \
  -display none -serial file:/tmp/hurd-smp-serial.log \
  -monitor unix:"$MON",server,nowait
