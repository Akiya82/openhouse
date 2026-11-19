#!/usr/bin/env python3
"""Host tool for the doorman dongle (nRF52840, doorman/dongle-fw).

  dongle.py info | stats | echo TEXT | reset | crash-clear
  dongle.py bootinfo                 read INFO_UF2.TXT from a mounted UF2 drive (no flashing)
  dongle.py flash sd|nosd|FILE.uf2   app -> UF2 bootloader -> copy -> wait for the app to return
  dongle.py scan                     RSSI per 1 MHz channel 2400..2500 MHz (pauses the link)
  dongle.py stats | link on|off | listen [SECS]   tracker link counters / frame rates
  dongle.py signal [SECS [SLOT]]     signal strength both ways + where polls get lost
  dongle.py slots | pair [SECS] | forget SLOT|all | channels | slotstats SLOT
  dongle.py crash-test 1             deliberate HardFault (tests the crash record)
Only touches 6767:0727 devices and UF2 drives with INFO_UF2.TXT; never Valve dongles (28de:*).
"""
import glob, os, shutil, struct, subprocess, sys, time
import hid

VID, PID = 0x6767, 0x0727
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "dongle-fw", "build")

def open_dev():
    for d in hid.enumerate(VID, PID):
        if d.get("product_string") == "Doorman Dongle":
            return hid.Device(path=d["path"])
    sys.exit("dongle app not found (6767:0727 'Doorman Dongle')")

_seq = 0
def cmd(h, c, args=b"", timeout=1000):
    global _seq
    _seq = (_seq + 1) & 0xFF
    h.write(bytes([0, c, _seq]) + bytes(args) + bytes(64 - 2 - len(args)))
    t = time.time()
    while time.time() - t < timeout / 1000:
        r = h.read(64, 100)
        if r and r[0] == c and r[1] == _seq:
            return r[2], bytes(r[3:])
    raise TimeoutError(f"no reply to 0x{c:02x}")

def find_uf2_drives():
    out = []
    for f in glob.glob("/run/media/*/*/INFO_UF2.TXT") + glob.glob("/media/*/*/INFO_UF2.TXT"):
        out.append(os.path.dirname(f))
    return out

def try_mount():
    # udisks automount may be off: mount any small FAT disk whose label looks like a UF2 bootloader
    try:
        lsblk = subprocess.run(["lsblk", "-rno", "NAME,LABEL,FSTYPE,MOUNTPOINT"], capture_output=True, text=True).stdout
    except FileNotFoundError:
        return
    for line in lsblk.splitlines():
        p = line.split(" ")
        if len(p) >= 3 and p[2] == "vfat" and (len(p) < 4 or not p[3]) and p[1]:
            lab = p[1].replace("\\x20", " ")
            if any(k in lab.upper() for k in ("NICENANO", "UF2", "PROMICRO", "NRF52")):
                subprocess.run(["udisksctl", "mount", "-b", "/dev/" + p[0]], capture_output=True)

def wait_drive(timeout=15):
    t = time.time()
    while time.time() - t < timeout:
        d = find_uf2_drives()
        if d: return d[0]
        try_mount(); time.sleep(0.5)
    return None

def info():
    h = open_dev(); st, d = cmd(h, 0x01)
    magic, ver, variant, wdt, base, rr, up, id0, id1 = struct.unpack_from("<4sHBBIIIII", d)
    print(f"magic {magic} fw {ver:#06x} variant {'sd' if variant else 'nosd'} wdt {wdt} app@{base:#x}")
    print(f"resetreas {rr:#x} uptime {up/1000:.1f}s deviceid {id1:08x}{id0:08x} quick-crashes {d[52]} softdevice-in-flash {d[53]}")
    cnt, typ, pc, lr, cfsr, hfsr = struct.unpack_from("<6I", d, 28)
    if cnt:
        names = {1: "HardFault", 2: "MemManage", 3: "BusFault", 4: "UsageFault", 5: "WDT"}
        print(f"last crash: {names.get(typ, typ)} pc={pc:#x} lr={lr:#x} cfsr={cfsr:#x} hfsr={hfsr:#x} (total {cnt})")

