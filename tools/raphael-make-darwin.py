#!/usr/bin/env python3
"""Make the `darwin` partition (the iOS root) on a Redmi K20 Pro, cut off the end of Android's userdata.

    tools/raphael-make-darwin.py GIB                plan only: reads the GPT from the phone, writes nothing
    tools/raphael-make-darwin.py GIB --apply        save both GPT copies to ~/gpt-backup/STAMP, write, read back
    tools/raphael-make-darwin.py --format-data      after a reboot into TWRP: make Android's userdata again
    tools/raphael-make-darwin.py --restore DIR      write the saved GPT copies of DIR back
    tools/raphael-make-darwin.py --show             print the table (both copies checked), free entries, free space;
                                                    read only
    tools/raphael-make-darwin.py --save DIR         save both GPT copies to DIR, to put back with --restore later
                                                    (Windows can wreck the table when it makes a recovery partition)
    tools/raphael-make-darwin.py GIB --offline DIR  the same plan on DIR/gpt-head.bin + DIR/gpt-tail.bin, no phone;
                                                    writes DIR/gpt-head.new + DIR/gpt-tail.new

The phone is in TWRP (any TWRP for raphael with a root adb shell, `fastboot boot twrp.img` is enough).
ERASES ANDROID'S DATA: userdata gets shorter and is formatted again (--format-data).

What changes in the table, and nothing else (names, GUIDs, attributes of every other entry stay byte-identical):
  userdata  ends earlier; Android keeps at least 8 GiB
  darwin    new, in the first free entry: GIB GiB (at least 30: the root image is 28 GiB), from a 2 MiB boundary,
            partition type "Apple APFS container" (the name `darwin` is what the boot image looks for)
  recovery  moved to the last 128 MiB of the old userdata (the iOS boot image needs more than the stock 64 MiB);
            its old 64 MiB stay unused. --keep-recovery leaves it alone (then darwin reaches the old end of userdata).
Each copy of the table is written table first, header second, the backup copy before the primary one, every write
read back; a mismatch stops at once (then: --restore with the printed directory).

Written for this port. The way of carving (only userdata's end and one new entry, 2 MiB alignment, the APFS type,
both copies checked like firmware does) follows HakiOS's userdata-carve.py (MIT, the HakiOS authors).
"""
import argparse
import datetime
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

DISK = '/dev/block/sda'
BS = 4096
GIB = 2**30 // BS                 # blocks in a GiB
ALIGN = 512                       # 2 MiB in blocks
RECOVERY_BLOCKS = 128 * 2**20 // BS
MIN_DARWIN_GIB = 30
MIN_ANDROID_GIB = 8
APFS_CONTAINER = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')


def crc(b):
    return zlib.crc32(b) & 0xffffffff


# ---------------------------------------------------------------- the table, on bytes only (no phone)

def header(block, where):
    if block[:8] != b'EFI PART':
        sys.exit(f'{where}: no GPT header')
    size = struct.unpack_from('<I', block, 12)[0]
    if not 92 <= size <= BS:
        sys.exit(f'{where}: header length {size} not supported')
    h = bytearray(block[:size])
    want = struct.unpack_from('<I', h, 16)[0]
    struct.pack_into('<I', h, 16, 0)
    if crc(bytes(h)) != want:
        sys.exit(f'{where}: header CRC wrong')
    my, alt, first, last = struct.unpack_from('<QQQQ', block, 24)
    ent, count, esz, ecrc = struct.unpack_from('<QIII', block, 72)
    if esz != 128 or not 0 < count <= 128:
        sys.exit(f'{where}: {count} entries of {esz} bytes not supported')
    return dict(size=size, my=my, alt=alt, first=first, last=last, ent=ent, count=count, esz=esz, ecrc=ecrc)


def seal(block, table):
    b = bytearray(block)
    size = struct.unpack_from('<I', b, 12)[0]
    struct.pack_into('<I', b, 88, crc(table))
    struct.pack_into('<I', b, 16, 0)
    struct.pack_into('<I', b, 16, crc(bytes(b[:size])))
    return bytes(b)


