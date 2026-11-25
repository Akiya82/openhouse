# Testing the Doorman codec and link without hardware

`doorman/test/` has what was used to design and check the air codec, the batches and the answer sizing.
Everything runs on recordings; nothing needs a dongle or a device.

## Frames from a capture
A replay with `LH_AIR` set routes each device's IMU and LH2 data through
`src/runtime/air_experiment.hpp` before the session sees it. `LH_AIR_DUMP=FILE` writes the frames a
Doorman radio would carry (`[device][length][time, 0.1 ms][frame]`); `LH_AIR_REPORT=FILE` writes bytes
on the air per device for the experimental formats. With `LH_AIR` unset nothing changes.

    LH_AIR=base LH_AIR_DUMP=frames.bin build-linux/lhtool session CAPTURE.lhraw

The `'s'` frames are rebuilt from Watchman or wired captures (up to 59 bytes or 1 ms per frame), so
absolute byte counts are a model; comparisons between formats use the same frames.

## The tools
* `codec_roundtrip FRAMES [POLL_MS [ANSWER_BYTES]]`: every frame through today's codec (the size
  reference) and through batches as the radio builds them, decoded as the dongle does, compared byte
  for byte. Also checks the table CRC-8 against the bitwise one. ctest `doorman_codec` runs it on
  `tests/data/captures/lh2-five-devices` at 8-device settings.
* `emu/`: the radio's batch path built for the Index-family radio's Cortex-M4 with the radio's flags,
  run in Unicorn on real frames (`run_emu.py`), checked against the host build (`batch_ref`). It reports
  instructions per frame and the longest run between two `DMC_YIELD`s, as microseconds at 64 MHz for CPI
  1.25 and 2.0 (Unicorn counts instructions, not cycles). Between two yields nothing may run past the
  answer window, ~180 us. Needs `arm-none-eabi-gcc` and `pip install unicorn`.
* `cycle_sim`: a model of the polling cycle under load, fixed answer sizes against `sizing.h`.
* `run.sh CAPTURE [LHTOOL] [PYTHON]` does all of it on one capture.

## What it showed (two LH2 captures, 4 bases)
* Batches with LH2 prediction: all traffic -31 to -35 % at 8 devices (176 B answers, ~10 ms cycle),
  every frame rebuilt exactly. A tracker ~8.3 -> ~5.5 kB/s.
* Radio cost: 30-57 us per frame typically, 1-2 % of the radio's CPU, at most 56 us between yields
  (after fixing a sort, the CRC and a slot clear that ran 120-200 us without yielding).
* Large answers while saturated: drops at 16 kB/s per device 10 -> 3 %, none of the cost below ~13 kB/s.
* The radio's other paths (updater, boot) are covered by `doorman/tools/radio_emu.py`. The radio
  exchange itself (POLL/RESP timing, the full main loop) isn't emulated: hardware tests cover it.
