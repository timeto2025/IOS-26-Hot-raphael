#!/bin/bash
# Build the IOS-26-Hot bare-metal shim for the Redmi K20 Pro / Mi 9T Pro (raphael, SM8150)
# as a ZERO-WRITE probe: SHIM_READONLY compiles out every UFS write, so it only reads,
# shows what it found on the screen, and resets to fastboot.
#
#   shim/build-raphael.sh STOCK_BOOT_IMG [OUT]
#
# STOCK_BOOT_IMG is this phone's own Android boot image (for example the installer's
# backup session-private/<serial>/<stamp>/boot.before.img). Its appended DTB and its
# header fields are reused, so the image matches what this phone's ABL already boots.
# Test with `fastboot boot OUT` only; never flash it.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)

STOCK=${1:?usage: shim/build-raphael.sh STOCK_BOOT_IMG [OUT]}
OUT=${2:-out/shim-raphael-probe.img}

CC="clang --target=aarch64-none-elf"
LD="ld.lld"
OBJCOPY="llvm-objcopy"
READELF="llvm-readelf"
NM="llvm-nm"
for t in clang ld.lld llvm-objcopy llvm-readelf llvm-nm gzip python3; do
	command -v "$t" >/dev/null || { echo "missing $t (Arch: sudo pacman -S clang lld llvm)" >&2; exit 1; }
done

CFLAGS="-O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -DBOARD_RAPHAEL -DSHIM_READONLY ${SHIM_CFLAGS:-}"
SRCS="start.S shim.c console.c log.c util.c mmu.c fdt.c loader.c ufs.c spmi.c sde.c showlog.c diag.c disptest.c gpuprobe.c gpufw.S smmuprobe.c ramlog.c"

mkdir -p out/raphael
objs=
for f in $SRCS; do
	$CC $CFLAGS -c "$f" -o "out/raphael/${f%.*}.o"
	objs="$objs out/raphael/${f%.*}.o"
done

abs=$($READELF -r $(echo "$objs" | sed 's| out/raphael/start.o||') | grep -c R_AARCH64_ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }

$LD -static -nostdlib -T shim.ld --build-id=none -N $objs -o out/raphael/shim.elf
$OBJCOPY -O binary out/raphael/shim.elf out/raphael/Image
size=$($NM out/raphael/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/raphael/Image
gzip -9 -n -c out/raphael/Image > out/raphael/Image.gz

# Empty newc ramdisk (just the trailer): the probe carries no XNU payload.
python3 - out/raphael/ramdisk.bin <<'PY'
import gzip, sys
name = b'TRAILER!!!\0'
hdr = b'070701' + b''.join(b'%08x' % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
data = hdr + name
data += bytes(-len(data) % 4)
data += bytes(-len(data) % 512)
open(sys.argv[1], 'wb').write(gzip.compress(data, 9, mtime=0))
PY

python3 "$ROOT/tools/mkbootimg_raphael_probe.py" --stock "$STOCK" \
	--kernel out/raphael/Image.gz --ramdisk out/raphael/ramdisk.bin -o "$OUT"
ls -lh out/raphael/Image "$OUT"
echo "Test with: fastboot boot $OUT   (do not flash)"
