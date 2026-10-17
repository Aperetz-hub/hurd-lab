#!/bin/sh
# Start the rumpnet translator (rump virtio-net) + the /dev/vioif0 devnode.
# Called at boot by /etc/network/interfaces (pre-up), before ifup/dhclient.
# Idempotent: does not restart what is already running.
set -u
WRAP=${WRAP:-/home/demo/rumpnet-vioif.sh}
LOG=${LOG:-/tmp/start-rumpnet.log}

exec >>"$LOG" 2>&1
echo "=== start-rumpnet $(date)"

rumpnet_up() { ps -ef 2>/dev/null | grep -q '[r]umpnet-vioif-test'; }
vioif_up()   { ps -ef 2>/dev/null | grep -q '[d]evnode -M /dev/rumpnet vioif0'; }

# --- translator rumpnet ---
if rumpnet_up; then
	echo "/dev/rumpnet already active"
else
	[ -e /dev/rumpnet ] || touch /dev/rumpnet
	settrans -ac /dev/rumpnet "$WRAP"
	echo "settrans /dev/rumpnet rc=$?"
fi

# wait for the driver to respond (up to 30 s)
n=0
while [ "$n" -lt 30 ]; do
	rumpnet_up && break
	sleep 1
	n=$(expr "$n" + 1)
done
echo "rumpnet active: $(rumpnet_up && echo yes || echo no)"

# --- devnode /dev/vioif0 ---
if vioif_up; then
	echo "/dev/vioif0 already active"
else
	[ -e /dev/vioif0 ] || touch /dev/vioif0
	settrans -ac /dev/vioif0 /hurd/devnode -M /dev/rumpnet vioif0
	echo "settrans /dev/vioif0 rc=$?"
fi
sleep 1

echo "vioif0 active: $(vioif_up && echo yes || echo no)"
ps -ef 2>/dev/null | grep -E '[r]umpnet-vioif-test|[d]evnode -M /dev/rumpnet'
echo "=== done $(date)"
