#!/usr/bin/env python3
"""PC-side reader for the openhouse dongle (1209:0001): per-tracker frame stream + link health.

    from ohlink import Dongle
    with Dongle() as d:
        for slot, f in d.frames():          # f: Imu | Light | Status | Raw
            ...

Stream reports: [0x80][slot][n][n bytes of whole [len][frame] items] (64-byte HID IN); a frame
too big for one report comes as consecutive [0x81][slot][n][raw bytes of [len][frame]]. Frames are the
tracker MCU's own radio frames, already decoded from the air codec:
  'p' IMU     acc[3] gyro[3] (s16), t (u32, 48 MHz), seq (u8)
  'q' light   n x 8 B: [sensor][b1][A u16][t u32], or sync [0xFF][b1][t u32][A u16]
  'V' status  passed through raw
CLI:  python ohlink.py [SECS]    prints decoded frames briefly, then per-slot rates + signal.
See ai/doorman/protocol.md.
"""
import struct, sys, time
from dataclasses import dataclass
import hid

VID, PID = 0x1209, 0x0001

@dataclass
class Imu:
    acc: tuple; gyro: tuple; t: int; seq: int

@dataclass
class LightPulse:
    sensor: int; b1: int; width: int; t: int; sync: bool

@dataclass
class Light:
    pulses: list

@dataclass
class Status:
    data: bytes

@dataclass
class Raw:
    kind: int; data: bytes

def parse(f: bytes):
    k = f[0]
    if k == 0x70 and len(f) == 18:                                   # 'p'
        v = struct.unpack_from("<6hIB", f, 1)
        return Imu(v[0:3], v[3:6], v[6], v[7])
    if k == 0x71 and (len(f) - 1) % 8 == 0:                          # 'q'
        ps = []
        for o in range(1, len(f), 8):
            e = f[o:o + 8]
            if e[0] == 0xFF:
                t, a = struct.unpack_from("<IH", e, 2); ps.append(LightPulse(0xFF, e[1], a, t, True))
            else:
                a, t = struct.unpack_from("<HI", e, 2); ps.append(LightPulse(e[0], e[1], a, t, False))
        return Light(ps)
    if k == 0x56: return Status(f[1:])                               # 'V'
    return Raw(k, f[1:])

class Dongle:
    def __init__(self, path=None):
        if path is None:
            devs = hid.enumerate(VID, PID)
            if not devs: raise IOError("openhouse dongle (1209:0001) not found")
            path = next((d["path"] for d in devs if d.get("interface_number", 0) in (0, -1)), devs[0]["path"])
        self.h = hid.Device(path=path)
        # interface 1 (dongle fw with two HID interfaces): stream of the odd slots
        self.h1 = None
        sib = [d["path"] for d in hid.enumerate(VID, PID) if d.get("interface_number") == 1
               and d["path"] != path and d.get("serial_number") == self.h.serial]
        if sib: self.h1 = hid.Device(path=sib[0])
        self._pending = []
        self._seq = 0
        self._frag = {}
    def __enter__(self): return self
    def __exit__(self, *a):
        self.h.close()
        if self.h1: self.h1.close()

    def cmd(self, c, args=b"", timeout=1000):
        self._seq = (self._seq + 1) & 0xFF
        seq = self._seq
        self.h.write(bytes([0, c, seq]) + bytes(args) + bytes(62 - len(args)))
        t0 = time.time()
        while time.time() - t0 < timeout / 1000:
            r = self.h.read(64, 100)
            if r and r[0] == c and r[1] == seq: return r[2], bytes(r[3:])
            if r and r[0] in (0x80, 0x81): self._pending.append(bytes(r))   # keep stream data
        raise TimeoutError(f"dongle cmd {c:#x}")

    def reports(self, timeout_ms=100):
        while self._pending: yield self._pending.pop(0)
        if self.h1:
            for _ in range(8):
                r = self.h1.read(64, 0)
                if not r: break
                yield bytes(r)
        r = self.h.read(64, timeout_ms if not self.h1 else min(timeout_ms, 2))
        if r: yield bytes(r)

    def frames(self, raw=False, timeout_ms=100):
        """Yield (slot, frame) forever (None-free; returns nothing when idle, so loop over it)."""
        while True:
            got = False
            for r in self.reports(timeout_ms):
                if r[0] == 0x81:
                    b = self._frag.setdefault(r[1], bytearray())
                    b += r[3:3 + r[2]]
                    if len(b) >= 1 + b[0]:
                        f = bytes(b[1:1 + b[0]]); del self._frag[r[1]]
                        got = True; yield r[1], (f if raw else parse(f))
                    continue
                if r[0] != 0x80: continue
                self._frag.pop(r[1], None)
                body = r[3:3 + r[2]]; i = 0
                while i < len(body) and i + 1 + body[i] <= len(body):
                    f = body[i + 1:i + 1 + body[i]]; i += 1 + body[i]
                    if f: got = True; yield r[1], (f if raw else parse(f))
            if not got: return

    def slots(self):
        _, d = self.cmd(0x25)
        return {k: dict(id=d[k*10+2:k*10+10][::-1].hex(), active=bool(d[k*10+1]))
                for k in range(6) if d[k*10]}

    STATS = "polls resp_ok crc_err timeouts dup frames_up bytes_up drops down_frames down_resends rssi_dongle rssi_tracker tracker_missed tracker_crc ms_since_resp".split()
    def slot_stats(self, slot):
        return dict(zip(self.STATS, struct.unpack_from("<15I", self.cmd(0x23, bytes([slot]))[1])))

    def pair(self, secs=30): self.cmd(0x24, bytes([secs]))
    def send(self, slot, frame: bytes):
        st, _ = self.cmd(0x21, bytes([slot, len(frame)]) + frame)
        if st: raise IOError("downlink queue full")

if __name__ == "__main__":
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
    with Dongle() as d:
        print("slots:", d.slots())
        t0 = time.time(); shown = set(); cnt = {}
        while time.time() - t0 < secs:
            for slot, f in d.frames():
                k = (slot, type(f).__name__); cnt[k] = cnt.get(k, 0) + 1
                if k not in shown: shown.add(k); print(f"slot {slot}: {f}"[:160], flush=True)
                if time.time() - t0 >= secs: break
        dt = time.time() - t0
        for (slot, name), n in sorted(cnt.items()): print(f"slot {slot} {name}: {n/dt:.0f}/s")
        for slot in d.slots():
            s = d.slot_stats(slot)
            print(f"slot {slot}: rssi -{s['rssi_dongle']}/-{s['rssi_tracker']} dBm, polls {s['polls']}, unanswered {s['timeouts']+s['crc_err']}")
