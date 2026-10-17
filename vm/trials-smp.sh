#!/bin/bash
# Manches repetees pour classer le gel `-smp 2` (BOOT / WEDGE) d'un noyau
# donne, sur un overlay JETABLE (le SRC n'est jamais ecrit : on le copie).
#
# Principe (lecons des 3e/4e passes) :
#   * AUCUN echantillonnage continu de l'ecran pendant la manche : chaque
#     lecture `xp` tient le BQL de QEMU ~ms et desynchronise les vCPU
#     (un `vgacap.py` en continu fausse le taux de gel). On lit l'ecran UNE
#     fois, a la fin, via `vgaprobe.py` ;
#   * le noyau teste est celui installe dans l'overlay SRC (verifie par md5
#     dans le guest au moment de l'installation) — jamais l'image de base ;
#   * en cas de BOOT : controle par SSH (`nproc` + md5 du noyau) ;
#   * `maxcpus` : injection gdb de `maxcpus=1` (AP non demarre), cf.
#     `vm/maxcpus-inject.gdb`.
#
# Usage :
#   vm/trials-smp.sh <SRC-overlay> [N] [maxcpus] [ELF-du-noyau]
# Defauts : N=6, SRC=<dir>/ovl-fork-master.qcow2, ELF absent (pas d'injection).
# Variables : SMP (defaut 2), WAIT (secondes d'attente par manche, defaut 85),
#             BASE (image de base, defaut ../vm/hurd-dev.qcow2).
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
[ -f "$SRC" ] || { echo "SRC introuvable: $SRC"; exit 1; }

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
	echo "manche $MODE #$i => $V"
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
echo "=== FIN manches $MODE ($SRC) ==="
