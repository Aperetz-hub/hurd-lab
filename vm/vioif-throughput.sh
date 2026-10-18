#!/bin/bash
# Host side of the vioif0 throughput test.
#
# Answers the second question left open by the SMP stress pass: does vioif0
# (rumpnet over virtio-net) survive a sustained heavy TCP transfer, and at what
# rate, while the machine is busy.
#
# Shape of the experiment:
#
#   * a TCP sink listens on the host, and the guest pushes a fixed volume at it
#     over /dev/vioif0.  The sink's own byte count is the throughput number,
#     because it is measured by the machine that receives the bytes and does not
#     depend on the guest's clock -- which this kernel runs fast (see
#     vm/clock-probe.py);
#   * the same volume goes again with scp over the same virtio device, so the
#     difference between the two is the cost of SSH on top of the device rather
#     than a property of the device;
#   * one transfer runs on an idle machine and one under the full concurrent load
#     of the SMP stress pass -- a CPU-bound worker on the master processor set,
#     one on the slave processor set through /sbin/smp, rumpdisk-backed
#     filesystem I/O, and ICMP -- so the two differ in exactly one thing;
#   * the kernel is sampled from the outside with vm/clock-probe.py during and
#     after, so clock stability and the per-CPU clock interrupt rates are
#     measured on the same run as the traffic.
#
# Usage: vm/vioif-throughput.sh [--mib N] [--dur N]
set -u
cd "$(dirname "$0")/.."

MIB=100
DUR=60
while [ $# -gt 0 ]; do
	case "$1" in
	--mib) MIB="$2"; shift 2 ;;
	--dur) DUR="$2"; shift 2 ;;
	*) echo "unknown option: $1"; exit 2 ;;
	esac
done

MON=${MON:-/tmp/smp2-mon}
SSH_PORT=${SSH_PORT:-2223}     # the virtio-net hostfwd, i.e. vioif0
SINK_PORT=${SINK_PORT:-9999}
OUT=${OUT:-/tmp/vioif-throughput}
GUEST=${GUEST:-demo@127.0.0.1}

SSHOPT="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -p $SSH_PORT"
SCPOPT="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -P $SSH_PORT"

rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== $MIB MiB over vioif0, ${DUR}s of concurrent load, sink on port $SINK_PORT ==="

# --------------------------------------------------------------------------
# The sink.  It timestamps each connection when it is accepted, so its own
# throughput figure does not depend on when this script started it.
# --------------------------------------------------------------------------
cat > "$OUT/sink.py" <<'PY'
import socket, sys, time

port = int(sys.argv[1])
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(8)
srv.settimeout(600)
print("listening on %d" % port, flush=True)

while True:
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        break
    t0 = time.time()
    got = 0
    conn.settimeout(300)
    try:
        while True:
            chunk = conn.recv(1 << 20)
            if not chunk:
                break
            got += len(chunk)
    except OSError as exc:
        print("recv error: %s" % exc, flush=True)
    dt = time.time() - t0
    conn.close()
    mibs = got / (1 << 20)
    print("received %d bytes (%.3f MiB) in %.3f s = %.4f MiB/s from %s"
          % (got, mibs, dt, mibs / dt if dt else 0.0, addr[0]), flush=True)
PY
python3 "$OUT/sink.py" "$SINK_PORT" > "$OUT/sink.txt" 2>&1 &
SINK=$!
# Wait for the sink to say it is listening, and give up if it cannot.  Without
# this check a port collision sends every transfer to some other process and the
# run reports guest-side numbers with nothing to corroborate them -- which is
# exactly what the first run of this script did.
for i in $(seq 1 40); do
	grep -q "listening on" "$OUT/sink.txt" 2>/dev/null && break
	kill -0 $SINK 2>/dev/null || break
	sleep 0.25
done
if ! grep -q "listening on" "$OUT/sink.txt" 2>/dev/null; then
	echo "the sink did not start; is something already on port $SINK_PORT?"
	cat "$OUT/sink.txt"
	kill $SINK 2>/dev/null
	exit 1
fi
echo "sink listening on $SINK_PORT"

echo "=== uploading the guest-side helpers ==="
scp $SCPOPT vm/vioif-send.py "$GUEST":/home/demo/ 2>&1 | grep -v Warning
scp $SCPOPT vm/smp-stress-burn.c vm/smp-stress-clock.c vm/smp-stress.sh \
	"$GUEST":/home/demo/ 2>&1 | grep -v Warning
