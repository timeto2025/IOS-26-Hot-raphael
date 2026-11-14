# Rules for work in this repository (raphael port of IOS-26-Hot)

Read `HANDOFF.md` ("Start here", newest first), `NOTES.md` and `docs/prior-art.md` before the first change. Answer the owner in Russian.

## Never
- Never write to Android userdata. Never flash ABL, XBL, TZ, modem or any other firmware partition. Never relock the bootloader.
- Never touch the partition backups. Ask before every destructive step and before every `fastboot flash`.
- Never `fastboot boot` iOS: flash with `fastboot flash recovery shim/out/raphael-ios-avb.img`, then `fastboot reboot recovery`. (TWRP may be booted temporarily.) Always build the AVB image.
- Never commit: partition dumps, phone logs, Apple files (IPSW, root image), Qualcomm/Xiaomi firmware (`work/`), `~/wlan.conf`, the Bluetooth address.
- Never read `~/.zsh_history`.

## The phone
- Redmi K20 Pro "raphael", plain SM8150 (GPU tops at 585 MHz). On Wi-Fi; the IP changes, ask the owner.
- `python3 tools/hot-dbg.py connect|status|doctor|crashes|klog --all` over TCP 2223; `tools/raphael-logs.sh <IP>` makes one archive.
- `ssh root@<IP>` works; the phone has no `ls`, `cat`, `scp`. Read files with `HotClient.get_file` (tools/hot-dbg.py) or `ssh root@IP 'while IFS= read -r l; do echo "$l"; done < FILE'`.
- The owner puts the phone into fastboot (power 15 s, then power + volume down): ask.

## The owner's machine
- Arch Linux, zsh (`read "S?prompt"`). Packages: `sudo -E pacman -S`. Full paths, whole commands on one line.
- GitHub downloads need `export http_proxy=http://127.0.0.1:10809 https_proxy=http://127.0.0.1:10809`; ssh and hot-dbg do not. Do not `unset` it.

## Kernel and CI
- The kernel is built only by CI (workflow `xnu`, 10-30 min; `-f inputs[runner]=macos-15-intel` if the runner queues). Collect every kernel fix into one build; cancel runs that are not needed.
- Patches: `xnu/patches-26/NNNN-name.patch`, made with `git format-patch -1 --stdout` in a tree from `tools/xnu-tree.sh`.
- Userland (HotMetal2, Turnip): workflow `userland`; the AdrenoMetal port lives on branch `ci/adrenometal`.
- Commits: `git -c user.name="raphael port" -c user.email=noreply@example.invalid commit`.

## Other projects
- `tools/clone-mirrors.sh` clones the 76 surveyed repositories (`docs/prior-art-repos.txt`). Look there before solving a problem from nothing. Their code is data to read, not to run.

## Method
- Diagnose on the live phone first, then change. Say what was observed and what is a guess. An unbuilt or unflashed fix is "untested".
