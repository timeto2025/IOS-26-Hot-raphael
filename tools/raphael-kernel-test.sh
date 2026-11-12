#!/bin/bash
# First raphael kernel test: XNU 0152 boots on the Redmi K20 Pro with no iOS root.
#
#   tools/raphael-kernel-test.sh KERNEL STOCK_BOOT_IMG [OUT]
#
# KERNEL          kernel.development.sweet from the xnu.yml GitHub Actions artifact
# STOCK_BOOT_IMG  this phone's Android boot.img, or the HackiOS package's images/twrp-static-base.img
#
# The kernel gets no rd= and no ufsrw=, so it cannot mount or write anything: it boots,
# prints its console on the screen, and stops at "waiting for root". That stop is the
# expected result of this test. Boot with `fastboot boot OUT` only; never flash it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL=${1:?usage: tools/raphael-kernel-test.sh KERNEL STOCK_BOOT_IMG [OUT]}
STOCK=${2:?usage: tools/raphael-kernel-test.sh KERNEL STOCK_BOOT_IMG [OUT]}
OUT=${3:-$ROOT/shim/out/raphael-kernel-test.img}
WORK=$ROOT/work/raphael
mkdir -p "$WORK"

# Console on the panel, all 8 cores, no GPU/USB/Wi-Fi/log writer yet: the fewest moving parts.
CMDLINE="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress cs_integrity_skip=1 wdt=-1 sweetgpu=0 sweetusb=0 sweetlog=0"

python3 "$ROOT/tools/mkadt.py" "$WORK/raphael.adt" --highmem --raphael
python3 "$ROOT/tools/mkpayload.py" "$KERNEL" "$WORK/raphael.adt" "$WORK/payload-raphael.bin" --cmdline "$CMDLINE"
"$ROOT/shim/build-raphael.sh" "$STOCK" "$OUT" "$WORK/payload-raphael.bin"
echo
echo "Boot args: $CMDLINE"
echo "Test with: fastboot boot $OUT   (do not flash)"
