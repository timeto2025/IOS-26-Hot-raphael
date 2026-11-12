#!/bin/bash
# raphael: put this phone's modem/Wi-Fi firmware and NV copies into the iOS root image on the PC.
#
#   tools/raphael-stage-fw.sh ROOT_IMG
#
# Copies work/fw/modem -> /private/var/local/fw/modem and work/fw/rmtfs -> /private/var/local/fw/rmtfs
# inside ROOT_IMG (mounted read-write on the PC, then fsck). Afterwards write the changed parts to the
# phone with tools/raphael-write-darwin.py ROOT_IMG (it only rewrites chunks that differ).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=${1:?usage: tools/raphael-stage-fw.sh ROOT_IMG}
[[ -f "$IMG" ]] || { echo "error: $IMG not found" >&2; exit 1; }
[[ -s "$ROOT/work/fw/modem/modem.mdt" ]] || { echo "error: run tools/raphael-extract-fw.sh first" >&2; exit 1; }
MNT=$(mktemp -d)
trap 'mountpoint -q "$MNT" && sudo umount "$MNT"; rmdir "$MNT"' EXIT
fsck.hfsplus -fy "$IMG" >/dev/null || true
sudo mount -t hfsplus -o loop,force,rw "$IMG" "$MNT"
sudo install -d -m 0755 -o 0 -g 0 "$MNT/private/var/local/fw/modem" "$MNT/private/var/local/fw/rmtfs"
sudo rsync -a --delete --chown=0:0 --chmod=D755,F644 "$ROOT/work/fw/modem/" "$MNT/private/var/local/fw/modem/"
sudo rsync -a --delete --chown=0:0 --chmod=D700,F600 "$ROOT/work/fw/rmtfs/" "$MNT/private/var/local/fw/rmtfs/"
sudo du -sh "$MNT/private/var/local/fw/modem" "$MNT/private/var/local/fw/rmtfs"
sync
sudo umount "$MNT"
fsck.hfsplus -fy "$IMG" | tail -1
echo "staged. Next (phone in TWRP): python3 tools/raphael-write-darwin.py $IMG"
