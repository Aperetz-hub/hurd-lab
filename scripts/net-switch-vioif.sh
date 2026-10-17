#!/bin/sh
# Bascule le reseau Hurd de /dev/eth0 (netdde e1000) vers /dev/vioif0
# (translator rump virtio-net) sur l'instance pfinet VIVANTE (aucun
# redemarrage de pfinet : cf. docs/suivi.md, "redemarrer pfinet fige").
#
# Prerequis (rumpnet deja demarre, sinon le faire) :
#   settrans -ac /dev/rumpnet /chemin/vers/rumpnet-vioif.sh
#   settrans -c  /dev/vioif0 /hurd/devnode -M /dev/rumpnet vioif0
#
# A lancer EN DETACHE (sudo) : la bascule coupe eth0, la session SSH via le
# port 2222 tombe. Le watchdog restaure eth0 si le drapeau KEEP n'est pas
# pose avant WAIT secondes (permet de valider vioif via le hostfwd 2223).
#
#   echo demo | sudo -S -v
#   nohup sudo KEEP=/tmp/keep-vioif WAIT=120 ./net-switch-vioif.sh &
#
# Variables : LOG KEEP WAIT IP MASK GW
set -u
LOG=${LOG:-/tmp/net-switch-vioif.log}
KEEP=${KEEP:-/tmp/keep-vioif}
WAIT=${WAIT:-120}
IP=${IP:-10.0.2.15}
MASK=${MASK:-255.255.255.0}
GW=${GW:-10.0.2.2}

exec >"$LOG" 2>&1
rm -f "$KEEP"
echo "=== bascule eth0 -> vioif0 ($(date))"

# dhclient gere eth0 : le tuer, sinon il re-active eth0.
pkill -f dhclient 2>/dev/null
ifconfig /dev/eth0 down
sleep 1
ifconfig /dev/vioif0 "$IP" netmask "$MASK" up
sleep 3
ifconfig

if ping -c2 -W2 "$GW"; then echo PING_OK; else echo PING_FAIL; fi

echo "watchdog: attente $WAIT s du drapeau $KEEP (SSH via 2223)..."
n=0
while [ "$n" -lt "$WAIT" ]; do
	[ -f "$KEEP" ] && { echo "KEEP -> vioif conserve"; exit 0; }
	sleep 1
	n=$(expr "$n" + 1)
done

echo "=== watchdog: rollback vers eth0"
ifconfig /dev/vioif0 down
sleep 1
ifconfig /dev/eth0 "$IP" netmask "$MASK" up
echo "=== rollback termine ($(date))"
