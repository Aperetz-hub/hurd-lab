#!/bin/sh
# ============================================================================
#  DEPRECATED - DO NOT USE (17/10/2026)  -> DISABLED by default.
#
#  HOT SWITCH from /dev/eth0 (netdde/e1000) to /dev/vioif0 (rumpnet) on a
#  LIVE pfinet instance.  Tests on the rebuilt dev VM: this switch renders
#  the machine UNUSABLE --
#     irq handler [11]: release a dead delivery port ...
#     irq handler [11]: still 1 unacked irqs in entry ...
#  -> network dead, VGA console no longer echoing, reboot required.
#  (This is NOT a pfinet crash: cf. docs/suivi.md § "vioif resolution".)
#
#  SUPPORTED METHOD: attach AT BOOT via /etc/network/interfaces
#  (scripts/interfaces.vioif: `auto /dev/vioif0` + pre-up start-rumpnet.sh),
#  then reboot.  To go back to eth0:
#     cp /etc/network/interfaces.eth0.bak /etc/network/interfaces && reboot
#
#  Kept for reference: refuses to run unless FORCE=1.
# ============================================================================
set -u
: "${FORCE:=0}"
if [ "$FORCE" != "1" ]; then
	echo "net-switch-vioif.sh: DISABLED (the hot switch freezes the machine)." >&2
	echo "  -> attach at boot: /etc/network/interfaces = scripts/interfaces.vioif, then reboot." >&2
	echo "  -> to force anyway: FORCE=1 $0" >&2
	exit 2
fi

# --- Old (hot-switch) logic, kept as-is ------------------------------------
LOG=${LOG:-/tmp/net-switch-vioif.log}
KEEP=${KEEP:-/tmp/keep-vioif}
WAIT=${WAIT:-120}
IP=${IP:-10.0.2.15}
MASK=${MASK:-255.255.255.0}
GW=${GW:-10.0.2.2}

exec >"$LOG" 2>&1
rm -f "$KEEP"
echo "=== switch eth0 -> vioif0 ($(date))"

# dhclient manages eth0: kill it, otherwise it re-activates eth0.
pkill -f dhclient 2>/dev/null
ifconfig /dev/eth0 down
sleep 1
ifconfig /dev/vioif0 "$IP" netmask "$MASK" up
sleep 3
ifconfig

if ping -c2 -W2 "$GW"; then echo PING_OK; else echo PING_FAIL; fi

echo "watchdog: waiting $WAIT s for the $KEEP flag (SSH via 2223)..."
n=0
while [ "$n" -lt "$WAIT" ]; do
	[ -f "$KEEP" ] && { echo "KEEP -> keeping vioif"; exit 0; }
	sleep 1
	n=$(expr "$n" + 1)
done

echo "=== watchdog: rollback to eth0"
ifconfig /dev/vioif0 down
sleep 1
ifconfig /dev/eth0 "$IP" netmask "$MASK" up
echo "=== rollback done ($(date))"