def flash(arg):
    path = arg if arg.endswith(".uf2") else os.path.join(BUILD, f"doorman_dongle_{arg}.uf2")
    if not os.path.exists(path): sys.exit(f"{path} missing: run make in doorman/dongle-fw")
    drive = (find_uf2_drives() or [None])[0]
    if not drive:
        h = open_dev(); cmd(h, 0x10); h.close()
        print("dongle -> UF2 bootloader ...")
        drive = wait_drive()
        if not drive: sys.exit("UF2 drive did not appear (double-tap reset puts it there by hand)")
    print(open(os.path.join(drive, "INFO_UF2.TXT")).read().strip())
    print(f"copy {os.path.basename(path)} -> {drive}")
    try:
        shutil.copyfile(path, os.path.join(drive, "NEW.UF2")); os.sync()
    except OSError:
        pass   # the drive disappears when the bootloader restarts into the app
    t = time.time()
    while time.time() - t < 15:
        if any(d.get("product_string") == "Doorman Dongle" for d in hid.enumerate(VID, PID)):
            time.sleep(0.3); info(); return
        time.sleep(0.3)
    sys.exit("app did not come back within 15 s (check with bootinfo / dmesg)")

def scan():
    h = open_dev(); rows = []
    for first in (0, 60):
        st, d = cmd(h, 0x22, bytes([first]), 3000); rows += list(d[:60])
    for ch in range(101):
        v = rows[ch]
        if v == 0xFF: continue
        print(f"{2400+ch} MHz  -{v:3d} dBm  " + "#" * max(0, 100 - v))

NAMES = {0x70: "p imu", 0x71: "q light", 0x51: "Q ctrl", 0x56: "V status", 0x50: "P reply", 0x52: "connected", 0x53: "disc"}

def listen(secs, slot=0):
    """Decode the tracker frame stream ([0x80][slot][n][bytes] reports -> [len][frame]...), print rates."""
    h = open_dev(); buf = bytearray(); counts = {}; nbytes = 0
    t0 = tick = time.time()
    while not secs or time.time() - t0 < secs:
        r = h.read(64, 100)
        if r and r[0] == 0x80 and r[1] == slot:
            buf = bytearray(r[3:3 + r[2]]); nbytes += r[2]        # whole frames per report
            while buf and len(buf) >= 1 + buf[0]:
                n = buf[0]; f = bytes(buf[1:1 + n]); del buf[:1 + n]
                if f: counts[f[0]] = counts.get(f[0], 0) + 1
        if time.time() - tick >= 1:
            dt = time.time() - tick; tick = time.time()
            print("  ".join(f"{NAMES.get(k, hex(k))}: {v/dt:.0f}/s" for k, v in sorted(counts.items())) + f"  | {nbytes/dt/1000:.1f} kB/s", flush=True)
            counts.clear(); nbytes = 0

SLOT_STATS = "polls resp_ok crc_err timeouts dup frames_up bytes_up drops down_frames down_resends rssi_dongle rssi_tracker tracker_missed tracker_crc ms_since_resp".split()

def slot_stats(h, slot):
    return dict(zip(SLOT_STATS, struct.unpack_from("<15I", cmd(h, 0x23, bytes([slot]))[1])))

def signal(secs, slot=0):
    """Per second: signal strength both ways and where polls are lost (one slot)."""
    h = open_dev(); prev = None; t0 = time.time()
    while not secs or time.time() - t0 < secs:
        s = slot_stats(h, slot)
        if prev:
            dp = s["polls"] - prev["polls"] or 1
            lost = (s["timeouts"] + s["crc_err"]) - (prev["timeouts"] + prev["crc_err"])
            tm = s["tracker_missed"] - prev["tracker_missed"]; tc = s["tracker_crc"] - prev["tracker_crc"]
            print(f"slot {slot}: rssi dongle<-tracker -{s['rssi_dongle']} dBm  tracker<-dongle -{s['rssi_tracker']} dBm"
                  f" | polls {dp}/s unanswered {100*lost/dp:.1f}% (crc {s['crc_err']-prev['crc_err']})"
                  f" | tracker missed {tm} (crc {tc}) | dup/out-of-order {s['dup']-prev['dup']}", flush=True)
        prev = s
        time.sleep(1)

