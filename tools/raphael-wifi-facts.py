#!/usr/bin/env python3
"""Print the Wi-Fi/modem facts wland needs from this phone's own device trees (read-only).

    tools/raphael-wifi-facts.py BOOT_IMG

BOOT_IMG: the HackiOS package's images/twrp-original.img or the phone's boot.img (gzip kernel with
appended DTBs). For every appended DTB: model, qcom,board-id, the reserved-memory carve-outs for the
modem / WLAN (modem_region -> boot-arg wlanmpss=PA:SIZE) and the icnss (WLAN) node.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fdt  # noqa: E402


def dtbs_of(img):
    """Every valid FDT in the image: appended to a gzip kernel, to a raw/LZ4 kernel or in a dtb section."""
    if img[:8] == b'ANDROID!':
        ksize, page = struct.unpack_from('<I', img, 8)[0], struct.unpack_from('<I', img, 36)[0]
        kernel = img[page:page + ksize]
        if kernel[:2] == b'\x1f\x8b':
            d = zlib.decompressobj(31)
            d.decompress(kernel)
            img = d.unused_data + img
    out, pos = [], 0
    while True:
        at = img.find(b'\xd0\x0d\xfe\xed', pos)
        if at < 0:
            return out
        pos = at + 4
        if at + 40 > len(img):
            continue
        size, off_struct, off_strings, _, version = struct.unpack_from('>5I', img, at + 4)
        if 16 <= version <= 17 and 256 <= size <= len(img) - at and off_struct < size and off_strings < size:
            blob = img[at:at + size]
            try:
                fdt.parse(blob)
            except Exception:
                continue
            out.append(blob)
            pos = at + size


def cells(v):
    return [struct.unpack_from('>I', v, i)[0] for i in range(0, len(v) - len(v) % 4, 4)]


def main():
    img = open(sys.argv[1], 'rb').read()
    for i, blob in enumerate(dtbs_of(img)):
        root = fdt.parse(blob)
        model = root.props.get('model', b'').rstrip(b'\0').decode(errors='replace')
        bid = cells(root.props.get('qcom,board-id', b''))
        msm = cells(root.props.get('qcom,msm-id', b''))
        print(f'== DTB {i}: {model}  board-id {[hex(x) for x in bid]}  msm-id {[hex(x) for x in msm]}')
        for n in root.walk():
            path = n.path
            low = path.lower()
            if path.startswith('/reserved-memory/') and any(k in low for k in ('modem', 'mpss', 'wlan', 'wcnss', 'pil')):
                reg = cells(n.props.get('reg', b''))
                if len(reg) >= 4:
                    pa, size = (reg[0] << 32) | reg[1], (reg[2] << 32) | reg[3]
                    extra = f'   ->  wlanmpss=0x{pa:x}:0x{size:x}' if 'modem' in low or 'mpss' in low else ''
                    print(f'  {path}: 0x{pa:x} + 0x{size:x} ({size >> 20} MiB){extra}')
                else:
                    print(f'  {path}: reg {reg}')
            if 'icnss' in low or ('wlan' in low and 'qcom,' in str(n.props.get('compatible', b''))):
                comp = n.props.get('compatible', b'').replace(b'\0', b' ').decode(errors='replace').strip()
                if comp:
                    regs = [k for k in n.props if k.endswith('-supply')]
                    print(f'  {path}: compatible "{comp}" supplies {regs}')


if __name__ == '__main__':
    main()
