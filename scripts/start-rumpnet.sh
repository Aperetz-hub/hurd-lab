#!/bin/sh
# Demarre le translator rumpnet (rump virtio-net) + le devnode /dev/vioif0.
# Appele au boot par /etc/network/interfaces (pre-up), avant ifup/dhclient.
# Idempotent : ne relance pas ce qui tourne deja.
set -u
WRAP=${WRAP:-/home/demo/rumpnet-vioif.sh}
LOG=${LOG:-/tmp/start-rumpnet.log}

exec >>"$LOG" 2>&1
echo "=== start-rumpnet $(date)"

rumpnet_up() { ps -ef 2>/dev/null | grep -q '[r]umpnet-vioif-test'; }
vioif_up()   { ps -ef 2>/dev/null | grep -q '[d]evnode -M /dev/rumpnet vioif0'; }

# --- translator rumpnet ---
if rumpnet_up; then
	echo "/dev/rumpnet deja actif"
else
	[ -e /dev/rumpnet ] || touch /dev/rumpnet
	settrans -ac /dev/rumpnet "$WRAP"
	echo "settrans /dev/rumpnet rc=$?"
fi

# attendre que le driver reponde (jusqu'a 30 s)
n=0
while [ "$n" -lt 30 ]; do
	rumpnet_up && break
	sleep 1
	n=$(expr "$n" + 1)
done
echo "rumpnet actif: $(rumpnet_up && echo oui || echo non)"

# --- devnode /dev/vioif0 ---
if vioif_up; then
	echo "/dev/vioif0 deja actif"
else
	[ -e /dev/vioif0 ] || touch /dev/vioif0
	settrans -ac /dev/vioif0 /hurd/devnode -M /dev/rumpnet vioif0
	echo "settrans /dev/vioif0 rc=$?"
fi
sleep 1

echo "vioif0 actif: $(vioif_up && echo oui || echo non)"
ps -ef 2>/dev/null | grep -E '[r]umpnet-vioif-test|[d]evnode -M /dev/rumpnet'
echo "=== done $(date)"
