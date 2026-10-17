#!/bin/bash
# Repeated rounds to classify the `-smp 2` hang (BOOT / WEDGE) of a given
# kernel, on a DISPOSABLE overlay (the SRC is never written: we copy it).
#
# Principle (lessons from the 3rd/4th passes):
#   * NO continuous screen sampling during the round: each `xp` read holds
#     QEMU's BQL for ~ms and desynchronizes the vCPUs (continuous
#     `vgacap.py` skews the hang rate). The screen is read ONCE, at the
#     end, via `vgaprobe.py` ;
#   * the kernel under test is the one installed in the SRC overlay
#     (verified by md5 in the guest at install time) — never the base image ;
#   * on BOOT: check over SSH (`nproc` + kernel md5) ;
#   * `maxcpus`: gdb injection of `maxcpus=1` (AP not started), cf.
#     `vm/maxcpus-inject.gdb`.
#
# Usage:
#   vm/trials-smp.sh <SRC-overlay> [N] [maxcpus] [kernel-ELF]
# Defaults: N=6, SRC=<dir>/ovl-fork-master.qcow2, ELF absent (no injection).
# Variables: SMP (default 2), WAIT (per-round wait in seconds, default 85),
#            BASE (base image, default ../vm/hurd-dev.qcow2).
set -u
cd "$(dirname "$0")"
BASE=${BASE:-/home/ben/hurd-fork/vm/hurd-dev.qcow2}
SRC=${1:-$(dirname "$BASE")/ovl-fork-master.qcow2}
N=${2:-6}
MODE=${3:-smp2}
ELF=${4:-}
SMP=${SMP:-2}
WAIT=${WAIT:-85}
GDB=${GDB:-/tmp/gdbroot/usr/bin/gdb}
GDBLIB="LD_LIBRARY_PATH=/tmp/gdbroot/usr/lib/x86_64-linux-gnu:/tmp/gdbroot/usr/lib"
[ -f "$SRC" ] || { echo "SRC not found: $SRC"; exit 1; }

for i in $(seq 1 "$N"); do
	OVL=$(dirname "$SRC")/ovl-trial-$$-$i.qcow2
	MON=/tmp/hurd-trial-$$-$i-mon
	VIO=$((3900 + i))
	GP=$((1900 + i))
	cp "$SRC" "$OVL"
	rm -f "$MON"
	if [ "$MODE" = maxcpus ] && [ -n "$ELF" ]; then
		SMP=$SMP FRESH=0 GDB=1 STOP=1 GDBPORT=$GP OVL="$OVL" MON="$MON" \
			SSH_PORT=$((3800 + i)) VIOIF_PORT=$VIO ./run-hurd64-smp.sh "$BASE" \
			> /tmp/trial-$$-$i.log 2>&1 &
		for _ in $(seq 1 24); do
			pgrep -f "qemu-system-x86_64.*drive file=$OVL" >/dev/null && break
			sleep 0.5
		done
		sleep 3
		sed "s/localhost:1234/localhost:$GP/" maxcpus-inject.gdb \
			> /tmp/maxcpus-trial-$$-$i.gdb
		timeout 100 env $GDBLIB "$GDB" -q -batch \
			-x /tmp/maxcpus-trial-$$-$i.gdb "$ELF" \
			> /tmp/trial-$$-$i.gdb.log 2>&1
		printf 'cont\n' | timeout 8 socat - UNIX-CONNECT:$MON >/dev/null 2>&1
		printf 'info status\n' | timeout 8 socat - UNIX-CONNECT:$MON 2>/dev/null | tail -1
	else
		SMP=$SMP FRESH=0 GDB=1 GDBPORT=$GP OVL="$OVL" MON="$MON" \
			SSH_PORT=$((3800 + i)) VIOIF_PORT=$VIO ./run-hurd64-smp.sh "$BASE" \
			> /tmp/trial-$$-$i.log 2>&1 &
	fi
	sleep "$WAIT"
	V=$(MON="$MON" python3 vgaprobe.py | tee /tmp/trial-$$-$i.verdict | head -1)
	echo "round $MODE #$i => $V"
	if [ "$V" = BOOT ]; then
		printf "   ssh : "
		timeout 25 ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
			-o UserKnownHostsFile=/dev/null -p "$VIO" demo@127.0.0.1 \
			'nproc; sudo -n md5sum /boot/gnumach-1.8-amd64-up.gz' 2>&1 \
			| grep -v Warning | tr '\n' ' '
		echo
	fi
	printf 'quit\n' | timeout 8 socat - UNIX-CONNECT:$MON >/dev/null 2>&1
	sleep 4
	rm -f "$MON" "$OVL"
done
echo "=== DONE rounds $MODE ($SRC) ==="
