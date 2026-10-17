#!/bin/bash
# Install a kernel image into a DISPOSABLE copy of the fork master overlay.
# The master overlay and the dev disk are never modified.
#
# Usage: install-kernel.sh <kernel.gz> <out-overlay.qcow2> [ssh_port]
# NOTE: the guest is only reachable over SSH through the virtio-net device,
#       i.e. on the VIOIF_PORT forwarded by vm/run-hurd64-smp.sh.
set -u
cd "$(dirname "$0")"
KGZ=$1
OUT=$2
SP=${3:-2840}                 # e1000 forward (unused for SSH)
VP=$((SP + 1))                # virtio-net forward = the one that answers
MASTER=/home/ben/hurd-fork/vm/ovl-fork-master.qcow2
BASE=/home/ben/hurd-fork/vm/hurd-dev.qcow2
MON=/tmp/p5-inst-mon
SSHOPT="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -p $VP"
# NB: sc spells the port option -P (uppercase); -p means "preserve times".
SCPOPT="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -P $VP"
ssh_g() { ssh $SSHOPT demo@127.0.0.1 "$@" 2>&1 | grep -v "Warning"; }

[ -f "$KGZ" ] || { echo "no such kernel: $KGZ"; exit 1; }
WANT=$(md5sum "$KGZ" | cut -d' ' -f1)
echo "installing $(basename "$KGZ") md5=$WANT"

for p in $(pgrep -f "qemu-sys[t]em-x86_64" 2>/dev/null); do kill $p 2>/dev/null; done
sleep 2
rm -f "$OUT" "$MON" /tmp/hurd-smp-serial.log
cp "$MASTER" "$OUT" || exit 1

# Boot single-CPU (the SMP bug does not fire there) purely to install the file.
setsid nohup env SMP=1 FRESH=0 OVL="$OUT" MON="$MON" SSH_PORT=$SP VIOIF_PORT=$VP \
	./run-hurd64-smp.sh "$BASE" > /tmp/p5-inst.log 2>&1 < /dev/null &
disown

echo -n "waiting for SSH "
SSHOK=0
for i in $(seq 1 40); do
	sleep 5
	echo -n "."
	if ssh_g 'echo READY' | grep -q READY; then SSHOK=1; break; fi
done
echo
if [ "$SSHOK" != 1 ]; then
	echo "SSH never came up; aborting"
	printf 'quit\n' | timeout 8 socat - UNIX-CONNECT:$MON >/dev/null 2>&1
	exit 1
fi

echo "guest reports: $(ssh_g 'nproc; sudo -n md5sum /boot/gnumach-1.8-amd64-up.gz' | tr '\n' ' ')"
scp $SCPOPT "$KGZ" demo@127.0.0.1:/tmp/newkern.gz 2>&1 | grep -v "Warning"
echo "uploaded: $(ssh_g 'md5sum /tmp/newkern.gz')"
ssh_g 'sudo -n cp /tmp/newkern.gz /boot/gnumach-1.8-amd64-up.gz && sudo -n sync && echo COPIED'
GOT=$(ssh_g 'sudo -n md5sum /boot/gnumach-1.8-amd64-up.gz' | cut -d' ' -f1)
ssh_g 'sudo -n /sbin/halt' >/dev/null
sleep 6
printf 'quit\n' | timeout 8 socat - UNIX-CONNECT:$MON >/dev/null 2>&1
sleep 3
for p in $(pgrep -f "qemu-sys[t]em-x86_64" 2>/dev/null); do kill $p 2>/dev/null; done
rm -f "$MON"
if [ "$GOT" = "$WANT" ]; then
	echo "OK: kernel installed, md5 verified in $OUT"
	exit 0
fi
echo "MD5 MISMATCH: got '$GOT' want '$WANT'"
exit 1