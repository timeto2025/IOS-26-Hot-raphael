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
# Root on darwin, writable; Wi-Fi (WIFI=1), Bluetooth (BT=1) and audio (AUDIO=1) opt-in, cellular off.
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
# EXTRA_USERLAND=raphael-tools.tar.gz (the fork's raphael-tools workflow): newer wland + dash on top
if [[ -n "${EXTRA_USERLAND:-}" ]]; then
	tar -xzf "$EXTRA_USERLAND" -C "$WORK/userland"
	echo "extra userland: $(tar -tzf "$EXTRA_USERLAND" | grep -c '^./bin/.') tools from $EXTRA_USERLAND"
fi

CMDLINE="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress"
CMDLINE+=" rd=ufsroot0 ufsroot=darwin ufsrw=1 rootrw=1 ufslogsecs=5"
CMDLINE+=" cs_integrity_skip=1 fips_mode=0x44 unentitled_ios_sim_launch=1 ignition_level=1"
CMDLINE+=" wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1"
# Time of day = PM8150 RTC + sweetrtcoff. The offset is real time - RTC (tools/raphael-rtc.py writes it to
# work/rtc-offset once, with the phone booted). Without it the PC's Unix time is passed, which is too far ahead
# by the RTC's own count (the clock then waits for iOS's network time).
if [[ -s "$ROOT/work/rtc-offset" ]]; then
	RTCOFF=$(<"$ROOT/work/rtc-offset")
else
	RTCOFF=$(date +%s)
	echo "RTC: no work/rtc-offset (python3 tools/raphael-rtc.py PHONE_IP once): the phone's clock will be ahead"
fi
CMDLINE+=" sweetfb_noconsole=1 flamereboot=0 sweetrtcoff=$RTCOFF"
if [[ "${USB:-1}" == 0 ]]; then CMDLINE+=" sweetusb=0"; else CMDLINE+=" sweetusb=2 sweetusbframes=0"; fi
# WIFI=1: Wi-Fi bridge on (sweetwifid=1) and this board's carve-outs for wland, from raphael's DT
# (tools/raphael-wifi-facts.py): modem_region 0x8e000000+0x9600000, pil_wlan_fw_region (MSA)
# 0x8bc00000+0x180000. WLANMPSS= / WLANMSA= override. Firmware: tools/raphael-stage-fw.sh.
if [[ "${WIFI:-0}" == 1 || -n "${WLANMPSS:-}" ]]; then
	CMDLINE+=" sweetwifid=1 wlanmpss=${WLANMPSS:-0x8e000000:0x9600000} wlanmsa=${WLANMSA:-0x8bc00000:0x180000}"
	[[ "${WIFI_KLOG:-1}" == 1 ]] && CMDLINE+=" wlandklog=1"   # wland log into the kernel log ring (bring-up)
	WLAND_START=1
fi
gpu_args=()
if [[ -n "${GPUFW:-}" ]]; then
	for fw in a640_gmu.bin a630_sqe.fw a640_zap.mdt a640_zap.b02; do
		[[ -s "$GPUFW/$fw" ]] || { echo "error: $GPUFW/$fw missing" >&2; exit 1; }
	done
	gpu_args=(--gpufw "$GPUFW")
	# GPU pool reserve for backboardd/SpringBoard (default 160 MB): with v0.8.1's HotMetal2 those two hold
	# ~750 of 944 MB on raphael, so every other Metal client (keyboard, apps) was refused. GPURESERVE= overrides.
	CMDLINE+=" sweetgpu=1 hotmtl=2 sweetgpufreq=6 sweetgpureserve=${GPURESERVE:-32}"
else
	CMDLINE+=" sweetgpu=0"
fi

