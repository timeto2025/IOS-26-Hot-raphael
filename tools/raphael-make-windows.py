#!/usr/bin/env python3
"""Room for Windows next to iOS on a Redmi K20 Pro, without adding GPT entries (the table is full: 32 of 32).

    tools/raphael-make-windows.py [GIB]           plan only: reads the GPT from the phone (TWRP), writes nothing
    tools/raphael-make-windows.py [GIB] --apply   save both GPT copies to ~/gpt-backup/STAMP, write, read back
    tools/raphael-make-windows.py [GIB] --offline DIR   the same on DIR/gpt-head.bin + DIR/gpt-tail.bin, no phone
    tools/raphael-make-darwin.py --restore DIR    undo (writes the saved copies back)

GIB: the new size of darwin (default 32, at least 30: the iOS root image is 28 GiB).

Three entries change, nothing else (the number of entries, every other entry, both headers' layout stay):
  cust      -> esp   same place (1 GiB), type EFI System; Xiaomi's MIUI region files are gone
  userdata  -> win   same start, now reaching up to the new darwin; type Microsoft basic data. ANDROID IS GONE:
                     its data partition no longer exists (TWRP will say it cannot mount Data)
  darwin    shorter, moved to the end: it keeps its last block and starts GIB GiB earlier (2 MiB boundary).
            The iOS volume at its old start is LOST: write it again with tools/raphael-write-darwin.py IMAGE.
            The kernel's log ring (the last 4 MiB) follows the partition's end by itself.
  recovery  (the iOS boot image) is not touched.

After --apply: the running TWRP still sees the old table. Reboot to fastboot (adb reboot bootloader), boot TWRP
again (fastboot boot twrp.img), then write iOS (tools/raphael-write-darwin.py ~/ios26-root-raphael.img) and check it
boots (adb reboot recovery). esp (FAT32) and win (NTFS) are formatted when Windows is installed.
"""
import argparse
import datetime
import importlib.util
import os
import struct
import sys
import uuid

_spec = importlib.util.spec_from_file_location('gpt', os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                   'raphael-make-darwin.py'))
gpt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gpt)
BS, GIB, ALIGN = gpt.BS, gpt.GIB, gpt.ALIGN

ESP_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b')
BASIC_DATA = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')


def set_entry(table, i, *, name=None, type_=None, first=None, last=None):
    e = bytearray(table[i * 128:(i + 1) * 128])
    if type_ is not None:
        e[:16] = type_.bytes_le
    if first is not None:
        struct.pack_into('<Q', e, 32, first)
    if last is not None:
        struct.pack_into('<Q', e, 40, last)
    if name is not None:
        e[56:128] = bytes(72)
        e[56:56 + 2 * len(name)] = name.encode('utf-16le')
    table[i * 128:(i + 1) * 128] = e


def plan(head, tail, blocks, gib):
    ph, bh, table, ents = gpt.parse(head, tail, blocks)
    idx = {}
    for i, e in enumerate(ents):
        if e['type'].int:
            idx.setdefault(e['name'], []).append(i)
    for name in ('esp', 'win'):
        if name in idx:
            sys.exit(f'there is a partition named {name} already: done before? nothing made')
    for name in ('cust', 'userdata', 'darwin', 'recovery'):
        if len(idx.get(name, [])) != 1:
            sys.exit(f'not exactly one partition named {name}')
    c, u, d, r = (idx[n][0] for n in ('cust', 'userdata', 'darwin', 'recovery'))
    if ents[u]['last'] + 1 != ents[d]['first']:
        sys.exit('userdata does not end where darwin starts: this layout is not the one this tool knows')
    if ents[d]['type'] != gpt.APFS_CONTAINER:
        sys.exit('darwin does not have the type this port gives it')
    if gib < gpt.MIN_DARWIN_GIB:
        sys.exit(f'darwin needs at least {gpt.MIN_DARWIN_GIB} GiB (the root image is 28 GiB)')
    dl = ents[d]['last']
    df = (dl + 1 - gib * GIB) // ALIGN * ALIGN
    if df <= ents[d]['first']:
        sys.exit(f'darwin is not bigger than {gib} GiB now: nothing to give')
    wf, wl = ents[u]['first'], df - 1

    new = bytearray(table)
    set_entry(new, c, name='esp', type_=ESP_TYPE)
    set_entry(new, u, name='win', type_=BASIC_DATA, last=wl)
    set_entry(new, d, first=df)
    new = bytes(new)

    tail_base = blocks - len(tail) // BS
    nh, nt = bytearray(head), bytearray(tail)
    nh[ph['ent'] * BS:ph['ent'] * BS + len(new)] = new
    nh[BS:2 * BS] = gpt.seal(head[BS:2 * BS], new)
    at = (bh['ent'] - tail_base) * BS
    nt[at:at + len(new)] = new
    nt[-BS:] = gpt.seal(tail[-BS:], new)
    nh, nt = bytes(nh), bytes(nt)

    _, _, _, after = gpt.parse(nh, nt, blocks)
    changed = [i for i in range(len(ents)) if table[i * 128:(i + 1) * 128] != new[i * 128:(i + 1) * 128]]
    if changed != sorted({c, u, d}) or nh[:BS] != head[:BS]:
        sys.exit('the new table changes more than it should; nothing made')
    for i in (c, u, d):                              # unique GUIDs and attributes kept
        if table[i * 128 + 16:i * 128 + 32] != new[i * 128 + 16:i * 128 + 32] or \
                table[i * 128 + 48:i * 128 + 56] != new[i * 128 + 48:i * 128 + 56]:
            sys.exit('an entry lost its GUID or attributes; nothing made')
    if (after[r]['first'], after[r]['last']) != (ents[r]['first'], ents[r]['last']):
        sys.exit('recovery would move; nothing made')

    gb = lambda f, l: f'{(l - f + 1) / GIB:.2f} GiB'
    lines = [
        f'cust      {ents[c]["first"]}..{ents[c]["last"]} ({gb(ents[c]["first"], ents[c]["last"])}) -> esp, same place, '
        f'type EFI System',
        f'userdata  {wf}..{ents[u]["last"]} ({gb(wf, ents[u]["last"])}) -> win {wf}..{wl} ({gb(wf, wl)}), '
        f'type Microsoft basic data: Android\'s data partition is gone',
        f'darwin    {ents[d]["first"]}..{dl} ({gb(ents[d]["first"], dl)}) -> {df}..{dl} ({gb(df, dl)}): the iOS '
        f'volume is lost, write it again',
        f'recovery  {ents[r]["first"]}..{ents[r]["last"]} unchanged']
    return nh, nt, lines, ph, bh


