#!/bin/bash
# Host side of the SMP + vioif0 MP stress test.
#
# Boots a DISPOSABLE overlay at -smp 2 with the virtio-net device present,
# checks that the guest really has two CPUs and a working /dev/vioif0, then
# runs the guest-side stress window while sampling the kernel clock from the
# outside through the QEMU monitor.
#
# The kernel clock is sampled with vm/kprobe.py, which reads the kernel
# globals straight out of guest memory.  A single sample takes the QEMU BQL
# for a moment, so the sampler deliberately takes a few samples rather than
# running continuously: a continuous read is known to perturb the vCPUs.
# The fine-grained clock trace comes from the guest's own /proc/uptime, which
# costs the host nothing.
#
# Environment variables:
#   OVL=...      the disposable overlay to boot (required)
#   SMP=2        QEMU CPU count
#   DUR=60       stress window, seconds
#   ELF=...      kernel ELF with symbols, for kprobe.py
#   MON=...      QEMU monitor socket
#
# Usage: vm/smp-stress-run.sh
set -u
cd "$(dirname "$0")/.."
PROJECT=$(pwd)

OVL=${OVL:?set OVL to the disposable overlay}
SMP=${SMP:-2}
DUR=${DUR:-60}
ELF=${ELF:-/tmp/gnumach-fork-4207ebda}
MON=${MON:-/tmp/smp-stress-mon}
VIOIF_PORT=${VIOIF_PORT:-2223}
SSH_PORT=${SSH_PORT:-2222}
OUT=${OUT:-/tmp/smp-stress-host}

rm -rf "$OUT"; mkdir -p "$OUT"
SSHOPT="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -p $VIOIF_PORT"
SCPOPT="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -P $VIOIF_PORT"

sample() { # sample <label>
	# The host wall clock is stamped on every sample: it is completely
	# independent of the guest, so the difference between two samples is a
	# reliable time base for the kernel's tick counter.
	echo "--- kprobe sample: $1 at host $(date +%s.%N)" | tee -a "$OUT/kprobe.txt"
	KPROBE_ELF="$ELF" python3 vm/kprobe.py "$MON" 2>/dev/null \
		| grep -E "elapsed_ticks|uptime|curr_ipl" | tee -a "$OUT/kprobe.txt"
}

echo "=== booting $OVL at -smp $SMP ==="
setsid nohup env FRESH=0 SMP="$SMP" OVL="$OVL" MON="$MON" \
	VIOIF_PORT="$VIOIF_PORT" SSH_PORT="$SSH_PORT" \
	./vm/run-hurd64-smp.sh /home/ben/hurd-fork/vm/hurd-dev.qcow2 \
	> "$OUT/qemu.log" 2>&1 < /dev/null &
disown

echo -n "waiting for SSH "
for i in $(seq 1 60); do
	sleep 5; echo -n "."
	ssh $SSHOPT demo@127.0.0.1 'echo READY' 2>/dev/null | grep -q READY && break
done
echo

ssh $SSHOPT demo@127.0.0.1 'echo READY' >/dev/null 2>&1 || {
	echo "SSH never came up"; exit 1; }

echo "=== guest state ==="
ssh $SSHOPT demo@127.0.0.1 'nproc; uname -a; ifconfig /dev/vioif0 2>&1 | head -3' \
	2>&1 | grep -v Warning | tee "$OUT/guest-state.txt"

echo "=== uploading the stress harness ==="
scp $SCPOPT vm/smp-stress-burn.c vm/smp-stress-clock.c vm/smp-stress.sh \
	demo@127.0.0.1:/home/demo/ 2>&1 | grep -v Warning
ssh $SSHOPT demo@127.0.0.1 '
	gcc -O2 -o /home/demo/burn /home/demo/smp-stress-burn.c &&
	echo "burner built" || echo "BURNER BUILD FAILED"
	gcc -O2 -o /home/demo/clock-sampler /home/demo/smp-stress-clock.c &&
	echo "sampler built" || echo "SAMPLER BUILD FAILED"
' 2>&1 | grep -v Warning

echo "=== clock sample BEFORE the window ==="
sample before

echo "=== running the ${DUR}s stress window ==="
ssh $SSHOPT demo@127.0.0.1 "sh /home/demo/smp-stress.sh $DUR /tmp/stress" \
	> "$OUT/stress.txt" 2>&1 &
STRESS=$!

# Samples while the window runs.  Each one takes the QEMU BQL for a moment,
# so they are spaced out rather than continuous.
i=1
while [ "$i" -le 6 ]; do
	sleep $((DUR / 6))
	sample "during-$i"
	i=$((i + 1))
done

wait $STRESS
echo "=== the guest-side summary ==="
cat "$OUT/stress.txt"

echo "=== clock sample AFTER the window ==="
sample after

echo "=== collecting the guest-side clock trace ==="
scp $SCPOPT demo@127.0.0.1:/tmp/stress/clock.txt "$OUT/clock.txt" 2>&1 | grep -v Warning
scp $SCPOPT demo@127.0.0.1:/tmp/stress/summary.txt "$OUT/summary.txt" 2>&1 | grep -v Warning

echo "=== shutting the guest down ==="
ssh $SSHOPT demo@127.0.0.1 'sudo -n /sbin/halt' >/dev/null 2>&1
sleep 8
for p in $(pgrep -f "qemu-sys[t]em-x86_64"); do kill $p 2>/dev/null; done
sleep 2
pgrep -f "qemu-sys[t]em-x86_64" >/dev/null && echo "WARNING: a QEMU is still running"

echo "=== done, results in $OUT ==="
