# doorman protocol

Source of truth: `doorman/common/ohlink.h` (air format, constants) and
`doorman/common/ohcodec.h` (codec). The dongle side is in `dongle-fw/src/link.c` and
`main.c`, the device side in `radio-fw/src/main.c`.

## Air

* Nordic proprietary **2 Mbit**: 8-bit preamble, 4+1 byte address, 8-bit length, CRC16
  (0x11021, init 0xFFFF), no whitening.
* **The dongle leads.** Each cycle polls every paired, active slot once, all on one channel.
  Slot k is polled at `cycle_start + k * slot_us`.
* Every 20 ms there is also a JOIN beacon on channel 80 (2480 MHz), address "OHJN" / prefix
  0x5A.
* Slot address: BASE = a per-dongle network base (from the dongle's FICR), prefix
  `0xA0 + slot`. A device only hears its own polls.
* The device answers with a **hardware turnaround**: its RESP is prepared before the poll
  arrives and sent by the END→DISABLE→TXEN shortcuts (~150 µs).
* Timing adapts to the number of joined devices n (`link.c`):
  `max_resp = n<=1 ? 250 : n<=2 ? 160 : n<=4 ? 96 : 120` bytes,
  `slot_us = 450 + 4*max_resp`, `period = n*slot_us + 700 µs`.
  * 5–6 devices: 120 B responses, ~6.3 ms cycle, ~18 kB/s per device.
  * Each POLL carries the period (in 50 µs units) and the max RESP length.
* Hopping: 16 channels (2404–2479 MHz, 5 MHz apart). The channel for a cycle is the
  (cycle mod n_allowed)-th allowed channel. Each POLL announces the mask for the *next* cycle.
  * The dongle measures loss per channel over 2 s windows. A channel with more than 8 % loss
    (and at least 20 polls) is turned off for 30 s, keeping at least 8 on. In practice this
    removes Wi-Fi channels: loss went from 3.1 % to 0.7 %.
  * A device that misses polls keeps hopping on its own clock: `cycle = last + round(dt/period)`.
* Reliability:
  * Uplink: go-back-N, window 2, 3-bit seq; the POLL carries the ack.
  * Downlink: alternating bit (dseq/dack).
  * A device with no poll for 1 s goes back to joining.

### Packets
```
POLL  [01][slot][flags: ack 0-2, dseq 3, down 4][cycle][mask lo][mask hi][period/50us][max resp][frames]
RESP  [02][slot][flags: seq 0-2, dack 3][rssi -dBm][missed u8][crc_err u8][frames]
JOIN  [03][net base x4][flags: pairing 0][assign id x8][assign slot]
JREQ  [04][device id x8][flags]
frames = repeated [len][frame]
```

### Joining and pairing
* An unjoined device answers about half of the JOIN beacons with a JREQ carrying its radio
  chip's DEVICEID.
* The dongle sends the slot assignment in the next 5 beacons when the ID is already in its
  paired table (a flash page), or when pairing is open.
* Pairing target (firmware 0x0004): `pair SECS [SLOT+1]`. With a slot given, a device that was
  paired elsewhere moves there; with 0 or none given, it takes the first free slot. The target
  is saved and pairing closes.
* `forget SLOT|0xFF` removes devices from the table.
* The device starts pairing on its own pairing gesture (Tracker 1.0: hold the power button
  2 s; Index: the stock gesture). The main MCU tells the radio over SPI exactly as it does with
  the stock radio.

### Air codec (lossless, `ohcodec.h`)
* IMU `p` (18 B) is sent as `j`: a flags byte, then zigzag varint deltas for the 6 axes and the
  timestamp, plus the seq only if it isn't previous+1.
* Light `q` is sent as `k`: per record `[b0][b1][varint A][zz varint dt]`.
* Both ends update their state from the original frames; the state resets on join.
* The dongle decodes, so USB carries the original frames.
* Measured: 6.9 kB/s on air for 9.0 kB/s of frames (77 %). A device needs about 8 kB/s on air
  and 10.5 kB/s decoded.

## Dongle USB interface (VID:PID 1209:0001, "openhouse dongle")

**Two HID interfaces**, 64-byte reports, 1 ms interval (endpoints 0x01/0x81 and 0x02/0x82).
One interrupt IN endpoint carries at most 64 B per ms (~61 kB/s), which wasn't enough for 6
devices, so **interface 1 streams the odd slots**. Interface 0 streams the even slots and
carries all commands. Hosts must read both (`ohdongle.cpp` drains `dev1_` in `Pump`;
`ohlink.py` reads `h1` first).

* Commands go out on interface 0 as `[cmd][seq][args]`. Replies come back as
  `[cmd][seq][status][61 B data]`.
* Stream reports are `[0x80][slot][n][whole [len][frame] items]`. Every report stands alone,
  so readers never desync.
* A frame too big for one report (feature replies, up to 64 B) comes as consecutive
  `[0x81][slot][n][raw bytes of [len][frame]]` fragments. A `0x80` report for the same slot
  discards an unfinished one.
* Command replies can be interleaved with stream reports. Keep the stream reports, don't drop
  them.

| cmd | meaning |
|---|---|
| 0x01 | info: magic, fw version, variant (sd/nosd), WDT, app base, RESETREAS, uptime, device id, crash record, quick crashes, SoftDevice present |
| 0x02 | USB counters |
| 0x03 | echo |
| 0x10 / 0x11 | enter the UF2 / serial DFU bootloader |
| 0x12 | reset |
| 0x13 | crash test (deliberate HardFault) |
| 0x14 | clear the crash record |
| 0x20 | link on/off (0xFF = query) |
| 0x21 | downlink `[slot][len][frame]` |
| 0x22 | RSSI scan of 2400–2500 MHz (pauses the link) |
| 0x23 | per-slot stats (15 u32) |
| 0x24 | pair `[secs][slot+1]` |
| 0x25 | slots: per slot used, active, id x8. `d[60] = pairing \| (target+1) << 1` |
| 0x26 | forget slot (0xFF = all) |
| 0x27 | channels: mask, loss % per hop channel, joins, net base |
| 0x28 | codec: air bytes, frame bytes, bad frames |
| 0x29 | live: per slot `[used\|active<<1][rssi last rx][rssi last tx][min][max][channel][ms since reply u32]`. Min/max reset on read |
| 0x2A | identity: the slot's last `H` frame (16 B, zeros if none) |

## Device frames (uplink, inside RESP)

| Frame | Meaning |
|---|---|
| `p` + 17 B | one IMU sample, same as a wired IMU report slot |
| `q` + n×8 B | light records, same as the wired light report |
| `H` `[1][device_id u32][gyro range][accel range][hw_version u32][fw build u32]` | identity, on every link-up; serial = `LHR-%08X` of device_id |
| `B` `[percent \| charging << 7]` | battery, every 5 s and on link-up |
| `P` + bytes | reply to a downlink `P` |
| `0xF0` ... | the radio's own updater (`radio-fw.md`) |
| `0x52` / `0x53` | link up / down (also reported to the main MCU over SPI) |

Downlink frames (`0x21` command):
* `P [id]` with id < 0x80 reads feature report `id` (the reply is `P` + its bytes, without the
  id). `P [id][data]` writes it.
* `P [cmd][len][data]` with cmd ≥ 0x80 runs a Valve-style device command:
  * haptic 0x8F;
  * **power off 0x9F with `off!`**: `P 9f 04 'o' 'f' 'f' '!'`. Our tools power devices off
    after an update instead of rebooting them; the user turns them back on;
  * host reboot 0x95.
* `0xF0` frames: radio updater, handled by the radio itself (never reaches the main MCU).

The **sensor type** (the hardware ID's low byte, e.g. tracker 4, Index 9) decides how openhouse
decodes OOTX. A stock-LPC device doesn't put it in its config, so the host reads feature 0x05
(byte 41) over the downlink.
