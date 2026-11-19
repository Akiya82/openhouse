#!/usr/bin/env python3
"""Package the doorman firmware (dongle + device radios) that scripts/build-doorman.sh built.

usage: package-doorman.py   (packages doorman/*/build*; run build-doorman.sh to build first)
Output: dist/openhouse-doorman/ and dist/openhouse-doorman.zip
"""
import re
import shutil
import stat
import subprocess
import zipfile
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
dm = repo / 'doorman'
ver = re.search(r'#define\s+DONGLE_FW_VERSION\s+(0x[0-9A-Fa-f]+)', (dm / 'dongle-fw/src/dongle.h').read_text()).group(1)

files = {
    'dongle/ohdongle_sd.uf2': dm / 'dongle-fw/build/ohdongle_sd.uf2',      # Nordic's SoftDevice bootloader
    'dongle/ohdongle_nosd.uf2': dm / 'dongle-fw/build/ohdongle_nosd.uf2',  # UF2 bootloader without SoftDevice
    'radio/index/radio_idx.fw': dm / 'radio-fw/build-idx/radio_idx.fw',    # Index controller family (nRF52)
    'radio/tracker1/radio.bin': dm / 'radio-fw/build/radio.bin',           # Vive Tracker 1.0 (nRF51)
}
missing = [str(p) for p in files.values() if not p.exists()]
if missing:
    raise SystemExit('not built: ' + ', '.join(missing) + ' (run scripts/build-doorman.sh)')

def radio_version(path, off):
    data = path.read_bytes()
    return int.from_bytes(data[off:off + 4], 'little')

commit = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', '--short', 'HEAD'], text=True).strip()
dirty = bool(subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain', '--', 'doorman'], text=True).strip())

out = repo / 'dist/openhouse-doorman'
shutil.rmtree(out, ignore_errors=True)
for rel, src in files.items():
    (out / rel).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, out / rel)
shutil.copy2(repo / 'LICENSE', out / 'LICENSE.txt')
lic = out / 'third_party_licenses'
lic.mkdir()
for name, path in [('tinyusb', 'tinyusb/LICENSE'), ('nrfx', 'nrfx/LICENSE'), ('CMSIS_5', 'CMSIS_5/LICENSE.txt')]:
    shutil.copy2(dm / 'third_party' / path, lic / f'{name}.txt')

idx_ver = radio_version(files['radio/index/radio_idx.fw'], 0x24)
trk_ver = radio_version(files['radio/tracker1/radio.bin'], 0x18)
(out / 'INSTALL.txt').write_text(f"""openhouse doorman firmware ({commit}{'-dirty' if dirty else ''})

Dongle (nRF52840 Dongle), firmware {ver}
  openhouse-dongle's Firmware page installs it. By hand: double-tap the dongle's reset button
  so its UF2 drive appears, then copy dongle/ohdongle_sd.uf2 onto it (ohdongle_nosd.uf2 if the
  drive's INFO_UF2.TXT shows no SoftDevice).

Device radios (USB cable; SteamVR's lighthouse_watchman_update, tools/lighthouse/bin/<platform>)
  Index controllers, radio {idx_ver:#010x}:
    lighthouse_watchman_update -s <serial> --target=nrf52 radio/index/radio_idx.fw
  Vive Tracker 1.0, radio {trk_ver:#010x} (plug in one tracker at a time):
    lighthouse_watchman_update -rnt radio/tracker1/radio.bin
  Going back to the stock radio uses the same commands with SteamVR's own radio file.
""")

archive = repo / 'dist/openhouse-doorman.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
    for path in sorted(out.rglob('*')):
        if path.is_dir():
            continue
        info = zipfile.ZipInfo.from_file(path, Path(out.name) / path.relative_to(out))
        info.external_attr = (stat.S_IFREG | 0o644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, path.read_bytes())
print(f'Packaged {archive} (dongle {ver}, Index radio {idx_ver:#010x}, Tracker 1.0 radio {trk_ver:#010x})')
