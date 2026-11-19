# doorman dongle firmware (`doorman/dongle-fw`)

nRF52840 Dongle (PCA10059 or compatible), tinyusb, polled main loop (no radio interrupts). Sources:
`src/main.c` (USB commands, crash record, bootloader entry), `src/link.c` (the air link: slots,
joining/pairing, hopping, go-back-N, codec), `src/usb_descriptors.c`, `src/dongle.h` (`DONGLE_FW_VERSION`,
VID/PID 6767:0727, command numbers).

## Two builds (same code, different flash base)

| UF2 | For | App base | RAM |
|---|---|---|---|
| `doorman_dongle_sd.uf2` | dongles whose UF2 bootloader sits on Nordic's MBR/SoftDevice (the usual Adafruit-style bootloader) | 0x26000 (`ld/app_sd.ld`) | from 0x20003400 (SoftDevice RAM untouched) |
| `doorman_dongle_nosd.uf2` | UF2 bootloader without SoftDevice | 0x1000 (`ld/app_nosd.ld`) | from 0x20000000 |

Info (cmd 0x01) reports which variant is running, whether the SoftDevice magic is still in flash and
the board (byte 57: 0 UF2, 1 Holyiot 21017, 2 Nordic nRF52840 Dongle PCA10059);
doorman-dongle-manager picks the file name from that. Flash end 0xED000; the **paired table** is page 0xEC000
(magic "OHPR", never erased if the code grows into it). RAM from 0x2003E000 holds the crash record.

## Install / update

* `CMD_DFU_UF2` (0x10) sets GPREGRET and resets into the UF2 bootloader; the drive appears, the UF2 is
  copied, the bootloader starts the app. `tools/dongle.py flash sd|nosd|FILE` and doorman-dongle-manager's
  Firmware page do exactly this. By hand: double-tap reset.
* Holyiot 21017 (`doorman_dongle_holyiot.hex`, Nordic's open USB DFU bootloader): 0x10 sets GPREGRET 0xB1
  instead and the bootloader's serial port (1915:521F) appears; openhouse-installer and doorman-dongle-manager
  flash it with Nordic's serial DFU (`src/common/nordic_dfu.cpp`). Pairings live at 0xDA000 there (the
  bootloader owns 0xE0000+), the SKY66112 amplifier is switched by CTX P0.24 / CRX P0.22, TX power -4 dBm.
* PCA10059 (`doorman_dongle_pca10059.hex`): the same layout, bootloader, LED (P0.06) and update path as the
  Holyiot, no amplifier (TX +8 dBm). It runs off USB (VDDH): on first boot it sets UICR REGOUT0 to 3.0 V
  once (an unset chip gives 1.8 V GPIO, too low for the LEDs), like Nordic's board code. The bootloader
  looks the same on USB for both boards (1915:521F): the installer flashes the image of the board picked.
* 0x11 enters serial DFU instead. A brand-new PCA10059 with Nordic's stock bootloader needs a UF2 bootloader
  first (not automated).

## Robustness

* Watchdog; crash record (HardFault/MemManage/BusFault/UsageFault/WDT with pc, lr, CFSR, HFSR, count)
  survives resets and is read with cmd 0x01, cleared with 0x14; 0x13 forces one (test).
* Quick-crash counter: repeated crashes right after boot are counted (cmd 0x01 byte 52).

## Timing and capacity

See `protocol.md` for the air schedule. Per-slot overhead ~450 µs (turnaround, headers, guard), so bigger
responses win: 6 devices × 120 B = 6.3 ms cycle, ~18 kB/s each against ~8 kB/s needed on air. USB is the
other limit: one 64 B report per ms per HID interface, hence two interfaces (even/odd slots). Realistic
ceiling ≈ 8 devices per dongle (needs `OHL_SLOTS` and the USB split changed); 16–20 only with worse latency.
