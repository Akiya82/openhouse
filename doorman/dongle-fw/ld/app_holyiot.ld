/* Variant "holyiot": Holyiot 21017 with Nordic's open USB DFU bootloader. MBR at 0x0-0xFFF, app from
   0x1000; stays below 0xDA000 so the bootloader (0xE0000+) and its settings pages are never touched. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00001000, LENGTH = 0x000DA000 - 0x00001000
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x3E000  /* 0x2003E000+: crash record, see main.c */
}
INCLUDE "nrf_common.ld"