def slots():
    st, d = cmd(open_dev(), 0x25)
    for k in range(6):
        used, act, tid = d[k*10], d[k*10+1], d[k*10+2:k*10+10]
        if used: print(f"slot {k}: tracker {tid[::-1].hex()}  {'ACTIVE' if act else 'absent'}")
    print("pairing open" if d[60] else "pairing closed")

def channels():
    st, d = cmd(open_dev(), 0x27)
    m = d[0] | d[1] << 8
    for k in range(16):
        print(f"  {2400+d[2+k]} MHz  {'on ' if m >> k & 1 else 'OFF'}  loss {d[18+k]:3d}%")
    print("join requests heard", struct.unpack_from("<I", d, 34)[0], " network base", hex(struct.unpack_from("<I", d, 38)[0]))

def main():
    a = sys.argv[1:] or ["info"]
    c = a[0]
    if c == "info": info()
    elif c == "stats":
        h = open_dev(); st, d = cmd(h, 0x02)
        print(dict(zip("usb_rx usb_tx uptime_ms".split(), struct.unpack_from("<3I", d))))
        for k in range(6):
            s = slot_stats(h, k)
            if s["polls"]: print(f"slot {k}:", s)
    elif c == "signal": signal(float(a[1]) if len(a) > 1 else 0, int(a[2]) if len(a) > 2 else 0)
    elif c == "slots": slots()
    elif c == "pair": cmd(open_dev(), 0x24, bytes([int(a[1]) if len(a) > 1 else 30])); print("pairing open")
    elif c == "forget": cmd(open_dev(), 0x26, bytes([0xFF if len(a) < 2 or a[1] == "all" else int(a[1])])); slots()
    elif c == "channels": channels()
    elif c == "codec":
        h = open_dev(); slot = int(a[1]) if len(a) > 1 else 0
        a0, b0, _ = struct.unpack_from("<3I", cmd(h, 0x28, bytes([slot]))[1]); time.sleep(3)
        a1, b1, bad = struct.unpack_from("<3I", cmd(h, 0x28, bytes([slot]))[1])
        print(f"slot {slot}: air {(a1-a0)/3/1000:.1f} kB/s for {(b1-b0)/3/1000:.1f} kB/s of frames ({100*(a1-a0)/max(1,b1-b0):.0f}%), bad frames {bad}")
    elif c == "slotstats": print(slot_stats(open_dev(), int(a[1]) if len(a) > 1 else 0))
    elif c == "link": print("link", cmd(open_dev(), 0x20, bytes([{"on": 1, "off": 0}.get(a[1] if len(a) > 1 else "", 0xFF)]))[1][0])
    elif c == "listen": listen(float(a[1]) if len(a) > 1 else 0, int(a[2]) if len(a) > 2 else 0)
    elif c == "echo": print(cmd(open_dev(), 0x03, " ".join(a[1:]).encode()[:61])[1].rstrip(b"\0"))
    elif c == "reset": cmd(open_dev(), 0x12)
    elif c == "crash-clear": cmd(open_dev(), 0x14)
    elif c == "crash-test": cmd(open_dev(), 0x13, bytes([int(a[1])]))
    elif c == "dfu": cmd(open_dev(), 0x10); print("drive:", wait_drive())
    elif c == "bootinfo":
        d = wait_drive(3)
        print(open(os.path.join(d, "INFO_UF2.TXT")).read() if d else "no UF2 drive mounted")
    elif c == "flash": flash(a[1])
    elif c == "scan": scan()
    else: print(__doc__)

if __name__ == "__main__":
    main()
