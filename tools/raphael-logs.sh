#!/usr/bin/env bash
# All of a running raphael's logs in one archive to send: tools/raphael-logs.sh [PHONE_IP]
#   kernel log (whole ring), status, doctor, crash ring, and the daemons' own logs from /var/local
#   (HotMetal2 mtl2.log, adspd.log, wland.log, mtl2-clients), plus ~/build.log of the last image build.
# Writes ~/raphael-logs-YYYYmmdd-HHMMSS.tar.gz. Over Wi-Fi: hot-dbg's TCP 2223 and ssh (root key of the image).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DBG="python3 $ROOT/tools/hot-dbg.py"
HOST=${1:-$(cat "$ROOT/work/.hot-host" 2>/dev/null)}
[ -n "$HOST" ] || { echo "usage: $0 PHONE_IP" >&2; exit 1; }
HOST=${HOST%%:*}
TS=$(date +%Y%m%d-%H%M%S)
DIR=$(mktemp -d)/raphael-logs-$TS
mkdir -p "$DIR"
$DBG connect "$HOST" >/dev/null || { echo "phone $HOST does not answer on TCP 2223" >&2; exit 1; }
echo "phone $HOST: collecting..."
$DBG status  > "$DIR/status.txt"  2>&1
$DBG doctor  > "$DIR/doctor.txt"  2>&1
$DBG crashes > "$DIR/crashes.txt" 2>&1
$DBG klog --all > "$DIR/klog.txt" 2>&1
for f in mtl2.log adspd.log wland.log mtl2-clients; do
	ssh -o ConnectTimeout=10 -o BatchMode=yes "root@$HOST" \
		"[ -f /var/local/$f ] && while IFS= read -r l || [ -n \"\$l\" ]; do echo \"\$l\"; done < /var/local/$f" \
		> "$DIR/$f.txt" 2>/dev/null || true
	[ -s "$DIR/$f.txt" ] || rm -f "$DIR/$f.txt"
done
[ -f "$HOME/build.log" ] && cp "$HOME/build.log" "$DIR/build.log"
OUT="$HOME/raphael-logs-$TS.tar.gz"
tar -czf "$OUT" -C "$(dirname "$DIR")" "raphael-logs-$TS"
rm -rf "$(dirname "$DIR")"
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)):"
tar -tzf "$OUT" | sed 's|^|  |'