# Canvas 1125x2499 like RMX1931: the kernel's framebuffer (SWEET_FB_LOGICAL_H) and touch (SweetHID)
# present a 1125x2499 display; a 2437 ADT canvas made backboardd render 1125x2437 into it and
# grow its GPU pool share until allocations failed (missing icons, wallpaper, Control Center).
# EXTRA_ARGS="a=1 b=2": appended to the boot args (A/B tests, e.g. raphpanel=0 or GPURESERVE checks)
# CANVAS_H (default 2436, kernel 0167+): the canvas iOS draws, 1125 wide. 2436 is the iPhone 11 Pro's own
# (375x812 points @3x) and the K20 Pro panel's shape (1125x2436 x 0.96 = 1080x2339): no bars, no stretch.
# The kernel (sweetfb_h=), touch and the ADT must agree. CANVAS_H=2499 for kernels before 0167.
CANVAS_H=${CANVAS_H:-2436}
[[ "$CANVAS_H" != 2499 ]] && CMDLINE+=" sweetfb_h=$CANVAS_H"
[[ -n "${EXTRA_ARGS:-}" ]] && CMDLINE+=" $EXTRA_ARGS"
# BT_ADDR=AA:BB:CC:DD:EE:FF: the Bluetooth address for Settings > About and bluetoothd (btup prints the
# controller's: BD_ADDR); the default is a locally administered placeholder.
if [[ -z "${BT:-}" ]]; then ls "$ROOT"/work/fw/bt/crbtfw*.tlv >/dev/null 2>&1 && BT=1 || BT=0; fi
bt_addr_args=()
[[ -n "${BT_ADDR:-}" ]] && bt_addr_args=(--bt-addr "$BT_ADDR")
[[ "${BT:-0}" == 1 ]] && bt_addr_args+=(--bt-node --bt-transport "${BT_TRANSPORT_ENCODING:-3}")
python3 "$ROOT/tools/mkadt.py" "$WORK/raphael.adt" --highmem --raphael --canvas-height "$CANVAS_H" "${bt_addr_args[@]}"

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
overlay_args+=(--wallpaper "$WALLPAPER" --wallpaper-size "1125x$CANVAS_H")
# WLAN_CONF=FILE ("ssid=NAME" / "psk=PASSPHRASE" lines): wland joins that network by itself at boot.
# It ends up in the boot image and in darwin (/var/local/wlan.conf, 0600); never in git.
if [[ -n "${WLAN_CONF:-}" ]]; then
	grep -q '^ssid=' "$WLAN_CONF" || { echo "error: $WLAN_CONF has no ssid= line" >&2; exit 1; }
	overlay_args+=(--extra "$WLAN_CONF:private/var/local/wlan.conf:0600")
	echo "Wi-Fi: wlan.conf staged for '$(sed -n 's/^ssid=//p' "$WLAN_CONF" | head -1)'"
fi
if [[ "${WLAND_START:-0}" == 1 ]]; then
	# wland must know the raphael carve-outs (wlanmpss/wlanmsa): userland newer than v0.8.1
	grep -aq "wlanmpss=" "$WORK/userland/bin/wland" || {
		echo "error: this userland's wland ignores wlanmpss=/wlanmsa= (RMX1931 addresses); use the fork's ios-userland build" >&2
		exit 1
	}
	overlay_args+=(--wland-autostart)
fi
# RMTFS_RESTORE=1 (off by default: +24 MB makes the image too big for ABL, it drops to fastboot): the modem's EFS copies in darwin (/var/local/fw/rmtfs, which the modem
# writes back through rmtfs) are put back to the phone's own partitions (work/fw/rmtfs from
# raphael-extract-fw.sh) with every new image: hard resets in the middle of an EFS write left them
# half-written. RMTFS_RESTORE=0 keeps what the modem wrote.
if [[ "${RMTFS_RESTORE:-0}" == 1 ]]; then
	n=0
	for img in modemst1 modemst2 fsg fsc; do
		if [[ -f "$ROOT/work/fw/rmtfs/$img.img" ]]; then
			overlay_args+=(--extra "$ROOT/work/fw/rmtfs/$img.img:private/var/local/fw/rmtfs/$img.img:0600")
			n=$((n + 1))
		fi
	done
	echo "rmtfs: $n EFS image(s) restored from work/fw/rmtfs"
fi
# Bluetooth firmware (tools/raphael-extract-bt.sh): small, staged with every image for phone/bt/btup.
n=0
for f in "$ROOT"/work/fw/bt/crbtfw*.tlv "$ROOT"/work/fw/bt/crnv*.bin; do
	[[ -f "$f" ]] || continue
	overlay_args+=(--extra "$f:private/var/local/fw/bt/$(basename "$f"):0644")
	n=$((n + 1))
done
[[ $n -gt 0 ]] && echo "Bluetooth: $n firmware file(s) staged at /var/local/fw/bt"
# Daemons that only crash-loop on raphael (no Bluetooth driver yet, no SEP): STRIP_JOBS="" keeps them all.
# BT=1 (default when work/fw/bt has the firmware): bluetoothd started through btup, on the kernel control
# com.apple.uart.bluetooth (kernel 0169+), BlueTool replaced by /var/local/bin/bluetool.
if [[ "$BT" == 1 ]]; then
	overlay_args+=(--bluetooth "${BT_TRANSPORT:-H4BC:POSIX:com.apple.uart.bluetooth,115200}")
	STRIP_JOBS=${STRIP_JOBS-com.apple.fairplayd.H2 com.apple.seputil}
else
	STRIP_JOBS=${STRIP_JOBS-com.apple.bluetoothd com.apple.fairplayd.H2 com.apple.seputil}
