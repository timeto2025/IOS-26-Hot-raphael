#!/usr/bin/env python3
"""Give the K20 Pro a 128 MiB `recovery` at the end of the disk, cut from the unused tail of `darwin`.

    tools/raphael-move-recovery.py                plan only (reads the GPT, writes nothing)
    tools/raphael-move-recovery.py --apply        back up both GPTs to ~/gpt-backup, then write
    tools/raphael-move-recovery.py --restore DIR  write the saved original GPTs back

Run with the phone in TWRP (fastboot boot twrp-probe.img). The GPT has no free entry (32 of 32), so
no partition is added: only two entries change, and no data moves.
  darwin    12604928..29644794  ->  12604928..29611775   (still 64.9 GiB; the iOS volume uses 28 GiB)
  recovery    163840..180223    ->  29611776..29644543   (64 MiB -> 128 MiB; the old 64 MiB is left unused)
Everything else (names, types, GUIDs, attributes, the other 30 entries) stays byte-identical.
Primary and backup GPT are rewritten with fresh CRCs; the backup copy is written first.
"""
import argparse
import datetime
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

DISK = '/dev/block/sda'
BS = 4096
EXPECT = {
    'recovery': (163840, 180223),
    'darwin': (12604928, 29644794),
}
NEW = {
    'darwin': (12604928, 29611775),
    'recovery': (29611776, 29644543),
}


