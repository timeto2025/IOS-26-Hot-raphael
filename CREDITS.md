# Credits and Licences

`IOS-26-Hot` builds on [ios-sweet](https://github.com/dipa-studio/ios-sweet) and retains its [MIT License](LICENSE). Original additions in this repository (`HotStore 2.0`, `HotWebApp`, `libhot_appfix`, `HotAuth`, `HotAudio.driver`, `HotMetal2` iOS 26 extensions, `cellulard`, `adspd`, `RMX1931` platform drivers, and `IOS-26-Hot` host tooling) are under the MIT License (Copyright (c) 2026 the IOS-26-Hot & ios-flame contributors), with these file-level exceptions:

- `xnu/patches/` and `xnu/patches-26/` modify Apple's XNU kernel, which is licensed under the **Apple Public Source License 2.0 (`APSL-2.0`)**. All patched XNU files remain under `APSL-2.0`.
- Files adapted from third-party open-source projects retain their upstream licences, listed below.

Nothing from Apple's iOS IPSWs, Google's factory images, Oppo's firmware, or Qualcomm's proprietary firmware is included in this repository; see [README.md](README.md) for what you fetch from your own device.

## Projects This Builds On

- **XNU** (Apple, <https://github.com/apple-oss-distributions/xnu>, `APSL-2.0`): `xnu-12377.121.6` (Darwin `25.5.0` / iOS 26 generation) and `xnu-8796.141.3` (iOS 16 generation).
- **ios-sweet** (dipa-studio, <https://github.com/dipa-studio/ios-sweet>, `MIT`): the foundational XNU port to the Redmi Note 10 Pro (`SM7150`) that patches `0001`–`0081`, `shim/`, and early `tools/` originated from.
- **UniXNU** (<https://github.com/UniXNU/UniXNU>, `BSD-2-Clause-Patent` / `MIT`) & **UniXNU-zeta** (none24smu, <https://github.com/none24smu/UniXNU-zeta>, `BSD-2-Clause-Patent`):
  - `phone/metal2/NativeBridgeMetalFrontend.mm`: NativeMetal (`MIT`), extended for iOS 26 Liquid Glass (`MTLLinkedFunctions`, `bitCodeHash`, transient render-target pooling) with runtime optimizations adapted from `UniXNU-zeta` (`BSD-2-Clause-Patent`).
  - `phone/metal2/bridge/` (`NativeMetalBridge` / `NativeMetalObjects`): NativeMetal (`MIT`; see `phone/metal2/bridge/LICENSE`).
  - `phone/turnip/NativeAdrenoABI.h`: NativeAdreno (`MIT`).
- **metal2vulkan** (steelbrain, <https://github.com/steelbrain/metal2vulkan>, `GPL-2.0-or-later OR LGPL-3.0-or-later`): fetched at build time by `tools/build-m2v-bridge.sh`; `phone/metal2/bridge/patches/` fall under its licence.
- **Mesa / Turnip** (<https://gitlab.freedesktop.org/mesa/mesa>, `MIT`): freedreno Vulkan 1.3 driver for Adreno 640, fetched at build time by `tools/build-turnip-ios.sh` with `phone/turnip/patches/`.
- **ios-topaz** (MakrSas, <https://github.com/MakrSas/ios-topaz>; kernel patches `APSL-2.0`, no licence file of its own) & **HackiOS** (OOSEX GANG / the HakiOS authors, <https://github.com/oosexgang/hackios-xnu-kernel>; kernel `APSL-2.0`, `userland/src/` `MIT`, notice below):
  - `phone/wifi/radio.c` (userland port of `TopazModem`), `RedwoodWLAN` / `MiniIONetworkStack` `Apple80211` `en0` bridge, `HakiosAudio` CoreAudio HAL structure, and initial `containermanagerd`/`SpringBoard` interpose concepts.
  - `phone/wifi/radio.c` is a port of ios-topaz's kernel patches (`APSL-2.0`) and stays under `APSL-2.0`.
  - `phone/audio/audio_plugin.c`, `phone/audio/hot_audio.h`, `phone/media/inject_media.c` and `phone/media/inject_sysfix.c` are adapted from HackiOS `userland/src/` (`MIT`, Copyright (c) 2025-2026 the HakiOS authors); the changes made here are under the licence each file names.
- **Linux Kernel & Qualcomm Open-Source Tools**: `phone/wifi/`, `phone/cellular/`, and `phone/adsp/` follow the protocols documented in upstream Linux `ath10k` (`ISC`), Qualcomm `SMEM`/`SMP2P`/`GLINK`/`QRTR`/`Q6AFE`/`Q6ADM`/`Q6ASM` drivers (`GPL-2.0`), and Bjorn Andersson's `rmtfs`, `pd-mapper`, and `tqftpserv` (`BSD-3-Clause`).
- **blacktop/darwin-xnu-build** (<https://github.com/blacktop/darwin-xnu-build>, `MIT`): macOS XNU cross-build scaffolding used in `.github/workflows/xnu.yml`.
- **dash** (Herbert Xu and others, `BSD-3-Clause`), **Dropbear** (Matt Johnston and others, `MIT`-style, see its `LICENSE`) and **toybox** (Rob Landley, `0BSD`): fetched and built by `.github/workflows/userland.yml`; their binaries are in `ios-userland.tar.gz`.
- `shim/font.h`: glyphs of `cp850-8x16.psfu` from the Linux `kbd` console fonts, inherited from ios-sweet. The licence of that font file has not been verified.

## raphael port additions

- `tools/avbtool.py`: Android Verified Boot tool 1.3.0 (Copyright 2016 The Android Open Source Project, `MIT`, header in the file), the copy shipped with the HakiOS raphael package; not compared with AOSP byte for byte.
- `tools/raphael-make-darwin.py`: written for this port; its way of carving `darwin` off the end of userdata (only userdata's end and one new entry, 2 MiB alignment, the APFS container type, both GPT copies checked) follows HakiOS's `tools/userdata-carve.py` (`MIT`, notice below).
- `phone/metal2/` AdrenoMetal frontend (UniXNU/AdrenoMetal, `MIT`); `xnu/patches-26/0182` CoreAudio host clock after UniXNU-sheng (`BSD-2-Clause-Patent`); `0183` 24 MHz timebase after UniXNU-droidvm.

## Binaries

- `HotMetal2.bundle` links the metal2vulkan translator and runtime statically. The bundle as a whole is distributed under `LGPL-3.0-or-later` (metal2vulkan's terms; `GPL-2.0-or-later` is its other option). Its complete source is this repository at the commit that built it, metal2vulkan at `555b13d57e09bff70996a88cff64f1275256d5cf` with `phone/metal2/bridge/patches/` (UniXNU AdrenoMetal at the commit in `phone/metal2/ADRENOMETAL_REVISION`, `MIT`), and Mesa with `phone/turnip/patches/`; `tools/build-m2v-bridge.sh` and `tools/build-turnip-ios.sh` rebuild and relink it.
- `ios-userland.tar.gz` carries a `licenses/` directory: this file, the licence texts of the components above and `SOURCE`, which names the commit it was built from.
- The kernel is XNU with `xnu/patches-26/` under `APSL-2.0`; the patches are its published modifications.

## Notices

HackiOS user space and tools (`userland/src/`, `tools/`), from which the four files named above are adapted and `tools/raphael-make-darwin.py` takes its method:

```
MIT License

Copyright (c) 2025-2026 the HakiOS authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