def mounted(names):
    nodes = set()
    for n in names:
        p = gpt.sh(f'readlink -f /dev/block/by-name/{n} 2>/dev/null').strip()
        if p:
            nodes |= {p, f'/dev/block/by-name/{n}'}
    return [l for l in gpt.sh('cat /proc/mounts').splitlines() if l.split() and l.split()[0] in nodes]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('gib', nargs='?', type=int, default=32, help='new size of darwin in GiB (default 32)')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--offline')
    a = ap.parse_args()

    if a.offline:
        head = open(os.path.join(a.offline, 'gpt-head.bin'), 'rb').read()
        tail = open(os.path.join(a.offline, 'gpt-tail.bin'), 'rb').read()
        blocks = gpt.header(tail[-BS:], 'backup')['my'] + 1
        nh, nt, lines, _, _ = plan(head, tail, blocks, a.gib)
        print('\n'.join(lines))
        open(os.path.join(a.offline, 'gpt-head.new'), 'wb').write(nh)
        open(os.path.join(a.offline, 'gpt-tail.new'), 'wb').write(nt)
        print(f'made {a.offline}/gpt-head.new and gpt-tail.new (nothing written to a phone)')
        return

    gpt.in_twrp()
    head, tail, blocks = gpt.load()
    nh, nt, lines, ph, bh = plan(head, tail, blocks, a.gib)
    print('\n'.join(lines))
    if not a.apply:
        print('\nplan only: nothing written. With --apply: the GPT is saved, then written.')
        return

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    bdir = os.path.expanduser(f'~/gpt-backup/windows-{stamp}')
    os.makedirs(bdir)
    open(os.path.join(bdir, 'gpt-head.bin'), 'wb').write(head)
    open(os.path.join(bdir, 'gpt-tail.bin'), 'wb').write(tail)
    print(f'\noriginal GPT saved in {bdir}  (undo: tools/raphael-make-darwin.py --restore {bdir})')
    if input('type ERASE to write the new table (Android\'s data and the iOS volume will be lost): ').strip() != 'ERASE':
        sys.exit('not confirmed; nothing written')
    if mounted(('cust', 'userdata', 'darwin')):
        gpt.sh('umount /data 2>/dev/null; umount /sdcard 2>/dev/null; umount /cust 2>/dev/null; true')
        left = mounted(('cust', 'userdata', 'darwin'))
        if left:
            sys.exit('still mounted in TWRP (Mount menu: untick everything): ' + '; '.join(left) + '\nnothing written')
    if gpt.load()[:2] != (head, tail):
        sys.exit('the GPT changed since it was read; nothing written')
    gpt.put(nh, nt, blocks, ph, bh)
    if gpt.load()[:2] != (nh, nt):
        sys.exit(f'the table read back is not the one written: run tools/raphael-make-darwin.py --restore {bdir}')
    print('new GPT written and read back.')
    print('Next: adb reboot bootloader; fastboot boot <twrp.img>; then '
          'python3 tools/raphael-write-darwin.py ~/ios26-root-raphael.img; then adb reboot recovery (iOS).')


if __name__ == '__main__':
    main()
