#!/usr/bin/env python3
"""Read or change the Qualcomm Type-C / USB role-switch settings in an offline Windows SYSTEM hive.

  winusb.py SYSTEM_HIVE                      report only
  winusb.py SYSTEM_HIVE VbusCtrlMode 2       set that value on ACPI\\QCOM0582 (writes; back up first)
"""
import struct
import sys

import hivex

HIDS = ['QCOM0582', 'QCOM057D', 'QCOMFFE1', 'QCOM05A9']


def child(h, node, name):
    for c in h.node_children(node):
        if h.node_name(c).lower() == name.lower():
            return c
    return None


def main():
    path = sys.argv[1]
    setting = sys.argv[2] if len(sys.argv) > 3 else None
    newval = int(sys.argv[3]) if len(sys.argv) > 3 else None

    h = hivex.Hivex(path, write=setting is not None)
    root = h.root()
    sel = child(h, root, 'Select')
    cur = h.value_dword(h.node_get_value(sel, 'Current')) if sel else 1
    cs = child(h, root, 'ControlSet%03d' % cur)
    print('control set: ControlSet%03d' % cur)
    enum = child(h, cs, 'Enum')
    acpi = child(h, enum, 'ACPI') if enum else None
    if acpi is None:
        sys.exit('no ACPI enumerator in the hive')

    changed = False
    for hid in HIDS:
        dev = child(h, acpi, hid)
        if dev is None:
            print('%-9s NOT enumerated (device never appeared)' % hid)
            continue
        for inst in h.node_children(dev):
            iname = h.node_name(inst)
            bits = []
            for v in h.node_values(inst):
                k = h.value_key(v)
                if k in ('ConfigFlags', 'Problem', 'Service', 'Driver', 'DeviceDesc'):
                    try:
                        bits.append('%s=%d' % (k, h.value_dword(v)))
                    except Exception:
                        bits.append('%s=%s' % (k, h.value_string(v)))
            print('%-9s %s  %s' % (hid, iname, ' '.join(bits)))
            dp = child(h, inst, 'Device Parameters')
            dc = child(h, dp, 'DeviceConfig') if dp else None
            if dc is None:
                print('           (no DeviceConfig key)')
                continue
            for v in h.node_values(dc):
                print('           DeviceConfig\\%s = %d' % (h.value_key(v), h.value_dword(v)))
            if setting and hid == 'QCOM0582':
                h.node_set_value(dc, {'key': setting, 't': 4, 'value': struct.pack('<I', newval)})
                print('           -> set %s = %d' % (setting, newval))
                changed = True
    if changed:
        h.commit(None)
        print('committed')


main()
