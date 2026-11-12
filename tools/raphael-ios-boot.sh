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
# Root on darwin, writable; Wi-Fi, cellular and audio off (no raphael drivers yet).
# USB Ethernet debugging on by default (sweetusb=2: phone 192.168.7.2, host 192.168.7.1,
# tools/hot-dbg.py on TCP 2223); USB=0 turns it off. SSH (dropbear, root@192.168.7.2:22)
# with SSHKEY=FILE.pub, default ~/.ssh/id_ed25519.pub when it exists.
# Console text is drawn until the GUI presents, then muted (still in the log).
# GPUFW=DIR: Adreno firmware pulled from this phone's /vendor/firmware (a640_gmu.bin,
# a630_sqe.fw, a640_zap.mdt, a640_zap.b02) turns the GPU on (sweetgpu=1, HotMetal2).
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
CMDLINE+=" sweetfb_noconsole=1 flamereboot=0 sweetrtcoff=$(date +%s)"
if [[ "${USB:-1}" == 0 ]]; then CMDLINE+=" sweetusb=0"; else CMDLINE+=" sweetusb=2 sweetusbframes=0"; fi
gpu_args=()
if [[ -n "${GPUFW:-}" ]]; then
	for fw in a640_gmu.bin a630_sqe.fw a640_zap.mdt a640_zap.b02; do
		[[ -s "$GPUFW/$fw" ]] || { echo "error: $GPUFW/$fw missing" >&2; exit 1; }
	done
	gpu_args=(--gpufw "$GPUFW")
	CMDLINE+=" sweetgpu=1 hotmtl=2 sweetgpufreq=6"
else
	CMDLINE+=" sweetgpu=0"
fi

# Canvas 1125x2499 like RMX1931: the kernel's framebuffer (SWEET_FB_LOGICAL_H) and touch (SweetHID)
# present a 1125x2499 display; a 2437 ADT canvas made backboardd render 1125x2437 into it and
# grow its GPU pool share until allocations failed (missing icons, wallpaper, Control Center).
python3 "$ROOT/tools/mkadt.py" "$WORK/raphael.adt" --highmem --raphael --canvas-height 2499

overlay_args=(-o "$WORK/ios26-overlay-raphael.tar" --root-img "$ROOTIMG"
	--userland-dir "$WORK/userland" --timezone "${TZ:-Europe/Moscow}" --register-missing-hot-daemons)
if [[ -d "$WORK/userland/Extensions/HotMetal2.bundle" ]]; then
	overlay_args+=(--hotmetal2 "$WORK/userland/Extensions/HotMetal2.bundle")
fi
SSHKEY=${SSHKEY:-$HOME/.ssh/id_ed25519.pub}
if [[ -f "$SSHKEY" ]]; then
	overlay_args+=(--ssh-key "$SSHKEY")
	echo "SSH: root key $SSHKEY"
else
	echo "SSH: no key ($SSHKEY missing; create one with ssh-keygen -t ed25519)"
fi
if [[ ! -f "$WORK/userland/bin/dropbearmulti" ]]; then
	echo "SSH: warning: the userland has no bin/dropbearmulti; dropbear will not run"
fi
# Static Lock/Home wallpaper staged in the root (PosterBoard is off the GPU path): WALLPAPER=FILE.png,
# else a generated gradient.
if [[ -z "${WALLPAPER:-}" ]]; then
	WALLPAPER=$WORK/wallpaper-default.png
	python3 "$ROOT/tools/raphael-default-wallpaper.py" "$WALLPAPER"
fi
overlay_args+=(--wallpaper "$WALLPAPER" --wallpaper-size 1125x2499)
python3 "$ROOT/tools/make-ios26-overlay.py" "${overlay_args[@]}"

# RECOVERY=1: no overlay in the payload, small enough for the 64 MiB recovery partition. The kernel
# writes the overlay into darwin once (stamp /private/var/local/.overlay-stamp), so first boot the
# full image with `fastboot boot`, then flash the RECOVERY=1 image.
ov_args=(--overlay "$WORK/ios26-overlay-raphael.tar")
if [[ "${RECOVERY:-0}" == 1 ]]; then
	ov_args=()
	[[ $# -ge 5 ]] || OUT=$ROOT/shim/out/raphael-ios-recovery.img
fi
python3 "$ROOT/tools/mkpayload.py" "$KERNEL" "$WORK/raphael.adt" "$WORK/payload-raphael-ios.bin" \
	"${ov_args[@]}" --cmdline "$CMDLINE" "${gpu_args[@]}"
"$ROOT/shim/build-raphael.sh" "$STOCK" "$OUT" "$WORK/payload-raphael-ios.bin"
echo
echo "Boot args: $CMDLINE"
if [[ "${RECOVERY:-0}" == 1 ]]; then
	size=$(stat -c%s "$OUT")
	echo "recovery image: $size bytes (partition 67108864)"
	(( size <= 67108864 )) || { echo "error: too big for recovery" >&2; exit 1; }
	echo "First boot the full image once (fastboot boot), then: fastboot flash recovery $OUT"
else
	echo "Test with: fastboot boot $OUT   (do not flash)"
fi
