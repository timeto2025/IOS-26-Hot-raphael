#!/usr/bin/env bash
# A local XNU source tree with every patch of xnu/patches-26 applied, for reading the kernel and writing the next patch
# on Linux. It is not a build: the kernel is built by CI (.github/workflows/xnu.yml, macOS runner).
#   tools/xnu-tree.sh [DIR]          default DIR: ~/raphael-xnu  -> DIR/xnu (a git tree, one commit per stage)
# Same order as CI: xnu-12377.121.6, blacktop/darwin-xnu-build's patches/26.0 (those that apply), our stage scripts,
# then xnu/patches-26/*.patch.
# New patch: edit in DIR/xnu, commit, then
#   git -C DIR/xnu format-patch -1 --stdout > xnu/patches-26/NNNN-name.patch
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=${1:-$HOME/raphael-xnu}
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=bd88697145b2d8008bbed951fef99795308262f0
ID=(-c user.name="raphael port" -c user.email=noreply@example.invalid)
[[ -e "$DIR/xnu" ]] && { echo "$DIR/xnu exists: remove it or name another directory"; exit 1; }
mkdir -p "$DIR"
git clone -q --depth 1 --branch xnu-12377.121.6 https://github.com/apple-oss-distributions/xnu.git "$DIR/xnu"
git init -q "$DIR/dxb"
git -C "$DIR/dxb" fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
git -C "$DIR/dxb" checkout -q FETCH_HEAD
cd "$DIR/xnu"
for p in "$DIR"/dxb/patches/26.0/*.patch; do
	git apply --check "$p" 2>/dev/null && git apply "$p"
done
mkdir -p "$DIR/fake-kdk"
for s in "$ROOT"/xnu/patches-26/*.sh; do
	MACHINE_CONFIG=SWEET MACOS_VERSION=26.5 KDKROOT="$DIR/fake-kdk" bash "$s" || echo "stage script $(basename "$s") failed (it may need macOS): carried on"
done
git add -A && git "${ID[@]}" commit -q -m "dxb + stage"
partial=()
for p in "$ROOT"/xnu/patches-26/*.patch; do
	# A file that only the macOS build stages (the firehose header of 0134) is missing here: the rest of such a patch
	# is applied and the patch is named at the end.
	git apply --whitespace=nowarn "$p" 2>/dev/null || { git apply --whitespace=nowarn --reject "$p" >/dev/null 2>&1 || true; partial+=("$(basename "$p")"); }
done
find . -name "*.rej" -delete
git add -A && git "${ID[@]}" commit -q -m "patches-26 applied ($(ls "$ROOT"/xnu/patches-26/*.patch | tail -1 | xargs basename))"
find . -name "*.rej" -delete
echo "tree: $DIR/xnu"
((${#partial[@]})) && echo "applied in part (expected: 0134, its firehose header): ${partial[*]}" || true
