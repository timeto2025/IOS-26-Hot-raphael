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
# The daemons' logs through the kernel's file server (TCP 2223, as hot-dbg does): ssh needs the image's root key
# and its BatchMode attempt failed on the user's PC (20261114-123757: no mtl2/adspd/wland logs in the archive).
python3 - "$ROOT/tools/hot-dbg.py" "$HOST" "$DIR" <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("hotdbg", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
host, out = sys.argv[2], sys.argv[3]
client = None
for name in dir(m):
    cls = getattr(m, name)
    if isinstance(cls, type) and hasattr(cls, "get_file") and hasattr(cls, "klog"):
        client = cls(host)
        break
if client is None:
    sys.exit("hot-dbg.py: no client class with get_file")
for f in ("mtl2.log", "adspd.log", "wland.log", "mtl2-clients", "mtl2-env", "wlan.log"):
    try:
        raw = client.get_file("/var/local/" + f, max_bytes=8 * 1024 * 1024)
    except Exception as e:
        print(f"  {f}: {e}")
        continue
    if raw:
        open(f"{out}/{f}.txt", "wb").write(raw)
        print(f"  {f}: {len(raw)} bytes")
    else:
        print(f"  {f}: not on the phone (or empty)")
PY
[ -f "$HOME/build.log" ] && cp "$HOME/build.log" "$DIR/build.log"
OUT="$HOME/raphael-logs-$TS.tar.gz"
tar -czf "$OUT" -C "$(dirname "$DIR")" "raphael-logs-$TS"
rm -rf "$(dirname "$DIR")"
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)):"
tar -tzf "$OUT" | sed 's|^|  |'
