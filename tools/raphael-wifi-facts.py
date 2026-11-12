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
    if img[:8] != b'ANDROID!':
        sys.exit('not an Android boot image')
    ksize = struct.unpack_from('<I', img, 8)[0]
    page = struct.unpack_from('<I', img, 36)[0]
    kernel = img[page:page + ksize]
    d = zlib.decompressobj(31)
    d.decompress(kernel)
    rest = d.unused_data
    out = []
    while len(rest) >= 8 and rest[:4] == b'\xd0\x0d\xfe\xed':
        size = struct.unpack_from('>I', rest, 4)[0]
        out.append(rest[:size])
        rest = rest[size:]
    return out


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
