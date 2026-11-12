#!/bin/bash
# raphael: boot image that starts iOS 26 from the `darwin` partition (Redmi K20 Pro).
#
#   tools/raphael-ios-boot.sh KERNEL USERLAND_TAR_GZ ROOT_IMG STOCK_BOOT_IMG [OUT]
#
# KERNEL            kernel.development.sweet from the xnu.yml artifact (patches up to 0153)
# USERLAND_TAR_GZ   ios-userland-hot-v0.8.1.tar.gz from the IOS-26-Hot v0.8.1-ios26 release
# ROOT_IMG          the HFSX root built with prepare-rmx1931-from-device.sh build-rootfs,
#                   already written to the phone's `darwin` partition
# STOCK_BOOT_IMG    this phone's boot.img, or the HackiOS package's images/twrp-static-base.img
#
# First-boot profile: root on darwin, writable; GPU, USB, Wi-Fi, cellular and audio off
# (raphael drivers for those do not exist yet); console stays on the screen.
# Boot with `fastboot boot OUT` only; never flash it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
usage="usage: tools/raphael-ios-boot.sh KERNEL USERLAND_TAR_GZ ROOT_IMG STOCK_BOOT_IMG [OUT]"
KERNEL=${1:?$usage}
USERLAND=${2:?$usage}
ROOTIMG=${3:?$usage}
STOCK=${4:?$usage}
OUT=${5:-$ROOT/shim/out/raphael-ios.img}
for f in "$KERNEL" "$USERLAND" "$ROOTIMG" "$STOCK"; do
	[[ -f "$f" ]] || { echo "error: not found: $f" >&2; exit 1; }
done
WORK=$ROOT/work/raphael
mkdir -p "$WORK" "$(dirname "$OUT")"
rm -rf "$WORK/userland"
mkdir -p "$WORK/userland"
tar -xzf "$USERLAND" -C "$WORK/userland"

CMDLINE="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress"
CMDLINE+=" rd=ufsroot0 ufsroot=darwin ufsrw=1 rootrw=1 ufslogsecs=5"
CMDLINE+=" cs_integrity_skip=1 fips_mode=0x44 unentitled_ios_sim_launch=1 ignition_level=1"
CMDLINE+=" wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1"
CMDLINE+=" sweetgpu=0 sweetusb=0 flamereboot=0 sweetrtcoff=$(date +%s)"

python3 "$ROOT/tools/mkadt.py" "$WORK/raphael.adt" --highmem --raphael

overlay_args=(-o "$WORK/ios26-overlay-raphael.tar" --root-img "$ROOTIMG"
	--userland-dir "$WORK/userland" --timezone "${TZ:-Europe/Moscow}" --register-missing-hot-daemons)
if [[ -d "$WORK/userland/Extensions/HotMetal2.bundle" ]]; then
	overlay_args+=(--hotmetal2 "$WORK/userland/Extensions/HotMetal2.bundle")
fi
python3 "$ROOT/tools/make-ios26-overlay.py" "${overlay_args[@]}"

python3 "$ROOT/tools/mkpayload.py" "$KERNEL" "$WORK/raphael.adt" "$WORK/payload-raphael-ios.bin" \
	--overlay "$WORK/ios26-overlay-raphael.tar" --cmdline "$CMDLINE"
"$ROOT/shim/build-raphael.sh" "$STOCK" "$OUT" "$WORK/payload-raphael-ios.bin"
echo
echo "Boot args: $CMDLINE"
echo "Test with: fastboot boot $OUT   (do not flash)"
