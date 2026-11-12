#!/bin/bash
# raphael: copy this phone's own modem/Wi-Fi firmware and modem NV off the phone, read-only.
#
#   tools/raphael-extract-fw.sh            (phone in TWRP: fastboot boot twrp-probe.img)
#
# Reads only; nothing on the phone is written or mounted read-write.
#   backup/{boot,modemst1,modemst2,fsg,fsc,persist}.img   raw partition copies (IMEI and radio
#                                                          calibration live here: keep private, never in git)
#   work/fw/modem/   the modem partition's image/ directory (modem.mdt/.bNN, wlanmdsp.mbn, bdwlan*)
#   work/fw/rmtfs/   modemst1/modemst2/fsg/fsc.img for wland's rmtfs (/var/local/fw/rmtfs)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-adb}
"$ADB" devices | grep -q 'recovery$' || { echo "error: phone not in TWRP (adb devices must say recovery)" >&2; exit 1; }
mkdir -p "$ROOT/backup" "$ROOT/work/fw/modem" "$ROOT/work/fw/rmtfs"

for p in boot modemst1 modemst2 fsg fsc persist; do
	dst=$ROOT/backup/$p.img
	if [[ -s "$dst" ]]; then
		echo "keeping $dst"
	else
		"$ADB" exec-out "dd if=/dev/block/by-name/$p bs=4096 2>/dev/null" > "$dst"
		[[ -s "$dst" ]] || { rm -f "$dst"; echo "error: could not read $p" >&2; exit 1; }
		echo "saved $p: $(stat -c%s "$dst") bytes"
	fi
done
(cd "$ROOT/backup" && sha256sum ./*.img > SHA256SUMS.txt)
for p in modemst1 modemst2 fsg fsc; do
	cp "$ROOT/backup/$p.img" "$ROOT/work/fw/rmtfs/$p.img"
done

"$ADB" shell "mkdir -p /tmp/rfw && (grep -q /tmp/rfw /proc/mounts || mount -t vfat -o ro /dev/block/by-name/modem /tmp/rfw)"
src=$("$ADB" shell "for d in /tmp/rfw/image /tmp/rfw/IMAGE; do [ -d \$d ] && echo \$d && break; done" | tr -d '\r')
[[ -n "$src" ]] || { "$ADB" shell "umount /tmp/rfw" || true; echo "error: no image/ directory on the modem partition" >&2; exit 1; }
"$ADB" pull "$src/." "$ROOT/work/fw/modem/" >/dev/null
"$ADB" shell "umount /tmp/rfw" || true
# lower-case names, as wland and the modem's TFTP requests use them
(cd "$ROOT/work/fw/modem" && for f in *; do l=$(echo "$f" | tr 'A-Z' 'a-z'); [[ "$f" == "$l" ]] || mv -f "$f" "$l"; done)

echo
echo "firmware: $(ls "$ROOT/work/fw/modem" | wc -l) files in work/fw/modem"
for f in modem.mdt wlanmdsp.mbn; do
	[[ -s "$ROOT/work/fw/modem/$f" ]] && echo "  ok   $f" || echo "  MISSING $f"
done
ls "$ROOT/work/fw/modem" | grep -i '^bdwlan' | sed 's/^/  board data: /' || echo "  MISSING bdwlan*"
echo
python3 "$ROOT/tools/raphael-wifi-facts.py" "$ROOT/backup/boot.img"