def parse(head, tail, blocks):
    """both copies checked: (primary header, backup header, table bytes, entries)"""
    ph = header(head[BS:2 * BS], 'primary')
    bh = header(tail[-BS:], 'backup')
    if (ph['my'], ph['alt'], bh['my'], bh['alt']) != (1, blocks - 1, blocks - 1, 1):
        sys.exit('the two GPT headers do not point at each other / the disk end')
    if any(ph[k] != bh[k] for k in ('first', 'last', 'count', 'esz')):
        sys.exit('primary and backup headers differ')
    n = ph['count'] * ph['esz']
    tail_base = blocks - len(tail) // BS
    p = head[ph['ent'] * BS:ph['ent'] * BS + n]
    b = tail[(bh['ent'] - tail_base) * BS:(bh['ent'] - tail_base) * BS + n]
    if len(p) != n or len(b) != n:
        sys.exit('a partition table lies outside the saved window')
    if crc(p) != ph['ecrc'] or crc(b) != bh['ecrc']:
        sys.exit('partition table CRC wrong')
    if p != b:
        sys.exit('primary and backup tables differ')
    ents = []
    for i in range(ph['count']):
        e = p[i * 128:(i + 1) * 128]
        ents.append(dict(type=uuid.UUID(bytes_le=e[:16]), first=struct.unpack_from('<Q', e, 32)[0],
                         last=struct.unpack_from('<Q', e, 40)[0],
                         name=e[56:128].decode('utf-16le', 'replace').split('\0', 1)[0]))
    used = sorted((e for e in ents if e['type'].int), key=lambda e: e['first'])
    for a, c in zip(used, used[1:]):
        if a['last'] >= c['first']:
            sys.exit(f'partitions {a["name"]} and {c["name"]} overlap')
    for e in used:
        if not ph['first'] <= e['first'] <= e['last'] <= ph['last']:
            sys.exit(f'partition {e["name"]} is outside the usable range')
    return ph, bh, p, ents


