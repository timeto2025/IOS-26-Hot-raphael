#!/usr/bin/env python3
"""Dump the device-tree nodes the next raphael drivers need (read-only).

    tools/raphael-hw-facts.py IMG [IMG ...] > raphael-hw-facts.txt

IMG: any image with device trees in it: the phone's boot.img (backup/boot.img), the HackiOS
images/twrp-original.img, or a dtbo.img (Xiaomi puts most board devices in the dtbo overlays).
For every DTB/overlay: the nodes for the fuel gauge and charger (battery %, charging), the audio
amplifiers and codec (sound), haptics (vibration), the flash LED (torch), Bluetooth and the
cameras, each with its compatible, reg, interrupts, gpios and supplies, plus the parent bus.
Nothing here is secret: no serials, IMEI or calibration data live in these nodes.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fdt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location('wifi_facts', os.path.join(HERE, 'raphael-wifi-facts.py'))
wifi_facts = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wifi_facts)

GROUPS = (
    ('battery/charger', ('bq27', 'bq28', 'bq25', 'bq2597', 'fg', 'fuel', 'qg', 'qpnp-smb5', 'smb1390',
                         'smb5', 'charger', 'battery', 'ln8000', 'cp', 'usbpd', 'pd_phy', 'pm8150b')),
    ('audio', ('tfa98', 'tfa9874', 'nxp', 'cs35l', 'wcd9', 'wcd934', 'pahu', 'tavil', 'slimbus', 'swr',
               'sound', 'audio', 'q6', 'apr', 'lpass', 'aw88', 'speaker')),
    ('haptics', ('haptic', 'vibrator', 'drv26', 'aw86')),
    ('torch/flash', ('flash', 'torch', 'flash_led')),
    ('bluetooth', ('bt_wcn', 'bluetooth', 'wcn399', 'qca6', 'bt-', 'btfm')),
    ('camera', ('cam', 'cci', 'sensor', 'actuator', 'eeprom', 'ois')),
)
PROPS = ('compatible', 'reg', 'status', 'interrupts', 'interrupt-parent', 'qcom,slave-id', 'gpios',
         'pinctrl-names', 'label', 'qcom,cci-master', 'sensor-position', 'cell-index', 'clock-names')


def fmt(k, v):
    if k in ('compatible', 'status', 'pinctrl-names', 'label', 'clock-names'):
        return v.rstrip(b'\0').replace(b'\0', b' | ').decode(errors='replace')
    if len(v) % 4 == 0 and len(v) <= 64:
        return ' '.join(f'0x{x:x}' for x in wifi_facts.cells(v))
    return f'<{len(v)} bytes>'


def interesting(n):
    text = (n.path + ' ' + n.props.get('compatible', b'').decode(errors='replace')).lower()
    for group, keys in GROUPS:
        if any(k in text for k in keys):
            return group
    return None


def dump(blob, label):
    root = fdt.parse(blob)
    model = root.props.get('model', b'').rstrip(b'\0').decode(errors='replace')
    print(f'== {label}: {model}')
    seen = 0
    for n in root.walk():
        if n.path.startswith(('/reserved-memory', '/__symbols__', '/__fixups__', '/__local_fixups__', '/aliases')):
            continue
        group = interesting(n)
        if not group or not (('compatible' in n.props) or any(k.endswith('-gpio') or k.endswith('-gpios') for k in n.props)):
            continue
        seen += 1
        print(f'  [{group}] {n.path}')
        for k in PROPS:
            if k in n.props:
                print(f'      {k} = {fmt(k, n.props[k])}')
        for k, v in sorted(n.props.items()):
            if k.endswith(('-supply', '-gpio', '-gpios')) or k.startswith(('qcom,i2c', 'nxp,', 'ti,')):
                print(f'      {k} = {fmt(k, v)}')
        if n.parent is not None and 'compatible' in n.parent.props:
            print(f'      parent {n.parent.path} ({fmt("compatible", n.parent.props["compatible"])}) '
                  f'reg {fmt("reg", n.parent.props.get("reg", b""))}')
    print(f'  ({seen} nodes)')


def dtbo_entries(img):
    """Android dtbo.img: header 'd7b7ab1e', then entries (size, offset) pointing at FDT overlays."""
    if img[:4] != b'\xd7\xb7\xab\x1e':
        return []
    total, hdr, esz, count, eoff, _ = struct.unpack_from('>6I', img, 4)
    out = []
    for i in range(count):
        size, off = struct.unpack_from('>2I', img, eoff + i * esz)
        out.append(img[off:off + size])
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        img = open(path, 'rb').read()
        blobs = dtbo_entries(img) or wifi_facts.dtbs_of(img)
        print(f'#### {path}: {len(blobs)} device tree(s)')
        for i, blob in enumerate(blobs):
            try:
                dump(blob, f'{os.path.basename(path)} #{i}')
            except Exception as e:  # noqa: BLE001
                print(f'== {os.path.basename(path)} #{i}: unreadable ({e})')


if __name__ == '__main__':
    main()
