# Prior art in other iOS-on-Snapdragon ports (survey 2026-11-14)

76 repositories mirrored (15,552 distinct commits read by topic). What applies to raphael's open problems.

## Audio: CoreAudio's 24 MHz host clock
- UniXNU/UniXNU-sheng dea464e (BSD-2-Clause-Patent): the 24000000.0 constant behind
  AudioGetHostClockFrequency is rewritten on its shared-cache page by the kernel. Ported as xnu 0182 +
  phone/audio/hot_hostclock.h.
- sheng 1b75289 / dea464e: with a wrong clock Raw made the server write late; with the clock fixed Raw + stable +
  zero stamps from the IO start were smooth, SimpleIIR stuttered. Playback lead grown to 300 ms, ring 65536 frames.

## GPU: UI lag, blur, GPU memory
- UniXNU/AdrenoMetal (MIT) is the current upstream of our phone/metal2 (NativeBridgeMetalFrontend.mm,
  bridge/NativeMetalObjects.rs; ours ~2700/2300 lines, theirs ~5650/3600), on metal2vulkan development 555b13d5
  (we pin 720ef1ce). Relevant commits:
  - 20be9f9 render every pass in tile memory; 9ddfaf2 / 24177ba Turnip chooses each pass's rendering (binning);
  - 9c09d41 lowering thread, submit at commit, warm pipelines; c135133 driver pipelines on the asking thread;
  - dfb5edc relaxed half arithmetic; 8341127 system shader cache shared by all processes; priming;
  - 86fd95c / 4411df9 / 46fdc91 memory reuse, purgeable discard, memory pressure;
  - 7841a9f YCbCr surface textures sampled over their planes (no 63 MiB converted copy);
  - 51b7003 tile pipelines: supportsTileShaders stays NO by default (QuartzCore then takes its non-tile path).
- ry-diffusion/Chlorophyll 5e95c5d, 4a64d87, 4149e4a: QuartzCore's SIMD blur kernels translated (186/189), passes with
  mixed-size attachments and mip-level attachments (CoreAnimation 26 blur pyramid). Gallium/freedreno stack, not ours.
- UniXNU/UniXNU (umi, SM8250 with the same ios-sweet kernel lineage): 0242 GPU cache invalidate before every submit,
  0244 drawables released 3 vsyncs after the flip, 0245 IOSurface zombie views kept whole surfaces (1.5 GB, app
  switcher), 0247 small imports cleaned before submit. Our SweetSurface has no zombie list: 0245 does not apply as is.
- iola1999/hackios-redmi-k90-annibale 9186873: gesture fps 68-80 -> 100-108 (GPU clock, unmap without waiting for
  GPU idle, full-screen animation skips per-line hash).

## Haptics and keyboard feedback
- (Our mkadt.py already publishes /product/haptics closed-loop = 1.) akshaynexus/iOSPlus10 32a0112: UIKit gives keyboard/impact feedback only at support level 2 =
  MobileGestalt DeviceSupportsClosedLoopHaptics = IODeviceTree:/product/haptics "closed-loop" = 1 (D22 also
  "supports-3rd-party-haptics" = 1). Keyboard taps are CoreHaptics library patterns played on the HAL "Actuator".

## Wi-Fi
- sheng 3e679fb: Settings hands over a PMK (cipher 6, 32 bytes) - must go to the radio as 64 hex digits; joins that
  need a private address abort without /chosen/mac-address-wifi0. Both already in our tree (bridge.c, mkadt.py).
- Gsjsjzhznsz/hackios-xnu-kernel aa21593 (raphael, ath10k) and R59-R61: SAE/WPA3 join and the AP's status 53.

## Accessibility
- oosexgang/hackios-xnu-kernel a5b8df0: AccessibilityBundles must be in the root, or the accessibility tree is empty.
