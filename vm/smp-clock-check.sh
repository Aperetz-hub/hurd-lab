#!/bin/bash
# Pass 5 validator: N rounds of `-smp 2` against a given master overlay,
# classifying each round by the KERNEL CLOCK rather than by the screen.
#
#   BOOT    = the kernel clock is running (elapsed_ticks grows at ~100 Hz)
#   FROZEN  = the kernel clock stopped (elapsed_ticks stuck in the low tens),
#             which is the signature of the -smp 2 hang: CPU0 loses its 100 Hz
#             tick when the AP starts, so time/timeouts/preemption freeze.
#
# The clock is read straight out of guest memory through the QEMU monitor, so
# the classification never depends on what the console happens to show.
#
# Usage: smp-clock-check.sh <overlay> [rounds] [wait_seconds]
set -u
cd "$(dirname "$0")"
OVL_SRC=${1:-/home/ben/hurd-fork/vm/ovl-fork-master.qcow2}
N=${2:-4}
WAIT=${3:-50}
BASE=/home/ben/hurd-fork/vm/hurd-dev.qcow2
PROBE=$(dirname "$0")/kprobe.py
[ -x "$PROBE" ] || PROBE=/tmp/gm/kprobe.py
# The probe resolves the addresses of the kernel globals in this ELF: it MUST
# be the ELF of the kernel actually installed in the overlay under test, or the
# readings are meaningless (all zeros).
export KPROBE_ELF=${KPROBE_ELF:-/tmp/gnumach-new}
[ -f "$KPROBE_ELF" ] || { echo "KPROBE_ELF not found: $KPROBE_ELF"; exit 1; }

frozen=0
booted=0
for i in $(seq 1 "$N"); do
	OVL=/home/ben/hurd-fork/vm/ovl-cc-$$-$i.qcow2
	MON=/tmp/cc-$$-$i-mon
	rm -f "$OVL" "$MON"
	cp "$OVL_SRC" "$OVL" || exit 1
	SMP=2 FRESH=0 OVL="$OVL" MON="$MON" SSH_PORT=$((3940 + i)) \
		VIOIF_PORT=$((3941 + i)) ./run-hurd64-smp.sh "$BASE" \
		> /tmp/cc-$$-$i.log 2>&1 &
	sleep "$WAIT"
	python3 "$PROBE" "$MON" 0 > /tmp/cc-$$-$i.probe 2>&1
	t1=$(grep '^  elapsed_ticks' /tmp/cc-$$-$i.probe | head -1 | awk '{print $2}')
	up=$(grep '^  uptime' /tmp/cc-$$-$i.probe | head -1 | awk '{print $2}')
	sleep 10
	t2=$(python3 "$PROBE" "$MON" 0 2>/dev/null | grep '^  elapsed_ticks' | head -1 | awk '{print $2}')
	if [ -n "${t1:-}" ] && [ -n "${t2:-}" ] && [ "$t2" -gt 200 ] 2>/dev/null; then
		v="BOOT"
		booted=$((booted + 1))
	else
		v="FROZEN"
		frozen=$((frozen + 1))
	fi
	printf 'round %d: %-6s elapsed_ticks %s -> %s   uptime=%s\n' "$i" "$v" "${t1:-?}" "${t2:-?}" "${up:-?}"
	printf 'quit\n' | timeout 8 socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1
	sleep 3
	rm -f "$MON" "$OVL"
done
echo "=== $(basename "$OVL_SRC"): BOOT=$booted FROZEN=$frozen (of $N) ===" | tee -a /tmp/gm/cc-summary.log
[ "$frozen" -eq 0 ]