/* nRF51 startup for an app at 0x400 under HTC's 1 KB radio bootloader. The vector table keeps HTC's
 * header layout in unused Cortex-M0 slots: +0x10 image length (patched by tools/radio_image.py),
 * +0x14 0xFFFFFFFF (the staging commit marker word: must stay blank in the image), +0x18 version. */
#include <stdint.h>
#include "nrf.h"
#include "ohlink.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
extern int main(void);
extern void SystemInit(void);

void Reset_Handler(void);
static void Default_Handler(void) { for (;;) {} }   /* unused: no interrupts are enabled; WDT resets */

#ifdef OHL_IDX
/* Valve nRF52 radio: Cortex-M4 (slots 4-6 are fault handlers), header in reserved slots 7-10 like the
 * stock image: +0x1C length (patched), +0x20 blank commit marker slot, +0x24 version. */
void SystemInit(void) {}            /* no Nordic system_nrf52.c: it can write UICR and reset */
__attribute__((section(".vectors"), used))
const uint32_t vectors[64] = {
  (uint32_t)&_estack, (uint32_t)Reset_Handler,
  [2 ... 6] = (uint32_t)Default_Handler,
  0x00000000u,  /* +0x1C image length, patched after linking */
  0xFFFFFFFFu,  /* +0x20 blank: commit marker slot */
  OHL_VERSION,  /* +0x24 version */
  0,
  [11 ... 63] = (uint32_t)Default_Handler,
};
#else
__attribute__((section(".vectors"), used))
const uint32_t vectors[48] = {
  (uint32_t)&_estack, (uint32_t)Reset_Handler, (uint32_t)Default_Handler, (uint32_t)Default_Handler,
  0x00000000u,  /* +0x10 image length, patched after linking */
  0xFFFFFFFFu,  /* +0x14 blank: commit marker slot */
  OHL_VERSION,  /* +0x18 version */
  0,
  [8 ... 47] = (uint32_t)Default_Handler,
};
#endif

__attribute__((naked, noreturn)) void Reset_Handler(void)
{
  __asm volatile("ldr r0, =_estack\n msr msp, r0");     /* don't trust whatever SP the bootloader left */
  uint32_t *s = &_sidata, *d = &_sdata;
  while (d < &_edata) *d++ = *s++;
  for (d = &_sbss; d < &_ebss; ) *d++ = 0;
  SystemInit();
  main();
  for (;;) {}
}
