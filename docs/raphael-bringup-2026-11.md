# IOS-26-Hot on the Redmi K20 Pro / Mi 9T Pro (raphael)

Status: iOS 26 boots to SpringBoard from `darwin`; the iOS boot image can live in a moved 128 MiB `recovery`
(power + volume up). Kernel tests still go through `fastboot boot`.

| Step | What | Result |
|---|---|---|
| 1 | Zero-write shim probe (`shim/build-raphael.sh`) | **Works**: 8 cores, 6 GB RAM, SPMI/PMIC, UFS read, display 1080x2340 @ 0x9c000000 (cmd mode + TE, no DSC) |
| 2 | XNU 0152 kernel boots with no root (`tools/raphael-kernel-test.sh`) | **Works**: 8/8 CPUs, AMP E/P, AMFI/Sandbox/HFS up, stops at "Waiting on ... Apple_HFS" (no root given) |
| 3 | Goodix GT9886 touch, patch 0153 (`raphtouchlog=1` prints frames) | **Works**: touches reported; volume keys scroll the console (needs `sweetprobe=1`) |
| 4 | iOS 26 root in `darwin` (28G HFSX from `build-rootfs`, written with `tools/raphael-write-darwin.py`), boot image from `tools/raphael-ios-boot.sh` | **Works**: SpringBoard, GPU (Adreno 640, HotMetal2), USB Ethernet + `hot-dbg`, static wallpaper; canvas 1125x2499 |
| 4a | Touch retries with a power cycle (0154) | **Works**: GT9886 starts on the first attempt after warm restarts |
| 4b | Real AMOLED brightness by DCS 0x51 (0155, `bsd/dev/raphael_panel.c`) | **Works**: the iOS slider changes the panel level |
| 4c | Boot from `recovery`: `tools/raphael-move-recovery.py` (GPT: recovery 128 MiB at the end of darwin), image built on `twrp-original.img` (keeps recovery_dtbo) + `AVBTOOL=` footer | **Works** (power + volume up) |
| 4d | Control Center contents, GPU pool (backboardd ~500 MB of 944), SSH shell (no `dash` in the v0.8.1 userland) | open |
| 5 | Wi-Fi: WCN3990 through the modem (wland), raphael carve-outs `wlanmpss=0x8e000000:0x9600000 wlanmsa=0x8bc00000:0x180000`, own firmware/NV staged by `tools/raphael-extract-fw.sh` + `tools/raphael-stage-fw.sh`, 3 MiB rmtfs EFS window; build with `WIFI=1 EXTRA_USERLAND=raphael-tools.tar.gz` | **Works** |

## Hardware differences from RMX1931 handled so far

- Splash framebuffer `0x9c000000`, panel 1080x2340 (RMX1931: `0x9d000000`, 1080x2400)
- GPU zap-shader carve-out `0x98f15000` (RMX1931: `0x99515000`)
- RAM: 6 or 8 GB, so the ADT's DRAM window ends at `0x200000000` (`mkadt.py --raphael`)
- Volume down is RESIN on the PON block, as on the Pixel 4 that `sweet_keys.c` already handles
- RMX1931-only drivers (Synaptics touch, I2C battery gauge, AW8697 haptic, torch, MAX98937 audio) and Wi-Fi are off by default: `#if SWEET_BOARD_RAPHAEL` in each, with an `#error` if the macro is not visible
- `ufsrw=1` on `userdata` is refused: on raphael that partition is Android's

## Step 2: build and run the first kernel test

1. GitHub → this repository → **Actions** → **XNU Kernel CI** → **Run workflow** (defaults). It builds `xnu-12377.121.6` + patches `0003..0152` on a macOS runner.
2. Download the `xnu-kernel` artifact and unzip it: it holds `kernel.development.sweet`.
3. On Linux (Arch: `sudo pacman -S --needed clang lld llvm python`):
   ```
   tools/raphael-kernel-test.sh /path/to/kernel.development.sweet /path/to/boot.img
   ```
   `boot.img` = this phone's Android boot image, or the HackiOS package's `images/twrp-static-base.img`.
4. Reboot the phone into fastboot cleanly, then one command: `fastboot boot shim/out/raphael-kernel-test.img`.

Expected: kernel console text on the panel, ending at a "waiting for root" style stop or a panic about the missing root. Photograph the last screens. A black screen, or an early reset back to fastboot, is also a result to report.

## Touch test (patch 0153)

`bsd/dev/raphael_touch.c` is this port's own GT9886 driver (helpers from IOS-26-Hot's MIT `sweet_touch.c`).
About 20 s after boot (`raphtouchdelay=S`) it prints `RAPH TOUCH:` lines. Good result:
`GT version block sum=0 pid=9886 sensor=N`, then `GT9886 ready`, then `frame n=1 ...` while a finger is down.
If X or Y is mirrored, boot with `raphtouchflip=0..3` (bit 0 = X, bit 1 = Y). `raphtouch=0` turns it off.