ssh $SSHOPT "$GUEST" '
	gcc -O2 -o /home/demo/burn /home/demo/smp-stress-burn.c &&
	gcc -O2 -o /home/demo/clock-sampler /home/demo/smp-stress-clock.c &&
	echo "guest helpers built"
	chmod +x /home/demo/vioif-send.py /home/demo/smp-stress.sh
' 2>&1 | grep -v Warning

# The guest's own clock trace is collected into $OUT/clock.txt, so the host-side
# probe log has a different name: two files of different provenance under one
# name is how the first run of this script lost its own measurements.
sample() { # sample <label>
	echo "--- clock-probe: $1 at host $(date +%s.%N)" >> "$OUT/clock-probe.txt"
	python3 vm/clock-probe.py "$MON" 10 --json "$OUT/clock-$1.json" \
		>> "$OUT/clock-probe.txt" 2>&1 || echo "clock-probe $1 FAILED"
}

echo "=== clock baseline on the idle machine ==="
sample idle-before

echo "=== raw TCP $MIB MiB, idle machine ==="
ssh $SSHOPT "$GUEST" \
	"python3 /home/demo/vioif-send.py $MIB 10.0.2.2 $SINK_PORT /tmp/vioif-tcp-idle.json" \
	2>&1 | grep -v Warning | tee "$OUT/tcp-idle.txt"

echo "=== raw TCP $MIB MiB, under ${DUR}s of concurrent load ==="
ssh $SSHOPT "$GUEST" \
	"sh /home/demo/smp-stress.sh $DUR /tmp/stress >/tmp/load.log 2>&1" &
LOAD=$!
sleep 5
t0=$(date +%s.%N)
ssh $SSHOPT "$GUEST" \
	"python3 /home/demo/vioif-send.py $MIB 10.0.2.2 $SINK_PORT /tmp/vioif-tcp-load.json" \
	2>&1 | grep -v Warning | tee "$OUT/tcp-load.txt"
t1=$(date +%s.%N)
echo "loaded transfer wall time (host): $(echo "$t1 - $t0" | bc) s"

wait $LOAD 2>/dev/null

echo "=== scp $MIB MiB over the same virtio device (SSH on top) ==="
ssh $SSHOPT "$GUEST" \
	"dd if=/dev/zero of=/tmp/scpblob bs=1M count=$MIB 2>/dev/null; sync" 2>&1 | grep -v Warning
t0=$(date +%s.%N)
scp $SCPOPT "$GUEST":/tmp/scpblob "$OUT/scpblob" 2>&1 | grep -v Warning
t1=$(date +%s.%N)
echo "scp host wall time: $(echo "$t1 - $t0" | bc) s"
ls -l "$OUT/scpblob" 2>/dev/null

sample after

echo "=== collecting the guest-side results ==="
for f in summary.txt clock.txt disk.log ping-gateway.txt ping-external.txt \
	 burn-master.txt burn-slave.txt; do
	scp $SCPOPT "$GUEST":/tmp/stress/"$f" "$OUT/$f" 2>/dev/null
done
for f in vioif-tcp-idle.json vioif-tcp-load.json; do
	scp $SCPOPT "$GUEST":/tmp/"$f" "$OUT/$f" 2>/dev/null
done

echo
echo "======================= SUMMARY ======================="
echo "--- sink measurements (authoritative for throughput) ---"
cat "$OUT/sink.txt"
echo
echo "--- guest-reported transfer figures ---"
echo "(the guest's own clock runs about twice real time on this kernel, so its"
echo " durations are about double the real ones; the sink line above is the real"
echo " one, and the marks in the JSON show where the time went)"
cat "$OUT/tcp-idle.txt" 2>/dev/null
cat "$OUT/tcp-load.txt" 2>/dev/null
echo
echo "--- concurrent load, during the loaded transfer ---"
sed -n '/stress end/,$p' "$OUT/summary.txt" 2>/dev/null | head -20
echo
echo "--- ICMP under load ---"
tail -2 "$OUT/ping-gateway.txt" 2>/dev/null
echo
echo "--- per-CPU clock interrupt rates ---"
python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
for tag in ("idle-before", "after"):
    path = os.path.join(out, "clock-%s.json" % tag)
    if not os.path.exists(path):
        continue
    d = json.load(open(path))
    print("%-12s elapsed_ticks %7.3f Hz   uptime/host %.4f   cpu_ticks %s"
          % (tag, d["elapsed_ticks_hz"], d["uptime_over_host"],
             {k: round(v, 2) for k, v in d["cpu_ticks_hz"].items()}))
PY

kill $SINK 2>/dev/null
echo "=== results in $OUT ==="