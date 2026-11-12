#!/usr/bin/env python3
"""Write an iOS root image into the K20 Pro `darwin` partition from TWRP, verified.

    tools/raphael-write-darwin.py IMAGE [--start-mib N]

Same method as the HackiOS installer (tools/scripts/installer.py write()): 32 MiB chunks,
`adb push` to /tmp in TWRP, sha256 of the pushed chunk checked, `dd seek= conv=notrunc`
into darwin, sync, then sha256 of the written region read back on the phone and compared.
A chunk whose region on the phone already matches is skipped, so a rerun only writes what
differs. Writes only to /dev/block/by-name/darwin, never past the image size.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import time

NODE = '/dev/block/by-name/darwin'
BS = 4096
CHUNK = 32 * 1024 * 1024
REMOTE = '/tmp/raphael-darwin-chunk.bin'


def adb(*args, data=None, timeout=300):
    r = subprocess.run(['adb', *args], input=data, capture_output=True, timeout=timeout)
    if r.returncode != 0:
        sys.exit(f'adb {" ".join(args[:2])} failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout.decode(errors='replace')


def shell(cmd, timeout=300):
    return adb('shell', cmd, timeout=timeout).strip()


def phone_sha(offset, length):
    out = shell(f'dd if={NODE} bs={BS} skip={offset // BS} count={length // BS} 2>/dev/null | sha256sum')
    return out.split()[0] if out else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--start-mib', type=int, default=0, help='resume from this offset (MiB)')
    a = ap.parse_args()

    size = os.path.getsize(a.image)
    if size % BS:
        sys.exit('image size is not a multiple of 4096')
    if shell('getprop ro.bootmode') != 'recovery' and 'recovery' not in adb('devices'):
        sys.exit('phone is not in TWRP recovery (adb devices should say "recovery")')
    cap = int(shell(f'blockdev --getsize64 {NODE}'))
    real = shell(f'readlink -f {NODE}')
    mounts = shell('cat /proc/mounts')
    print(f'darwin = {real}, {cap} bytes; image {size} bytes')
    if size > cap:
        sys.exit('image is larger than darwin; nothing written')
    if any(line.split()[0] in (NODE, real) for line in mounts.splitlines() if line.split()):
        sys.exit('darwin is mounted in TWRP; nothing written')
    if shell(f'blockdev --getro {real}') != '0':
        sys.exit('darwin is read-only in TWRP; nothing written')

    start = a.start_mib * 1024 * 1024
    written = skipped = 0
    t0 = time.monotonic()
    with open(a.image, 'rb') as f, tempfile.TemporaryDirectory() as tmp:
        local = os.path.join(tmp, 'chunk.bin')
        f.seek(start)
        off = start
        while off < size:
            data = f.read(min(CHUNK, size - off))
            want = hashlib.sha256(data).hexdigest()
            if phone_sha(off, len(data)) == want:
                skipped += len(data)
            else:
                with open(local, 'wb') as c:
                    c.write(data)
                adb('push', local, REMOTE, timeout=600)
                if shell(f'sha256sum {REMOTE}').split()[0] != want:
                    sys.exit(f'chunk at {off} arrived corrupted in /tmp; stopped (rerun with --start-mib {off >> 20})')
                shell(f'dd if={REMOTE} of={NODE} bs={BS} seek={off // BS} count={len(data) // BS} '
                      f'conv=notrunc 2>/dev/null && sync', timeout=600)
                if phone_sha(off, len(data)) != want:
                    sys.exit(f'read-back mismatch at {off}; stopped (rerun with --start-mib {off >> 20})')
                written += len(data)
            off += len(data)
            done = off - start
            el = time.monotonic() - t0
            eta = el / done * (size - off) if done else 0
            print(f'\r{off / size * 100:5.1f}%  {off >> 20}/{size >> 20} MiB  written {written >> 20} MiB, '
                  f'already correct {skipped >> 20} MiB  ~{int(eta // 60)} min left   ', end='', flush=True)
    shell(f'rm -f {REMOTE}; sync')
    print(f'\nDONE: all {size} bytes of darwin match {a.image}')


if __name__ == '__main__':
    main()
