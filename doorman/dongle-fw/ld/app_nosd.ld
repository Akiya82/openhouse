/* Variant "nosd": overwrites the SoftDevice; app right after the MBR (0x0-0xFFF, never touched). */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00001000, LENGTH = 0x000ED000 - 0x00001000
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 0x3E000  /* 0x2003E000+: crash record, see main.c */
}
INCLUDE "nrf_common.ld"