def adb(args, data=None, timeout=120):
    r = subprocess.run(['adb', *args], input=data, capture_output=True, timeout=timeout)
    if r.returncode != 0:
        sys.exit(f'adb {" ".join(args)[:60]} failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def read(lba, count):
    data = adb(['exec-out', f'dd if={DISK} bs={BS} skip={lba} count={count} 2>/dev/null'])
    if len(data) != count * BS:
        sys.exit(f'short read at LBA {lba}: {len(data)} bytes')
    return data


def write(lba, data):
    assert len(data) % BS == 0
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
    try:
        adb(['push', t.name, '/tmp/gpt-chunk.bin'])
        want = hashlib.sha256(data).hexdigest()
        got = adb(['shell', 'sha256sum /tmp/gpt-chunk.bin']).decode().split()[0]
        if got != want:
            sys.exit(f'chunk for LBA {lba} arrived corrupted; nothing written there')
        adb(['shell', f'dd if=/tmp/gpt-chunk.bin of={DISK} bs={BS} seek={lba} count={len(data) // BS} '
                      f'conv=notrunc 2>/dev/null && sync'])
    finally:
        os.unlink(t.name)
    if read(lba, len(data) // BS) != data:
        sys.exit(f'READ-BACK MISMATCH at LBA {lba}: run --restore with the backup directory NOW')


def parse_header(sector, where):
    if sector[:8] != b'EFI PART':
        sys.exit(f'{where}: no GPT header')
    hsize = struct.unpack_from('<I', sector, 12)[0]
    crc = struct.unpack_from('<I', sector, 16)[0]
    h = bytearray(sector[:hsize])
    h[16:20] = b'\0\0\0\0'
    if zlib.crc32(h) & 0xffffffff != crc:
        sys.exit(f'{where}: header CRC mismatch')
    my, alt, first, last = struct.unpack_from('<QQQQ', sector, 24)
    ent_lba, n, esz, ecrc = struct.unpack_from('<QIII', sector, 72)
    return dict(hsize=hsize, my=my, alt=alt, first=first, last=last, ent_lba=ent_lba, n=n, esz=esz, ecrc=ecrc)


def entries_of(blob, n, esz):
    out = []
    for i in range(n):
        e = blob[i * esz:(i + 1) * esz]
        name = e[56:128].decode('utf-16-le', errors='replace').split('\0')[0]
        first, last = struct.unpack_from('<QQ', e, 32)
        out.append((name, first, last))
    return out


def rebuild_header(sector, hsize, entries_blob):
    s = bytearray(sector)
    struct.pack_into('<I', s, 88, zlib.crc32(entries_blob) & 0xffffffff)
    struct.pack_into('<I', s, 16, 0)
    struct.pack_into('<I', s, 16, zlib.crc32(bytes(s[:hsize])) & 0xffffffff)
    return bytes(s)


def load():
    head = read(0, 6)
    ph = parse_header(head[BS:2 * BS], 'primary')
    nsec = (ph['n'] * ph['esz'] + BS - 1) // BS
    pent = head[ph['ent_lba'] * BS:(ph['ent_lba'] + nsec) * BS] if ph['ent_lba'] + nsec <= 6 else read(ph['ent_lba'], nsec)
    span = ph['n'] * ph['esz']
    if zlib.crc32(pent[:span]) & 0xffffffff != ph['ecrc']:
        sys.exit('primary entries CRC mismatch')
    bsec = read(ph['alt'], 1)
    bh = parse_header(bsec, 'backup')
    bent = read(bh['ent_lba'], nsec)
    if zlib.crc32(bent[:span]) & 0xffffffff != bh['ecrc']:
        sys.exit('backup entries CRC mismatch')
    if bent[:span] != pent[:span] or bh['alt'] != 1 or bh['my'] != ph['alt']:
        sys.exit('primary and backup GPT differ; stopping')
    return head, ph, pent, bsec, bh, bent, nsec, span


def plan(ph, pent, span):
    ents = entries_of(pent, ph['n'], ph['esz'])
    idx = {}
    for i, (name, first, last) in enumerate(ents):
        if name in EXPECT:
            if name in idx:
                sys.exit(f'two partitions named {name}; stopping')
            idx[name] = i
    for name, rng in EXPECT.items():
        if name not in idx:
            sys.exit(f'no partition {name}')
        got = ents[idx[name]][1:]
        if got != rng:
            if got == NEW[name]:
                sys.exit(f'{name} is already at its new place {got}: nothing to do')
            sys.exit(f'{name} is at {got}, expected {rng}; this tool only handles the known layout')
    if NEW['recovery'][1] > ph['last']:
        sys.exit('new recovery would end past the last usable sector')
    for i, (name, first, last) in enumerate(ents):
        if i in idx.values() or (first == 0 and last == 0):
            continue
        a, b = NEW['recovery']
        if not (last < a or first > b):
            sys.exit(f'new recovery would overlap {name}')
    new = bytearray(pent)
    for name, (first, last) in NEW.items():
        struct.pack_into('<QQ', new, idx[name] * ph['esz'] + 32, first, last)
    return idx, bytes(new)


def check_ios_volume():
    first = EXPECT['darwin'][0]
    sec = read(first, 1)
    vh = sec[1024:1536]
    if vh[:2] in (b'H+', b'HX'):
        bsz, total = struct.unpack_from('>II', vh, 0x28)
        need = bsz * total
        have = (NEW['darwin'][1] - NEW['darwin'][0] + 1) * BS
        print(f'iOS volume in darwin: {need / 2**30:.2f} GiB; new darwin {have / 2**30:.2f} GiB')
        if need > have:
            sys.exit('the iOS volume would not fit in the shrunk darwin; stopping')
    else:
        print('note: no HFS volume header at the start of darwin')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--restore')
    a = ap.parse_args()
    if 'recovery' not in adb(['devices']).decode():
        sys.exit('phone is not in TWRP recovery')

    if a.restore:
        head = open(os.path.join(a.restore, 'primary-lba0-5.bin'), 'rb').read()
        tail = open(os.path.join(a.restore, 'backup-tail.bin'), 'rb').read()
        tail_lba = int(open(os.path.join(a.restore, 'backup-tail.lba')).read())
        parse_header(head[BS:2 * BS], 'saved primary')
        print(f'restoring backup GPT at LBA {tail_lba} and primary at LBA 0')
        write(tail_lba, tail)
        write(0, head)
        print('restored; reboot to bootloader')
        return

    head, ph, pent, bsec, bh, bent, nsec, span = load()
    idx, new_ent = plan(ph, pent, span)
    check_ios_volume()
    for name in ('darwin', 'recovery'):
        o, n = EXPECT[name], NEW[name]
        print(f'  {name:9s} {o[0]}..{o[1]} ({(o[1] - o[0] + 1) * BS / 2**20:.0f} MiB) -> '
              f'{n[0]}..{n[1]} ({(n[1] - n[0] + 1) * BS / 2**20:.0f} MiB)')
    new_ph = rebuild_header(head[BS:2 * BS], ph['hsize'], new_ent[:span])
    new_bh = rebuild_header(bsec, bh['hsize'], new_ent[:span])
    parse_header(new_ph, 'new primary')
    parse_header(new_bh, 'new backup')
    if not a.apply:
        print('plan only: nothing written. Run again with --apply to back up and write.')
        return

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    bdir = os.path.expanduser(f'~/gpt-backup/{stamp}')
    os.makedirs(bdir)
    tail_lba = bh['ent_lba']
    tail = read(tail_lba, ph['alt'] - tail_lba + 1)
    open(os.path.join(bdir, 'primary-lba0-5.bin'), 'wb').write(head)
    open(os.path.join(bdir, 'backup-tail.bin'), 'wb').write(tail)
    open(os.path.join(bdir, 'backup-tail.lba'), 'w').write(str(tail_lba))
    print(f'original GPTs saved in {bdir}  (undo: tools/raphael-move-recovery.py --restore {bdir})')

    # backup copy first: if anything stops halfway, the primary is still the original
    write(bh['ent_lba'], new_ent[:nsec * BS])
    write(ph['alt'], new_bh + bsec[len(new_bh):])
    new_head = bytearray(head)
    new_head[BS:2 * BS] = new_ph + head[BS + len(new_ph):2 * BS]
    new_head[ph['ent_lba'] * BS:(ph['ent_lba'] + nsec) * BS] = new_ent[:nsec * BS]
    write(0, bytes(new_head))
    load()
    print('DONE: GPT updated and verified. Next: adb reboot bootloader, then')
    print('  fastboot getvar partition-size:recovery   (expect 0x8000000)')


if __name__ == '__main__':
    main()