def plan(head, tail, blocks, gib, move_recovery=True):
    """the new head and tail windows, and a description of the change"""
    ph, bh, table, ents = parse(head, tail, blocks)
    idx = {}
    for i, e in enumerate(ents):
        if e['type'].int:
            idx.setdefault(e['name'], []).append(i)
    if 'darwin' in idx:
        sys.exit('there is a partition named darwin already: nothing to do (to move only recovery on such a phone: '
                 'tools/raphael-move-recovery.py)')
    for name in ('userdata', 'recovery'):
        if len(idx.get(name, [])) != 1:
            sys.exit(f'not exactly one partition named {name}')
    u, r = idx['userdata'][0], idx['recovery'][0]
    free = [i for i, e in enumerate(ents) if not e['type'].int]
    if not free:
        sys.exit('no free GPT entry for darwin')
    slot = free[0]
    if gib < MIN_DARWIN_GIB:
        sys.exit(f'darwin needs at least {MIN_DARWIN_GIB} GiB (the root image is 28 GiB)')

    uf, ul = ents[u]['first'], ents[u]['last']
    end = ul + 1                                            # the old userdata ends just before this block
    rec_old = ents[r]['last'] - ents[r]['first'] + 1
    if move_recovery and rec_old >= RECOVERY_BLOCKS:
        print(f'recovery is {rec_old * BS // 2**20} MiB already: left where it is')
        move_recovery = False
    if move_recovery:
        rf = (end - RECOVERY_BLOCKS) // ALIGN * ALIGN
        rl = rf + RECOVERY_BLOCKS - 1
        dl = rf - 1
    else:
        dl = ul
    df = (dl + 1 - gib * GIB) // ALIGN * ALIGN
    if df - uf < MIN_ANDROID_GIB * GIB:
        most = ((dl + 1 - uf - MIN_ANDROID_GIB * GIB) // GIB)
        sys.exit(f'{gib} GiB leaves Android less than {MIN_ANDROID_GIB} GiB; at most {most} GiB here')

    new = bytearray(table)
    struct.pack_into('<Q', new, u * 128 + 40, df - 1)
    e = bytearray(128)
    e[:16] = APFS_CONTAINER.bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQQ', e, 32, df, dl, 0)
    e[56:56 + 12] = 'darwin'.encode('utf-16le')
    new[slot * 128:(slot + 1) * 128] = e
    if move_recovery:
        struct.pack_into('<QQ', new, r * 128 + 32, rf, rl)
    new = bytes(new)

    tail_base = blocks - len(tail) // BS
    nh, nt = bytearray(head), bytearray(tail)
    nh[ph['ent'] * BS:ph['ent'] * BS + len(new)] = new
    nh[BS:2 * BS] = seal(head[BS:2 * BS], new)
    at = (bh['ent'] - tail_base) * BS
    nt[at:at + len(new)] = new
    nt[-BS:] = seal(tail[-BS:], new)
    nh, nt = bytes(nh), bytes(nt)

    # the result checked the same way, and only the intended entries changed
    _, _, _, after = parse(nh, nt, blocks)
    changed = [i for i in range(len(ents)) if table[i * 128:(i + 1) * 128] != new[i * 128:(i + 1) * 128]]
    want = sorted({u, slot} | ({r} if move_recovery else set()))
    if changed != want or nh[:BS] != head[:BS]:
        sys.exit('the new table changes more than it should; nothing made')
    a, b = table[u * 128:(u + 1) * 128], new[u * 128:(u + 1) * 128]
    if a[:40] != b[:40] or a[48:] != b[48:]:
        sys.exit('userdata changed in more than its end; nothing made')

    lines = [f'userdata  {uf}..{ul} ({(ul - uf + 1) / GIB:.2f} GiB) -> {uf}..{df - 1} ({(df - uf) / GIB:.2f} GiB), '
             f'formatted again afterwards: Android\'s data goes',
             f'darwin    new, entry {slot + 1}: {df}..{dl} ({(dl - df + 1) / GIB:.2f} GiB)']
    if move_recovery:
        lines.append(f'recovery  {ents[r]["first"]}..{ents[r]["last"]} ({rec_old * BS // 2**20} MiB) -> {rf}..{rl} '
                     f'(128 MiB); the old place is left unused')
    return nh, nt, lines, ph, bh


# ---------------------------------------------------------------- the phone (adb, TWRP)

def adb(args, data=None, timeout=300):
    r = subprocess.run(['adb', *args], input=data, capture_output=True, timeout=timeout)
    if r.returncode != 0:
        sys.exit(f'adb {" ".join(args)[:70]} failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def sh(cmd, timeout=300):
    return adb(['shell', cmd], timeout=timeout).decode(errors='replace')


def read(lba, count):
    data = adb(['exec-out', f'dd if={DISK} bs={BS} skip={lba} count={count} 2>/dev/null'])
    if len(data) != count * BS:
        sys.exit(f'short read at block {lba}: {len(data)} bytes')
    return data


def write(lba, data):
    assert len(data) % BS == 0
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
    try:
        adb(['push', t.name, '/tmp/gpt-chunk.bin'])
        if sh('sha256sum /tmp/gpt-chunk.bin').split()[0] != hashlib.sha256(data).hexdigest():
            sys.exit(f'the chunk for block {lba} arrived damaged; nothing written there')
        sh(f'dd if=/tmp/gpt-chunk.bin of={DISK} bs={BS} seek={lba} count={len(data) // BS} conv=notrunc,fsync '
           f'2>/dev/null && sync')
    finally:
        os.unlink(t.name)
    if read(lba, len(data) // BS) != data:
        sys.exit(f'READ-BACK MISMATCH at block {lba}: run --restore with the backup directory NOW')


def in_twrp():
    if 'recovery' not in adb(['devices']).decode():
        sys.exit('the phone is not in TWRP (adb devices shows no "recovery")')


def disk_blocks():
    size = int(sh(f'blockdev --getsize64 {DISK}').strip())
    if size % BS:
        sys.exit('disk size is not a whole number of 4 KiB blocks')
    return size // BS


def load():
    """the head window (block 0 to the end of the primary table) and the tail window (backup table to the end)"""
    blocks = disk_blocks()
    first = read(0, 2)
    ph = header(first[BS:], 'primary')
    nt = (ph['count'] * ph['esz'] + BS - 1) // BS
    head = read(0, ph['ent'] + nt)
    bh = header(read(blocks - 1, 1), 'backup')
    tail = read(bh['ent'], blocks - bh['ent'])
    return head, tail, blocks


def put(head, tail, blocks, ph, bh):
    """each copy: table, then header; the backup copy first"""
    nt = (ph['count'] * ph['esz'] + BS - 1) // BS
    tail_base = blocks - len(tail) // BS
    at = bh['ent'] - tail_base
    write(bh['ent'], tail[at * BS:(at + nt) * BS])
    write(blocks - 1, tail[-BS:])
    write(ph['ent'], head[ph['ent'] * BS:(ph['ent'] + nt) * BS])
    write(1, head[BS:2 * BS])


def mounted_userdata():
    node = sh('readlink -f /dev/block/by-name/userdata').strip()
    return [l for l in sh('cat /proc/mounts').splitlines() if l.split() and l.split()[0] in (node, '/dev/block/by-name/userdata')]


def format_data():
    in_twrp()
    if not sh('readlink -f /dev/block/by-name/darwin 2>/dev/null').strip().startswith('/dev/block/sda'):
        sys.exit('no darwin partition seen by TWRP: reboot into TWRP after --apply first')
    size = int(sh('blockdev --getsize64 /dev/block/by-name/userdata').strip())
    print(f'userdata is {size / 2**30:.2f} GiB; formatting it (twrp format data), Android starts as new')
    out = sh('twrp format data', timeout=600)
    print(out.strip())
    if any(w in out.lower() for w in ('failed', 'unable', 'error')):
        sys.exit('TWRP reported a problem; see above')
    print('done. Next: write the iOS root with tools/raphael-write-darwin.py IMAGE')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('gib', nargs='?', type=int, help='size of darwin in GiB')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--keep-recovery', action='store_true')
    ap.add_argument('--offline')
    ap.add_argument('--restore')
    ap.add_argument('--format-data', action='store_true')
    ap.add_argument('--show', action='store_true')
    ap.add_argument('--save')
    a = ap.parse_args()

    if a.save:
        in_twrp()
        head, tail, blocks = load()
        parse(head, tail, blocks)
        os.makedirs(a.save, exist_ok=True)
        for n, d in (('gpt-head.bin', head), ('gpt-tail.bin', tail)):
            f = os.path.join(a.save, n)
            if os.path.exists(f):
                sys.exit(f'{f} exists already: name another directory')
            open(f, 'wb').write(d)
        print(f'saved the table of {blocks} blocks in {a.save}  (put it back: '
              f'tools/raphael-make-darwin.py --restore {a.save})')
        return

    if a.show:
        in_twrp()
        head, tail, blocks = load()
        ph, bh, _, ents = parse(head, tail, blocks)
        nt = (ph['count'] * ph['esz'] + BS - 1) // BS
        print(f'disk {blocks} blocks of {BS} ({blocks * BS / 2**30:.2f} GiB); primary table at {ph["ent"]} '
              f'({ph["count"]} entries, {nt} blocks), first usable {ph["first"]}; backup table at {bh["ent"]}, '
              f'last usable {ph["last"]}; both copies check out')
        used = sorted(((i, e) for i, e in enumerate(ents) if e['type'].int), key=lambda x: x[1]['first'])
        at = ph['first']
        for i, e in used:
            if e['first'] > at:
                print(f'   {"(free)":12s} {at:>10}..{e["first"] - 1:<10} {(e["first"] - at) * BS / 2**20:>10.1f} MiB')
            print(f'{i + 1:>2} {e["name"]:12s} {e["first"]:>10}..{e["last"]:<10} '
                  f'{(e["last"] - e["first"] + 1) * BS / 2**20:>10.1f} MiB  {e["type"]}')
            at = e['last'] + 1
        if at <= ph['last']:
            print(f'   {"(free)":12s} {at:>10}..{ph["last"]:<10} {(ph["last"] - at + 1) * BS / 2**20:>10.1f} MiB')
        free = [i + 1 for i, e in enumerate(ents) if not e['type'].int]
        print(f'free entries: {len(free)} of {ph["count"]} {free}; room for the primary table before the first '
              f'usable block: {(ph["first"] - ph["ent"]) * BS // 128} entries; blocks between the last usable '
              f'block and the backup table: {bh["ent"] - ph["last"] - 1}')
        return

    if a.format_data:
        return format_data()

    if a.restore:
        in_twrp()
        head = open(os.path.join(a.restore, 'gpt-head.bin'), 'rb').read()
        tail = open(os.path.join(a.restore, 'gpt-tail.bin'), 'rb').read()
        blocks = disk_blocks()
        ph, bh, _, _ = parse(head, tail, blocks)
        print(f'writing the saved GPT copies of {a.restore} back')
        put(head, tail, blocks, ph, bh)
        parse(*load())
        print('restored and read back; reboot into TWRP so the kernel reads the table again')
        return

    if a.gib is None:
        ap.error('give the size of darwin in GiB')

    if a.offline:
        head = open(os.path.join(a.offline, 'gpt-head.bin'), 'rb').read()
        tail = open(os.path.join(a.offline, 'gpt-tail.bin'), 'rb').read()
        blocks = header(tail[-BS:], 'backup')['my'] + 1
        nh, nt, lines, _, _ = plan(head, tail, blocks, a.gib, not a.keep_recovery)
        print('\n'.join(lines))
        open(os.path.join(a.offline, 'gpt-head.new'), 'wb').write(nh)
        open(os.path.join(a.offline, 'gpt-tail.new'), 'wb').write(nt)
        print(f'made {a.offline}/gpt-head.new and gpt-tail.new (nothing written to a phone)')
        return

    in_twrp()
    head, tail, blocks = load()
    nh, nt, lines, ph, bh = plan(head, tail, blocks, a.gib, not a.keep_recovery)
    print('\n'.join(lines))
    if not a.apply:
        print('\nplan only: nothing written. With --apply: the GPT is saved, then written (Android\'s data is lost).')
        return

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    bdir = os.path.expanduser(f'~/gpt-backup/darwin-{stamp}')
    os.makedirs(bdir)
    open(os.path.join(bdir, 'gpt-head.bin'), 'wb').write(head)
    open(os.path.join(bdir, 'gpt-tail.bin'), 'wb').write(tail)
    print(f'\noriginal GPT saved in {bdir}  (undo: tools/raphael-make-darwin.py --restore {bdir})')
    if input('type ERASE to write the new table (Android\'s data will be lost): ').strip() != 'ERASE':
        sys.exit('not confirmed; nothing written')
    if mounted_userdata():
        sh('umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true')
        if mounted_userdata():
            sys.exit('userdata is still mounted in TWRP (Mount menu: untick Data); nothing written')
    if load()[:2] != (head, tail):
        sys.exit('the GPT changed since it was read; nothing written')
    put(nh, nt, blocks, ph, bh)
    now = load()
    if now[:2] != (nh, nt):
        sys.exit(f'the table read back is not the one written: run --restore {bdir}')
    print('new GPT written and read back.')
    print('Next: adb reboot recovery  (TWRP again), then  tools/raphael-make-darwin.py --format-data')


if __name__ == '__main__':
    main()
