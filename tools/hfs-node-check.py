#!/usr/bin/env python3
"""Locate one B-tree node of an HFS+/HFSX image and show how to compare it on the phone.

    tools/hfs-node-check.py IMAGE [NODE] [FILEID]

FILEID 4 = catalog (default), 3 = extents overflow. NODE default 24448 (the node XNU
reported as invalid). Prints the node's byte offset in the image, whether it is all
zero there, its sha256, and the adb command that hashes the same bytes in `darwin`.
Read-only.
"""
import hashlib
import struct
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    node = int(sys.argv[2]) if len(sys.argv) > 2 else 24448
    fileid = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    with open(path, 'rb') as f:
        f.seek(1024)
        vh = f.read(512)
        sig = vh[0:2]
        if sig not in (b'H+', b'HX'):
            sys.exit(f'not HFS+/HFSX: signature {sig!r}')
        block = struct.unpack_from('>I', vh, 0x28)[0]
        fork_off = {3: 0xc0, 4: 0x110}[fileid]
        logical, _, total = struct.unpack_from('>QII', vh, fork_off)
        extents = [struct.unpack_from('>II', vh, fork_off + 16 + 8 * i) for i in range(8)]
        extents = [(s, c) for s, c in extents if c]
        in_vh = sum(c for _, c in extents)
        # node size from the header node (node 0, BTHeaderRec at +14, nodeSize at +32)
        f.seek(extents[0][0] * block)
        hdr = f.read(512)
        node_size = struct.unpack_from('>H', hdr, 32)[0]
        root, depth = struct.unpack_from('>I', hdr, 16)[0], struct.unpack_from('>H', hdr, 14)[0]
        total_nodes = struct.unpack_from('>I', hdr, 36)[0]
        print(f'signature {sig.decode()} block {block} fileID {fileid} logical {logical} '
              f'blocks {total} (in volume header extents: {in_vh}) node size {node_size} '
              f'nodes {total_nodes} root node {root} depth {depth}')
        for i, (s, c) in enumerate(extents):
            print(f'  extent {i}: start block {s} count {c}  bytes 0x{s * block:x}..0x{(s + c) * block:x}')
        rel = node * node_size
        pos = None
        for s, c in extents:
            if rel < c * block:
                pos = s * block + rel
                break
            rel -= c * block
        if pos is None:
            print(f'node {node} lies beyond the 8 volume-header extents '
                  f'(file continues in the extents overflow file: {total - in_vh} more blocks)')
            return
        f.seek(pos)
        data = f.read(node_size)
    zero = not any(data)
    desc = data[8] if len(data) > 8 else None
    nrec = struct.unpack_from('>H', data, 10)[0]
    print(f'node {node}: image offset {pos} (0x{pos:x}) kind {desc} records {nrec} all-zero {zero}')
    print(f'image sha256 {hashlib.sha256(data).hexdigest()}')
    if pos % 4096 == 0 and node_size % 4096 == 0:
        print('phone check (TWRP):')
        print(f'  adb shell "dd if=/dev/block/by-name/darwin bs=4096 skip={pos // 4096} '
              f'count={node_size // 4096} 2>/dev/null | sha256sum"')


if __name__ == '__main__':
    main()
