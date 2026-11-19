/* Variant "sd": keeps the S140 SoftDevice that ships on Pro Micro / nice!nano boards.
   App between the SoftDevice (ends 0x26000) and the bootloader's user-data area (0xED000). */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00026000, LENGTH = 0x000ED000 - 0x00026000
  RAM   (rwx) : ORIGIN = 0x20003400, LENGTH = 0x3E000 - 0x3400  /* below: MBR/SoftDevice RAM (untouched, like TinyUSB). 0x2003E000+: crash record */
}
INCLUDE "nrf_common.ld"
