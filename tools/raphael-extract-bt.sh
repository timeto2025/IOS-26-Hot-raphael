#!/bin/bash
# raphael: copy this phone's own Bluetooth firmware (WCN3990 rampatch + NVM) off the phone, read-only.
#
#   tools/raphael-extract-bt.sh            (phone in TWRP)
#
# Looks in the bluetooth partition (vfat, image/) and the vendor partition (firmware/); mounts read-only.
# Writes work/fw/bt/crbtfw*.tlv, crnv*.bin. tools/raphael-ios-boot.sh puts them at /var/local/fw/bt
# (for phone/bt/btup). Qualcomm's firmware: never in git.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-adb}
"$ADB" devices | grep -q 'recovery$' || { echo "error: phone not in TWRP (adb devices must say recovery)" >&2; exit 1; }
mkdir -p "$ROOT/work/fw/bt"
got=0
for spec in "bluetooth:vfat" "vendor:ext4"; do
	part=${spec%%:*}; fs=${spec##*:}
	"$ADB" shell "mkdir -p /tmp/rbt && (grep -q ' /tmp/rbt ' /proc/mounts || mount -t $fs -o ro /dev/block/by-name/$part /tmp/rbt)" 2>/dev/null || continue
	for f in $("$ADB" shell "find /tmp/rbt -maxdepth 3 \\( -iname 'crbtfw*.tlv' -o -iname 'crnv*.bin' \\) 2>/dev/null" | tr -d '\r'); do
		l=$(basename "$f" | tr 'A-Z' 'a-z')
		"$ADB" pull "$f" "$ROOT/work/fw/bt/$l" >/dev/null && got=$((got + 1)) && echo "  $part: $l"
	done
	"$ADB" shell "umount /tmp/rbt" 2>/dev/null || true
	[[ $got -gt 0 ]] && break
done
[[ $got -gt 0 ]] || { echo "error: no crbtfw*.tlv / crnv*.bin on bluetooth or vendor" >&2; exit 1; }
ls -l "$ROOT/work/fw/bt"
