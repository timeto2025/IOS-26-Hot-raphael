#!/bin/bash
# raphael: collect the phone's logs into one archive to attach in chat.
#
#   tools/raphael-collect-logs.sh
#
# Picks the way by the phone's state:
#   iOS running, USB Ethernet up  -> sets 192.168.7.1 on the phone's USB interface (sudo), then
#                                    kernel log + status (hot-dbg) and wland logs over SSH
#   phone in TWRP                 -> kernel log ring from the end of darwin (read-only)
#   PHONE=IP tools/raphael-collect-logs.sh  -> iOS over Wi-Fi (same logs, no USB needed)
# Result: logs/raphael-logs-DATE.tar.gz (path printed at the end). Nothing on the phone is changed.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$ROOT/logs/raphael-$STAMP
mkdir -p "$OUT"
cd "$ROOT"
got=0

if adb devices 2>/dev/null | grep -q 'recovery$'; then
	echo "phone in TWRP: reading the kernel log ring from darwin"
	size=$(adb shell blockdev --getsize64 /dev/block/by-name/darwin | tr -d '\r')
	blocks=$((size / 4096))
	adb exec-out "dd if=/dev/block/by-name/darwin bs=4096 skip=$((blocks - 1025)) count=1025 2>/dev/null" > "$OUT/ios-log.bin"
	if python3 tools/flame-log-decode.py "$OUT/ios-log.bin" "$OUT/ios-log.txt"; then
		rm -f "$OUT/ios-log.bin"
		got=1
	fi
elif [[ -n "${PHONE:-}" ]]; then
	HOST=$PHONE
	echo "iOS over the network at $HOST"
	ping -c 1 -W 3 "$HOST" >/dev/null 2>&1 || { echo "$HOST does not answer"; rmdir "$OUT"; exit 1; }
else
	# the phone's USB Ethernet has a fixed MAC (ios-sweet sweet_usb.c)
	IF=$(ip -br link | awk 'tolower($3) == "02:00:5e:77:ee:01" { print $1; exit }')
	if [[ -z "$IF" ]]; then
		echo "no TWRP (adb) and no iOS USB Ethernet interface."
		echo "  iOS running: unplug and replug the cable, wait 5 s, run this again"
		echo "  otherwise:   fastboot boot ~/Downloads/HackiOS-16.5-raphael-voxelsy-04112026-V2/images/twrp-probe.img"
		rmdir "$OUT"
		exit 1
	fi
	echo "iOS USB Ethernet on $IF"
	if ! ip -br addr show "$IF" | grep -q '192\.168\.7\.1/'; then
		sudo ip addr flush dev "$IF"
		sudo ip addr add 192.168.7.1/24 dev "$IF"
	fi
	sudo ip link set "$IF" up
	ok=0
	for _ in 1 2 3 4 5 6; do
		ping -c 1 -W 2 192.168.7.2 >/dev/null 2>&1 && { ok=1; break; }
		sleep 2
	done
	HOST=192.168.7.2
	if [[ $ok == 0 ]]; then
		echo "the phone does not answer on 192.168.7.2 (USB data path stuck)."
		echo "  replug the cable and run this again, or boot TWRP and run this again:"
		echo "  fastboot boot ~/Downloads/HackiOS-16.5-raphael-voxelsy-04112026-V2/images/twrp-probe.img"
		rmdir "$OUT" 2>/dev/null
		exit 1
	fi
fi

if [[ -n "${HOST:-}" ]]; then
	timeout 60 python3 tools/hot-dbg.py connect "$HOST" >/dev/null 2>&1
	timeout 60 python3 tools/hot-dbg.py klog > "$OUT/klog.txt" 2>&1 && got=1
	timeout 60 python3 tools/hot-dbg.py status > "$OUT/status.txt" 2>&1
	SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new "root@$HOST")
	if timeout 20 "${SSH[@]}" true 2>"$OUT/ssh-error.txt"; then
		rm -f "$OUT/ssh-error.txt"
		for f in wland.log wland.log.prev netsvc.log dropbear.log hotstored.log; do
			timeout 30 "${SSH[@]}" "cat /var/local/$f" > "$OUT/$f" 2>/dev/null || rm -f "$OUT/$f"
			[[ -s "$OUT/$f" ]] || rm -f "$OUT/$f"
		done
		timeout 30 "${SSH[@]}" 'ls -la /var/local /var/local/fw 2>&1; ifconfig -a 2>&1' > "$OUT/phone-state.txt" 2>&1
		got=1
	else
		echo "SSH not available (see ssh-error.txt); kernel log and status only"
	fi
fi

if [[ $got == 0 ]]; then
	echo "nothing collected"
	exit 1
fi
ARCH=$ROOT/logs/raphael-logs-$STAMP.tar.gz
tar czf "$ARCH" -C "$ROOT/logs" "raphael-$STAMP"
echo
ls -l "$OUT"
echo
echo "attach this file in chat: $ARCH"
