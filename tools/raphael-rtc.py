#!/usr/bin/env python3
"""raphael: calibrate the phone's clock once: sweetrtcoff = real time - PM8150 RTC.

  python3 tools/raphael-rtc.py [PHONE_IP]      (default: the host saved by tools/hot-dbg.py connect)

The PM8150 RTC counts seconds from whenever it was last set and cannot be written (Android keeps an
offset too), so the kernel's time of day is RTC + boot-arg sweetrtcoff. raphael-ios-boot.sh used to
pass the PC's Unix time as that offset, which put the phone's clock ahead by the RTC's own count.
This reads the RTC value the kernel logged at boot ("SWEET RTC: PM8150 RTC N s") and the uptime of
that line, adds the uptime since, and writes real time - RTC to work/rtc-offset, which
raphael-ios-boot.sh then passes. The RTC is battery-backed: calibrate once, again only after the
battery ran completely flat.
"""
import importlib.util
import os
import re
import sys
import time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
spec = importlib.util.spec_from_file_location('hot_dbg', os.path.join(ROOT, 'tools', 'hot-dbg.py'))
hd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hd)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else None
    if host is None:
        saved = os.path.join(ROOT, 'work', '.hot-host')
        if not os.path.exists(saved):
            sys.exit('usage: tools/raphael-rtc.py PHONE_IP (or run tools/hot-dbg.py connect IP first)')
        host = open(saved).read().strip()
    client = hd.HotClient(host) if hasattr(hd, 'HotClient') else None
    if client is None:
        cls = [getattr(hd, n) for n in dir(hd) if n.endswith('Client')]
        if not cls:
            sys.exit('tools/hot-dbg.py has no client class')
        client = cls[0](host)
    d = client.diag()
    total = d.get('system', {}).get('klog_bytes_total', 0)
    cursor, text = 0, b''
    while cursor < total:
        cursor_new, raw = client.klog(cursor, 131072)
        if cursor_new == cursor or not raw:
            break
        text += raw
        cursor = cursor_new
    m = None
    for m in re.finditer(rb'\[\s*([0-9.]+)\]\s*SWEET RTC: PM8150 RTC (\d+) s', text):
        pass
    if m is None:
        sys.exit('no "SWEET RTC: PM8150 RTC" line in the kernel log (ring wrapped? reboot and run again)')
    t_line, rtc_boot = float(m.group(1)), int(m.group(2))
    t0 = time.time()
    up = client.diag().get('system', {}).get('uptime_ms', 0) / 1000.0
    t1 = time.time()
    rtc_now = rtc_boot + (up - t_line)
    off = round((t0 + t1) / 2 - rtc_now)
    os.makedirs(os.path.join(ROOT, 'work'), exist_ok=True)
    with open(os.path.join(ROOT, 'work', 'rtc-offset'), 'w') as f:
        f.write(f'{off}\n')
    print(f'RTC {rtc_now:.0f} s now, real time {time.ctime(t0)}: sweetrtcoff={off} -> work/rtc-offset')


if __name__ == '__main__':
    main()