fi
for j in $STRIP_JOBS; do overlay_args+=(--strip-job "$j"); done
# Jobs the RMX1931 root strips but raphael needs: KEEP_JOBS="" strips them too. nanoregistryd answers
# com.apple.nano.nanoregistry.paireddeviceregistry ("no watch"); without it Settings > Bluetooth hangs in
# NanoRegistry's synchronous device-collection query.
KEEP_JOBS=${KEEP_JOBS-com.apple.nanoregistryd}
for j in $KEEP_JOBS; do overlay_args+=(--keep-job "$j"); done
# Mach services whose server never answers on raphael; their clients then fail at once instead of hanging
# (com.apple.audio.hapticd: Settings > Bluetooth hung 12 s in CoreHaptics and was killed). DROP_MACH="" keeps them.
DROP_MACH=${DROP_MACH-com.apple.audio.hapticd}
for m in $DROP_MACH; do overlay_args+=(--drop-mach-service "$m"); done
# AUDIO=1: speaker through adspd (LPASS ADSP from /var/local/fw/modem/adsp.*, QUAT MI2S SD0 24-bit, TFA9874
# on qup1 i2c) and the HotAudio HAL plug-in. Needs the raphael adspd from EXTRA_USERLAND (raphael-tools) and
# work/fw/audio/tfa98xx.cnt (this phone's /vendor/firmware/tfa98xx.cnt; never in git).
if [[ "${AUDIO:-0}" == 1 ]]; then
	grep -aq "tfa9874" "$WORK/userland/bin/adspd" 2>/dev/null || {
		echo "error: AUDIO=1 needs the raphael adspd (EXTRA_USERLAND=raphael-tools.tar.gz)" >&2
		exit 1
	}
	[[ -s "$ROOT/work/fw/audio/tfa98xx.cnt" ]] || { echo "error: work/fw/audio/tfa98xx.cnt missing" >&2; exit 1; }
	overlay_args+=(--enable-adsp --extra "$ROOT/work/fw/audio/tfa98xx.cnt:private/var/local/fw/audio/tfa98xx.cnt:0644")
	[[ -d "$WORK/userland/HAL/HotAudio.driver" ]] || echo "Audio: warning: no HAL/HotAudio.driver in the userland"
	echo "Audio: adspd + TFA9874 container staged"
fi
# REDUCE_FX=1: Reduce Transparency + Reduce Motion preset in com.apple.Accessibility (mobile's preferences), for when
# Settings > Accessibility does not open. backboardd's blur runs on Apple tile shaders that HotMetal2 refuses
# (tile_simd_blur_*, tile_downsample_*): the fallback costs ~35 passes / 31 Mpx per frame. REDUCE_FX=0 writes them off.
if [[ -n "${REDUCE_FX:-}" ]]; then
	python3 - "$WORK/com.apple.Accessibility.plist" "$REDUCE_FX" <<'PY'
import plistlib, sys
on = sys.argv[2] == '1'
plistlib.dump({'EnhancedBackgroundContrastEnabled': on, 'ReduceMotionEnabled': on,
               'ReduceMotionReduceSlideTransitionsPreference': on}, open(sys.argv[1], 'wb'), fmt=plistlib.FMT_BINARY)
PY
	overlay_args+=(--extra "$WORK/com.apple.Accessibility.plist:private/var/mobile/Library/Preferences/com.apple.Accessibility.plist:0600")
	echo "Accessibility: Reduce Transparency + Reduce Motion = $REDUCE_FX"
fi
# MTL2_CLIENTS='* -chronod ...' (spaces or newlines): which processes get HotMetal2 (/var/local/mtl2-clients);
# default as upstream: everyone but chronod, cameracaptured, WidgetRenderer_Default and PosterBoard.
if [[ -n "${MTL2_CLIENTS:-}" ]]; then
	overlay_args+=(--mtl2-clients "$(tr ' ' '\n' <<<"$MTL2_CLIENTS")")
	echo "HotMetal2 clients: $(tr '\n' ' ' <<<"$MTL2_CLIENTS")"
fi
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
# AVBTOOL=path/to/avbtool.py: also write OUT-avb.img with an unsigned AVB hash footer for the
# 128 MiB recovery partition (tools/raphael-move-recovery.py); flash that one to recovery.
# Use images/twrp-original.img as STOCK_BOOT_IMG for recovery: its recovery_dtbo is kept.
if [[ -z "${AVBTOOL:-}" ]]; then
	for c in "$ROOT/downloads/avb/avbtool.py" "$HOME"/Downloads/HackiOS-*/tools/scripts/avbtool.py; do
		[[ -f "$c" ]] && { AVBTOOL=$c; break; }
	done
fi
if [[ -n "${AVBTOOL:-}" ]]; then
	AVB_OUT=${OUT%.img}-avb.img
	cp "$OUT" "$AVB_OUT"
	python3 "$AVBTOOL" add_hash_footer --image "$AVB_OUT" --partition_name recovery \
		--partition_size "${RECOVERY_SIZE:-134217728}" --algorithm NONE
	echo "recovery image with AVB footer: $AVB_OUT  ->  fastboot flash recovery $AVB_OUT"
fi
if [[ "${RECOVERY:-0}" == 1 ]]; then
	size=$(stat -c%s "$OUT")
	echo "recovery image: $size bytes (partition 67108864)"
	(( size <= 67108864 )) || { echo "error: too big for recovery" >&2; exit 1; }
	echo "First boot the full image once (fastboot boot), then: fastboot flash recovery $OUT"
else
	echo "Test with: fastboot boot $OUT   (do not flash)"
fi
