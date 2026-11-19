#!/usr/bin/env python3
"""Builds openhouse's firmware and puts it in a package's firmwares/ folder, where
openhouse-installer looks for it:

  firmwares/basestation/lhbs-<date>-<commit>.bin       1.0 base stations
  firmwares/dongle/doorman-dongle-sd.uf2              doorman dongle, UF2 bootloader with a SoftDevice
  firmwares/dongle/doorman-dongle-nosd.uf2            ... without one
  firmwares/dongle/doorman-holyiot.hex                Holyiot 21017 (nRF Connect Programmer)
  firmwares/radio/doorman-radio-wm3.fw      every 2.0-era device (nRF52 radio)
  firmwares/radio/doorman-radio-neo.bin  VIVE Tracker 1.0 (nRF51 radio)

usage: stage-firmwares.py PACKAGE_DIR      (needs arm-none-eabi-gcc with newlib)
"""
import datetime
import shutil
import subprocess
import sys
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve() / 'firmwares'
shutil.rmtree(out, ignore_errors=True)
for sub in ('basestation', 'dongle', 'radio'):
    (out / sub).mkdir(parents=True)

def make(path, *args):
    subprocess.run(['make', '-s', '-C', str(repo / path), *args], check=True)

make('basestation_firmware')
image = repo / 'basestation_firmware/build/firmware.bin'
data = image.read_bytes()
if sum(int.from_bytes(data[i:i + 4], 'little') for i in range(0, 32, 4)) % 2**32:
    sys.exit('lhbs: bad vector checksum')
commit = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', '--short', 'HEAD'], text=True).strip()
shutil.copy2(image, out / 'basestation' / f'lhbs-{datetime.date.today():%Y%m%d}-{commit}.bin')

make('doorman/dongle-fw')
shutil.copy2(repo / 'doorman/dongle-fw/build/doorman_dongle_sd.uf2', out / 'dongle/doorman-dongle-sd.uf2')
shutil.copy2(repo / 'doorman/dongle-fw/build/doorman_dongle_nosd.uf2', out / 'dongle/doorman-dongle-nosd.uf2')
shutil.copy2(repo / 'doorman/dongle-fw/build/doorman_dongle_holyiot.hex', out / 'dongle/doorman-holyiot.hex')

make('doorman/radio-fw')
make('doorman/radio-fw', 'TARGET=idx')
shutil.copy2(repo / 'doorman/radio-fw/build-idx/radio_idx.fw', out / 'radio/doorman-radio-wm3.fw')
shutil.copy2(repo / 'doorman/radio-fw/build/radio.bin', out / 'radio/doorman-radio-neo.bin')
print(f'Staged firmware in {out}')
