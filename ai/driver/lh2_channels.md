# Lighthouse 2.0 channel management

The Base stations page can change a station to an unused channel over Bluetooth LE.
Enable **Automatically resolve channel conflicts** there to resolve further conflicts
during this overlay session. The overlay keeps one base on its channel, moves the
others one at a time, and waits for a fresh optical report after each write.
A write error, lost driver connection or missing confirmation stops automatic
resolution. The toggle resets when the overlay restarts.

Install the optional [ShayBox Lighthouse 2.1 C library](https://github.com/ShayBox/Lighthouse/releases/tag/2.1.0)
for your platform. For 64-bit Windows, use the `lighthouse-x86_64-pc-windows-msvc.dll`
release asset. Name it `lighthouse.dll` in `%LOCALAPPDATA%/openhouse` on Windows or
`liblighthouse.so` in `~/.local/share/openhouse` on Linux, or set
`OPENHOUSE_LIGHTHOUSE_BLE` to its path. Restart the overlay. A BLE adapter and a
station firmware that accepts BLE channel changes are required.

OpenHouse reserves channels known to the driver and changes only stations whose
optical serial matches a BLE advertisement. Bluetooth scans happen for channel
changes only. Normally the tracker has to see the station for several seconds
after the change to confirm it. If the old room was learned during a conflict,
use **Reset room** once the bases have distinct channels.
