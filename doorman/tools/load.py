#!/usr/bin/env python3
"""Load tests: make a few devices look like a busy eight-device dongle.

  load.py dongle MASK KBPS     the dongle queues filler at KBPS kB/s on each slot in MASK (0xFF = all 8);
                               "load.py dongle 0 0" stops it. Loads the dongle's queues, USB and the host
  load.py radio SLOT KBPS      the device in SLOT sends KBPS kB/s of filler over the air (0 stops it).
                               Loads the radio cycle too. Not kept across the device's reboot
  load.py stats [SECS]         per slot over SECS (default 5): polls, loss, data, drops, test load, USB queue peak

Filler frames are type 'Z': the dongle and the host carry them like data, the driver skips them.
Runs next to the driver (its commands use seq 128..255).
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from doorman_link import Dongle

def cmd(d, c, a=b''):
    for _ in range(10):
        try: return d.cmd(c, a, 200)
        except TimeoutError: pass
    sys.exit(f'no reply to {c:#x}')

def snap(d):
    out = {}
    for k in range(8):
        s = struct.unpack_from('<15I', cmd(d, 0x23, bytes([k]))[1])
        c = cmd(d, 0x28, bytes([k]))[1]
        out[k] = (s, struct.unpack_from('<H', c, 20)[0], struct.unpack_from('<I', c, 22)[0])
    return out

def stats(d, secs):
    used = cmd(d, 0x25, b'\x00')[1], cmd(d, 0x25, b'\x06')[1]
    a = snap(d); time.sleep(secs); b = snap(d)
    print(f'{"slot":>4} {"polls/s":>7} {"loss%":>6} {"data kB/s":>9} {"drops/s":>8} {"load kB/s":>9} {"USB queue peak":>14}')
    for k in range(8):
        sa, sb = a[k][0], b[k][0]
        polls, ok = sb[0] - sa[0], sb[1] - sa[1]
        data = (sb[6] - sa[6]) / secs / 1000
        drops = (sb[7] - sa[7]) / secs
        load = (b[k][2] - a[k][2]) / secs / 1000
        paired = (used[0] if k < 6 else used[1])[(k % 6) * 10]
        if not polls and not load and not drops: continue
        print(f'{k:>4} {polls / secs:>7.0f} {100 * (1 - ok / polls) if polls else 0:>6.1f} {data:>9.1f} {drops:>8.1f} {load:>9.1f} {b[k][1]:>12} B'
              + ('' if paired else '  (no device)'))

def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    with Dongle() as d:
        if a[0] == 'dongle' and len(a) == 3:
            r = cmd(d, 0x2B, bytes([int(a[1], 0), int(a[2])]))[1]
            print(f'dongle test load: mask {r[0]:#04x}, {r[1]} kB/s per slot')
        elif a[0] == 'radio' and len(a) == 3:
            from air_update import Air
            r = Air(d, int(a[1])).req('L', bytes([int(a[2])]))
            print(f'slot {a[1]}: radio test load {r[2]} kB/s')
        elif a[0] == 'stats':
            stats(d, float(a[1]) if len(a) > 1 else 5)
        else:
            sys.exit(__doc__)

if __name__ == '__main__':
    main()
