# doorman dongle firmware (nRF52840 Dongle, ai/doorman/dongle-fw.md)
# make            -> build/doorman_dongle_sd.uf2, build/doorman_dongle_nosd.uf2 and
#                    build/doorman_dongle_holyiot.hex and build/doorman_dongle_pca10059.hex (nRF Connect Programmer)
CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
TUSB = ../third_party/tinyusb
NRFX = ../third_party/nrfx
CFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -g -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter -std=gnu11 -DNRF52840_XXAA -DCFG_TUSB_MCU=OPT_MCU_NRF5X \
  -Isrc -I../common -I$(TUSB)/src -I$(NRFX) -I$(NRFX)/mdk -I$(NRFX)/hal -I$(NRFX)/drivers/include -I$(NRFX)/drivers/src -I$(NRFX)/templates \
  -I../third_party/CMSIS_5/CMSIS/Core/Include -D__STACK_SIZE=4096 -D__HEAP_SIZE=0
SRCS = src/main.c src/link.c src/usb_descriptors.c $(NRFX)/mdk/system_nrf52840.c \
  $(TUSB)/src/tusb.c $(TUSB)/src/common/tusb_fifo.c $(TUSB)/src/device/usbd.c $(TUSB)/src/device/usbd_control.c \
  $(TUSB)/src/class/hid/hid_device.c $(TUSB)/src/portable/nordic/nrf5x/dcd_nrf5x.c
LDFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Wl,--gc-sections -L$(NRFX)/mdk -specs=nano.specs -specs=nosys.specs

all: build/doorman_dongle_sd.uf2 build/doorman_dongle_nosd.uf2 build/doorman_dongle_holyiot.hex build/doorman_dongle_pca10059.hex

build/doorman_dongle_%.elf: $(SRCS) $(NRFX)/mdk/gcc_startup_nrf52840.S src/*.h ld/app_%.ld
	@mkdir -p build
	$(CC) $(CFLAGS) -DVARIANT_SD=$(if $(filter sd,$*),1,0) -DBOARD_HOLYIOT=$(if $(filter holyiot,$*),1,0) -DBOARD_PCA10059=$(if $(filter pca10059,$*),1,0) $(SRCS) $(NRFX)/mdk/gcc_startup_nrf52840.S \
	  $(LDFLAGS) -Tld/app_$*.ld -Wl,-Map=build/doorman_dongle_$*.map -o $@
	arm-none-eabi-size $@

build/%.hex: build/%.elf
	$(OBJCOPY) -O ihex $< $@
build/%.uf2: build/%.hex
	python3 tools/uf2conv.py -f 0xADA52840 -c -o $@ $< >/dev/null

clean:
	rm -rf build
.SECONDARY:
.PHONY: all clean
