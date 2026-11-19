# doorman radio firmware (ai/doorman/radio-fw.md).
#   make             Vive Tracker 1.0 (nRF51822)       -> build/radio.bin
#                    install: lighthouse_watchman_update -rnt build/radio.bin   (one tracker on USB)
#   make TARGET=idx  Valve nRF52 radio (Index family)  -> build-idx/radio.bin + build-idx/radio_idx.fw
#                    install: lighthouse_watchman_update -s <serial> --target=nrf52 build-idx/radio_idx.fw
#   DIAG=1           dev build: staging counters + SPI trace (tools/air_update.py info / trace)
MDK = ../third_party/nrfx/mdk
COMMON = -mthumb -Os -g -ffunction-sections -fdata-sections -Wall -Wextra -std=gnu11 \
  -I../common -I$(MDK) -I../third_party/CMSIS_5/CMSIS/Core/Include -ffreestanding
ifeq ($(DIAG),1)
COMMON += -DOHL_DIAG
endif
ifeq ($(TARGET),idx)
B = build-idx
CFLAGS = -mcpu=cortex-m4 $(COMMON) -DNRF52832_XXAA -DOHL_IDX
SRCS = src/main.c src/startup.c
LD = radio_idx.ld
FIX = --idx
OUT = $(B)/radio.bin $(B)/radio_idx.fw
else
B = build
CFLAGS = -mcpu=cortex-m0 $(COMMON) -DNRF51 -DNRF51822_XXAB
SRCS = src/main.c src/startup.c $(MDK)/system_nrf51.c
LD = radio.ld
FIX =
OUT = $(B)/radio.bin
endif
all: $(OUT)
$(B)/radio.elf: $(SRCS) $(LD) ../common/ohlink.h ../common/ohcodec.h
	@mkdir -p $(B)
	arm-none-eabi-gcc $(CFLAGS) $(SRCS) -nostartfiles -nostdlib -lgcc -Wl,--gc-sections -T$(LD) -Wl,-Map=$(B)/radio.map -o $@ -lc -lgcc
	arm-none-eabi-size $@
$(B)/radio.bin: $(B)/radio.elf
	arm-none-eabi-objcopy -O binary $< $@
	python3 ../tools/radio_image.py fix $(FIX) $@
$(B)/radio_idx.fw: $(B)/radio.bin
	python3 ../tools/radio_image.py container $< $@
clean:
	rm -rf build build-idx
