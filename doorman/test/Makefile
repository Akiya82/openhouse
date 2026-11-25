# Doorman codec and link tests (ai/doorman/testing.md). Host tools build with cc; the chip timing needs
# arm-none-eabi-gcc and Python's unicorn. run.sh runs everything on a capture.
#   make            host tools: codec_roundtrip, cycle_sim, emu/batch_ref
#   make emu        the radio's batch path for the Cortex-M4 (build/radio_batch.elf / .bin)
B = build
COMMON = ../common
CC ?= cc
HOSTFLAGS = -O2 -Wall -Wextra -std=gnu11 -I$(COMMON) -I../dongle-fw/src
ARMFLAGS = -g -mcpu=cortex-m4 -mthumb -Os -ffreestanding -ffunction-sections -fdata-sections -std=gnu11 -I$(COMMON) \
  -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-e,init -Wl,-u,push -Wl,-u,close_b -T emu/emu.ld
HDRS = $(COMMON)/doorman_codec.h $(COMMON)/doorman_lh2p.h $(COMMON)/doorman_link.h ../dongle-fw/src/sizing.h

all: $(B)/codec_roundtrip $(B)/cycle_sim $(B)/batch_ref
emu: $(B)/radio_batch.bin

$(B)/%: %.c $(HDRS)
	@mkdir -p $(B)
	$(CC) $(HOSTFLAGS) $< -o $@
$(B)/batch_ref: emu/batch_ref.c $(HDRS)
	@mkdir -p $(B)
	$(CC) $(HOSTFLAGS) $< -o $@
$(B)/radio_batch.elf: emu/radio_batch.c emu/emu.ld $(HDRS)
	@mkdir -p $(B)
	arm-none-eabi-gcc $(ARMFLAGS) $< -lgcc -o $@
$(B)/radio_batch.bin: $(B)/radio_batch.elf
	arm-none-eabi-objcopy -O binary -j .text $< $@
clean:
	rm -rf $(B)
.PHONY: all emu clean
