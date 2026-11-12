#!/usr/bin/env python3
"""Pack the shim as a boot image for this phone's own ABL, reusing its stock boot image.

From the stock boot image (header v0/v1/v2, page 4096, gzip kernel with appended DTBs, as
raphael's MIUI and custom ROM kernels ship) this keeps every header field the ABL looks at:
load addresses, page size, header version, os_version, command line. It replaces the kernel
with the shim's Image.gz followed by the stock appended DTBs, the ramdisk with the given one,
and keeps the stock recovery_dtbo section (header v1/v2): ABL refuses to start an image from the
recovery partition without it, while `fastboot boot` does not care. The v2 dtb section is dropped.
Nothing here touches a phone.
"""
import argparse, hashlib, struct, sys, zlib

PAGE_FIELDS = '<8s10I16s512s32s1024s'


def align(n, page):
    return (n + page - 1) // page * page


def die(message):
    sys.exit('mkbootimg_raphael_probe: ' + message)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stock', required=True, help="this phone's own Android boot.img")
    ap.add_argument('--kernel', required=True, help='shim Image.gz')
    ap.add_argument('--ramdisk', required=True)
    ap.add_argument('-o', '--output', required=True)
    args = ap.parse_args()

    stock = open(args.stock, 'rb').read()
    if stock[:8] != b'ANDROID!':
        die('stock image is not an Android boot image')
    (magic, ksize, kaddr, rsize, raddr, ssize, saddr, tags, page, version, osver,
     name, cmdline, _id, extra) = struct.unpack_from(PAGE_FIELDS, stock)
    if version > 2:
        die(f'header version {version}: raphael ABL images are v0-v2; wrong image?')
    if page != 4096:
        die(f'page size {page}, expected 4096')
    kernel = stock[page:page + ksize]
    if kernel[:2] != b'\x1f\x8b':
        die('stock kernel is not gzip (Image.gz-dtb expected)')
    d = zlib.decompressobj(31)
    image = d.decompress(kernel) + d.flush()
    dtbs = d.unused_data
    if not d.eof or dtbs[:4] != bytes.fromhex('d00dfeed'):
        die('no DTB appended after the stock gzip kernel')
    if image[0x38:0x3c] != b'ARMd':
        die('stock kernel is not an ARM64 Image')
    model = b''
    at = dtbs.find(b'Xiaomi')
    if at >= 0:
        model = dtbs[at:dtbs.find(b'\0', at)]
    print(f'stock: header v{version}, kernel {ksize} bytes, {dtbs.count(bytes.fromhex("d00dfeed"))} appended DTB(s), '
          f'os_version {osver:#x}, model {model.decode("ascii", "replace") or "?"}')

    shim = open(args.kernel, 'rb').read()
    if shim[:2] != b'\x1f\x8b':
        die('shim kernel must be gzip (Image.gz)')
    new_kernel = shim + dtbs
    ramdisk = open(args.ramdisk, 'rb').read()
    rdtbo = b''
    if version >= 1:
        rd_size, rd_off = struct.unpack_from('<IQ', stock, 1632)
        if rd_size:
            if rd_off == 0 or rd_off + rd_size > len(stock):
                second = struct.unpack_from('<I', stock, 24)[0]
                rd_off = page + align(ksize, page) + align(rsize, page) + align(second, page)
            rdtbo = stock[rd_off:rd_off + rd_size]
            if len(rdtbo) != rd_size:
                die('stock recovery_dtbo is truncated')
            print(f'stock recovery_dtbo: {rd_size} bytes kept')

    sha = hashlib.sha1()
    for blob in (new_kernel, ramdisk, b''):
        sha.update(blob); sha.update(struct.pack('<I', len(blob)))
    if version >= 1:
        sha.update(rdtbo); sha.update(struct.pack('<I', len(rdtbo)))
    if version >= 2:
        sha.update(struct.pack('<I', 0))          # dtb
    header = bytearray(page)
    struct.pack_into(PAGE_FIELDS, header, 0, magic, len(new_kernel), kaddr, len(ramdisk), raddr, 0, saddr,
                     tags, page, version, osver, name, cmdline, sha.digest().ljust(32, b'\0'), extra)
    rd_at = page + align(len(new_kernel), page) + align(len(ramdisk), page)
    if version >= 1:
        hsize = 1648 if version == 1 else 1660
        struct.pack_into('<IQI', header, 1632, len(rdtbo), rd_at if rdtbo else 0, hsize)
    if version == 2:
        dtb_addr = struct.unpack_from('<Q', stock, 1652)[0] if len(stock) >= 1660 else 0
        struct.pack_into('<IQ', header, 1648, 0, dtb_addr)

    out = bytes(header) + new_kernel + bytes(align(len(new_kernel), page) - len(new_kernel)) \
        + ramdisk + bytes(align(len(ramdisk), page) - len(ramdisk)) \
        + rdtbo + bytes(align(len(rdtbo), page) - len(rdtbo))
    open(args.output, 'wb').write(out)
    print(f'wrote {args.output}: {len(out)} bytes (shim {len(shim)} + stock DTBs {len(dtbs)})')


if __name__ == '__main__':
    main()
