# IOS-26-Hot on the Redmi K20 Pro / Mi 9T Pro (raphael)

Status: bring-up. Nothing here writes the phone's flash: every test is `fastboot boot` only.

| Step | What | Result |
|---|---|---|
| 1 | Zero-write shim probe (`shim/build-raphael.sh`) | **Works**: 8 cores, 6 GB RAM, SPMI/PMIC, UFS read, display 1080x2340 @ 0x9c000000 (cmd mode + TE, no DSC) |
| 2 | XNU 0152 kernel boots with no root (`tools/raphael-kernel-test.sh`) | **Works**: 8/8 CPUs, AMP E/P, AMFI/Sandbox/HFS up, stops at "Waiting on ... Apple_HFS" (no root given) |
| 3 | Goodix GT9886 touch, patch 0153 (`raphtouchlog=1` prints frames) | **Works**: touches reported; volume keys scroll the console (needs `sweetprobe=1`) |
| 4 | iOS 26 root in the `darwin` partition (replaces HackiOS iOS 16.5), writable: 60G HFSX from `build-rootfs`, boot image from `tools/raphael-ios-boot.sh` | to test |
| 5 | Wi-Fi (WCN3990 + raphael board data), audio (TFA9874), cellular | to do |

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
