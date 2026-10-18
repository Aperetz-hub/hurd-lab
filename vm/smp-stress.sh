#!/bin/sh
# Guest side of the SMP + vioif0 MP stress test.
#
# Runs, concurrently, for a fixed window:
#   * a CPU-bound worker on the master processor set;
#   * a CPU-bound worker on the slave processor set, via /sbin/smp;
#   * continuous filesystem I/O on the rumpdisk-backed root filesystem;
#   * continuous ICMP traffic over /dev/vioif0.
#
# While all of that runs, a 1 Hz sampler records the guest's own clock
# (/proc/uptime) next to the wall clock (date +%s).  The point of the sampler
# is that a frozen 100 Hz kernel clock shows up as /proc/uptime *stopping
# advancing* while the wall clock keeps going.
#
# Everything is written under $OUT, which the host collects afterwards.
#
# Usage (in the guest):  sh smp-stress.sh [seconds] [outdir]
set -u

DUR=${1:-60}
OUT=${2:-/tmp/stress}
BURN=${BURN:-/home/demo/burn}
SAMPLER_BIN=${SAMPLER_BIN:-/home/demo/clock-sampler}
IFACE=${IFACE:-/dev/vioif0}
GATEWAY=${GATEWAY:-10.0.2.2}

rm -rf "$OUT"
mkdir -p "$OUT" || exit 1

echo "=== stress start $(date) for ${DUR}s, outdir $OUT"

# --- the clock sampler -----------------------------------------------------
# A small C program rather than a shell loop: a shell loop forks a `date` and
# a `cat` per sample, and under load that overhead is tens of milliseconds,
# which shows up as a rate error and made the first trace unreadable.  The
# program samples three clocks in a tight loop instead: the C library's
# monotonic clock, the wall clock, and the kernel's own /proc/uptime.
if [ -x "$SAMPLER_BIN" ]; then
	"$SAMPLER_BIN" "$((DUR + 5))" "$OUT/clock.txt" &
	SAMPLER=$!
else
	echo "no $SAMPLER_BIN, the clock trace is skipped" > "$OUT/clock.txt"
	SAMPLER=""
fi

# --- two CPU-bound workers -------------------------------------------------
# Worker A runs on the master processor set, worker B on the slave one.
"$BURN" "$DUR" > "$OUT/burn-master.txt" 2>&1 &
BURNA=$!

if [ -x /sbin/smp ]; then
	# /sbin/smp is setuid root and needs the privilege for the processor
	# set control.
	sudo -n /sbin/smp "$BURN" "$DUR" > "$OUT/burn-slave.txt" 2>&1 &
else
	echo "no /sbin/smp, the slave worker is skipped" > "$OUT/burn-slave.txt"
fi
BURNB=$!

# --- filesystem I/O on the rumpdisk-backed root ---------------------------
# Write, flush and read back, so that the data really goes through the disk
# translator rather than sitting in a buffer.
(
	i=0
	while [ "$i" -lt "$DUR" ]; do
		dd if=/dev/urandom of="$OUT/diskfile" bs=64k count=4 \
			conv=fsync 2>> "$OUT/disk.log"
		dd if="$OUT/diskfile" of=/dev/null bs=64k 2>> "$OUT/disk.log"
		i=$((i + 1))
	done
	echo "$i" > "$OUT/disk-iterations.txt"
) &
DISK=$!

# --- network traffic over vioif0 ------------------------------------------
# The gateway is the QEMU user-mode network, which is the shortest path
# through the rumpnet translator and the virtio queues.
ping -i 0.2 -w "$DUR" "$GATEWAY" > "$OUT/ping-gateway.txt" 2>&1 &
PING1=$!
ping -i 0.5 -w "$DUR" 8.8.8.8 > "$OUT/ping-external.txt" 2>&1 &
PING2=$!

wait "$BURNA" "$BURNB" "$DISK" "$PING1" "$PING2" 2>/dev/null
# Only signal the sampler when there is one. "kill 0" would signal the whole
# process group, the script included.
if [ -n "$SAMPLER" ]; then
	kill "$SAMPLER" 2>/dev/null
	wait "$SAMPLER" 2>/dev/null
fi

# --- summary ---------------------------------------------------------------
{
	echo "=== stress end $(date)"
	echo "--- master worker ---"
	cat "$OUT/burn-master.txt" 2>/dev/null
	echo "--- slave worker ---"
	cat "$OUT/burn-slave.txt" 2>/dev/null
	echo "--- disk iterations ---"
	cat "$OUT/disk-iterations.txt" 2>/dev/null
	echo "--- ping gateway ---"
	tail -3 "$OUT/ping-gateway.txt" 2>/dev/null
	echo "--- ping external ---"
	tail -3 "$OUT/ping-external.txt" 2>/dev/null
	echo "--- load average ---"
	cat /proc/loadavg 2>/dev/null
} > "$OUT/summary.txt" 2>&1

cat "$OUT/summary.txt"
