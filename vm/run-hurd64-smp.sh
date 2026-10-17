#!/bin/bash
# -smp 2 test (pure console / headless) on a DISPOSABLE OVERLAY — the dev
# disk and the master overlay are never modified.
#   * `-smp 2` = hang in rumpdisk (AHCI attach) with the IOAPIC emulated by
#     KVM (`kernel-irqchip=on`, default), most of the time but NOT always
#     (intermittent hang: race/timing). With `MACHINE=q35,kernel-irqchip=split`
#     (IOAPIC emulated by QEMU) the fork kernel boots at -smp 2 and the
#     keyboard responds — cf. docs/suivi.md, "Autopsy of the -smp 2 hang"
#     (2nd/3rd pass).
#   * the `maxcpus=N`/`nosmp` boot option (AP not started) avoids the hang
#     most of the time (5 boots / 6 measurements in the 3rd pass) but not
#     always: the AP startup is the main trigger, and a race component
#     remains (cf. docs/suivi.md, 3rd pass).
#   * the FORK kernel is installed in the master overlay (MASTER), not in
#     the base image: a fresh overlay (FRESH=1) therefore takes the master
#     as its backing file, otherwise the STOCK kernel would be tested
#     unawares (trap fixed in the 4th pass, cf. docs/suivi.md).
#
# Environment variables:
#   SMP=2        QEMU CPU count (default 2)
#   GDB=1        enable the QEMU GDB stub (`-s`, tcp::1234)
#   STOP=1       freeze the VM at the first instruction (`-S`, with GDB=1)
#   GDBPORT=1234 stub port
#   FRESH=1      recreate the overlay (default) ; FRESH=0 reuse it as-is
#   OVL=...      overlay path (default <base dir>/hurd-smp-test.qcow2)
#   MASTER=...   MASTER overlay (fork kernel already installed) used as the
#                backing file of a fresh overlay (default <base dir>/ovl-fork-master.qcow2)
#   STOCK=1      explicitly test the STOCK image (backing = BASE) instead of the
#                fork master ; refused by default so the stock kernel is no
#                longer tested by accident (trap: FRESH=1 on hurd-dev.qcow2 = stock)
#   MON=...      QEMU monitor socket (default /tmp/hurd-smp-mon)
#   MACHINE=...  QEMU machine (default q35) ; e.g. q35,kernel-irqchip=split
#
# Usage: ./run-hurd64-smp.sh [base.qcow2]
set -e
cd "$(dirname "$0")"
BASE=${1:-hurd-dev.qcow2}
OVL=${OVL:-"$(dirname "$BASE")/hurd-smp-test.qcow2"}
MASTER=${MASTER:-"$(dirname "$BASE")/ovl-fork-master.qcow2"}
STOCK=${STOCK:-0}
SMP=${SMP:-2}
GDB=${GDB:-0}
STOP=${STOP:-0}
GDBPORT=${GDBPORT:-1234}
FRESH=${FRESH:-1}
MON=${MON:-/tmp/hurd-smp-mon}
MACHINE=${MACHINE:-q35}
SSH_PORT=${SSH_PORT:-2222}
VIOIF_PORT=${VIOIF_PORT:-2223}
[ -f "$BASE" ] || { echo "base not found: $BASE"; exit 1; }
if [ "$FRESH" = "1" ]; then
	rm -f "$OVL"
	if [ "$STOCK" = "1" ]; then
		qemu-img create -f qcow2 -F qcow2 -b "$BASE" "$OVL" >/dev/null
		echo "WARNING: STOCK overlay recreated: $OVL (backing $BASE, kernel NOT fork)"
	else
		[ -f "$MASTER" ] || {
			echo "fork master not found: $MASTER"
			echo "  -> create it once: cp ovl-stock-smp2.qcow2 ovl-fork-master.qcow2"
			echo "  -> or knowingly test the STOCK image: STOCK=1 $0 ..."
			exit 1; }
		qemu-img create -f qcow2 -F qcow2 -b "$MASTER" "$OVL" >/dev/null
		echo "fork overlay recreated: $OVL (backing master $MASTER)"
	fi
else
	[ -f "$OVL" ] || { echo "overlay missing: $OVL"; exit 1; }
	echo "overlay reused: $OVL"
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
