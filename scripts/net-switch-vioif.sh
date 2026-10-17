#!/bin/sh
# ============================================================================
#  DECONSEILLE - NE PLUS UTILISER (17/10/2026)  -> DESACTIVE par defaut.
#
#  Bascule A CHAUD de /dev/eth0 (netdde/e1000) vers /dev/vioif0 (rumpnet) sur
#  une instance pfinet VIVANTE.  Tests sur la VM dev reconstruite : cette
#  bascule rend la machine INUTILISABLE --
#     irq handler [11]: release a dead delivery port ...
#     irq handler [11]: still 1 unacked irqs in entry ...
#  -> reseau mort, console VGA qui n'echo plus, reboot obligatoire.
#  (Ce n'est PAS un crash de pfinet : cf. docs/suivi.md § "Resolution vioif".)
#
#  METHODE SUPPORTEE : attache AU BOOT via /etc/network/interfaces
#  (scripts/interfaces.vioif : `auto /dev/vioif0` + pre-up start-rumpnet.sh),
#  puis reboot.  Pour revenir a eth0 :
#     cp /etc/network/interfaces.eth0.bak /etc/network/interfaces && reboot
#
#  Conserve pour reference : refuse de s'executer sauf FORCE=1.
# ============================================================================
set -u
: "${FORCE:=0}"
if [ "$FORCE" != "1" ]; then
	echo "net-switch-vioif.sh: DESACTIVE (la bascule a chaud fige la machine)." >&2
	echo "  -> attache au boot : /etc/network/interfaces = scripts/interfaces.vioif, puis reboot." >&2
	echo "  -> pour forcer malgre tout : FORCE=1 $0" >&2
	exit 2
fi

# --- Ancienne logique (a chaud), conservee telle quelle --------------------
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
