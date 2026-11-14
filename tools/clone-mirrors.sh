#!/usr/bin/env bash
# Bare mirrors of the other iOS/XNU-on-Snapdragon projects surveyed in docs/prior-art.md (list: docs/prior-art-repos.txt).
#   tools/clone-mirrors.sh [DIR]     default DIR: ~/raphael-mirrors; a second run updates what is there.
# Each ends up as DIR/<owner>_<repo>.git; read them with e.g.
#   git -C DIR/UniXNU_UniXNU.git log --oneline -20
#   git -C DIR/UniXNU_UniXNU.git grep -n "text" HEAD -- patches
# About 2 GB in all. Their contents are other people's code: data to read, never run from here.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=${1:-$HOME/raphael-mirrors}
mkdir -p "$DIR"
ok=0 bad=0
while IFS= read -r url; do
	[[ -z "$url" || "$url" == \#* ]] && continue
	name=$(echo "$url" | sed -E 's|https://github.com/||; s|/|_|').git
	if [[ -d "$DIR/$name" ]]; then
		git -C "$DIR/$name" fetch -q --prune origin '+refs/heads/*:refs/heads/*' && ok=$((ok + 1)) || { echo "update failed: $url"; bad=$((bad + 1)); }
	else
		git clone -q --bare "$url" "$DIR/$name" && ok=$((ok + 1)) || { echo "clone failed: $url"; bad=$((bad + 1)); }
	fi
done < "$ROOT/docs/prior-art-repos.txt"
echo "$ok repositories in $DIR, $bad failed"
