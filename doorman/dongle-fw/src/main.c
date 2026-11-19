// openhouse nRF52840 dongle, v0: USB command channel, self-update via the UF2 bootloader,
// crash safety, and raw 2 Mbit radio test commands. Same code for both flash variants.
#include <string.h>
#include "tusb.h"
#include "dongle.h"
#include "ohlink.h"
#include "link.h"

#ifndef VARIANT_SD
#error "build with -DVARIANT_SD=0 or 1"
#endif
#ifndef USE_WDT
#define USE_WDT 0   /* off by default: a running WDT also keeps running inside the bootloader */
#endif

extern uint32_t __isr_vector[];
#define LED_PIN 15   /* P0.15: Pro Micro / nice!nano LED, active high */

/* ---------- crash record (survives soft resets; outside the linker's RAM region) ---------- */
typedef struct {
    uint32_t magic, type, pc, lr, psr, cfsr, hfsr, bfar, count, sum;
} crash_rec_t;
#define CRASH_MAGIC 0x43525348u   /* "CRSH" */
#define crash ((volatile crash_rec_t *)0x2003E000)
#define QUICK_CRASH_LIMIT 3       /* this many crashes in a row (each < 10 s uptime) -> UF2 bootloader */

static uint32_t crash_sum(void) {
    uint32_t s = 0x5A5A5A5A;
    const volatile uint32_t *p = &crash->magic;
    for (int i = 0; i < 9; i++) s = (s << 5 | s >> 27) ^ p[i];
    return s;
}
static int crash_valid(void) { return crash->magic == CRASH_MAGIC && crash->sum == crash_sum(); }

static void enter_bootloader(uint8_t magic) {
    __disable_irq();
    NRF_POWER->GPREGRET = magic;
    NVIC_SystemReset();
}

void crash_entry(uint32_t *frame, uint32_t type) {
    uint32_t n = crash_valid() ? crash->count : 0;
    crash->magic = CRASH_MAGIC; crash->type = type;
    crash->pc = frame ? frame[6] : 0; crash->lr = frame ? frame[5] : 0; crash->psr = frame ? frame[7] : 0;
    crash->cfsr = SCB->CFSR; crash->hfsr = SCB->HFSR; crash->bfar = SCB->BFAR;
    crash->count = n + 1;
    crash->sum = crash_sum();
    /* quick-crash counter lives in GPREGRET2 (retained across soft resets, cleared at power-on) */
    uint32_t q = (NRF_POWER->GPREGRET2 & 0xFF) + 1;
    NRF_POWER->GPREGRET2 = q;
    if (q >= QUICK_CRASH_LIMIT) { NRF_POWER->GPREGRET2 = 0; enter_bootloader(GPREGRET_UF2_DFU); }
    __disable_irq();
    NVIC_SystemReset();
}

#define FAULT_STUB(name, code) \
    __attribute__((naked)) void name(void) { \
        __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n movs r1, %0\n b crash_entry" :: "i"(code)); }
FAULT_STUB(HardFault_Handler, 1)
FAULT_STUB(MemoryManagement_Handler, 2)
FAULT_STUB(BusFault_Handler, 3)
FAULT_STUB(UsageFault_Handler, 4)
void WDT_IRQHandler(void) { crash_entry(0, 5); }   /* runs just before a watchdog reset (~2 clocks of 32k) */

/* ---------- time ---------- */
volatile uint32_t ms_ticks;
void SysTick_Handler(void) { ms_ticks++; }
uint32_t board_millis(void) { return ms_ticks; }

/* ---------- USB power events (no SoftDevice is ever enabled, in either variant) ---------- */
extern void tusb_hal_nrf_power_event(uint32_t event);
void POWER_CLOCK_IRQHandler(void) {
    if (NRF_POWER->EVENTS_USBDETECTED) { NRF_POWER->EVENTS_USBDETECTED = 0; tusb_hal_nrf_power_event(0); }
    if (NRF_POWER->EVENTS_USBREMOVED)  { NRF_POWER->EVENTS_USBREMOVED = 0;  tusb_hal_nrf_power_event(1); }
    if (NRF_POWER->EVENTS_USBPWRRDY)   { NRF_POWER->EVENTS_USBPWRRDY = 0;   tusb_hal_nrf_power_event(2); }
}
void USBD_IRQHandler(void) { tud_int_handler(0); }

/* ---------- command handling ---------- */
static uint32_t boot_resetreas, usb_rx, usb_tx;
static uint8_t reply[64], pending_reply, pending_action, pending_scan, scan_first;

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void handle(const uint8_t *in, uint16_t len) {
    usb_rx++;
    if (len < 2) return;
    memset(reply, 0, sizeof reply);
    reply[0] = in[0]; reply[1] = in[1];
    uint8_t *d = reply + 3, st = ST_OK;
    switch (in[0]) {
    case CMD_INFO:
        memcpy(d, "OHDG", 4);
        d[4] = DONGLE_FW_VERSION & 0xFF; d[5] = DONGLE_FW_VERSION >> 8;
        d[6] = VARIANT_SD; d[7] = USE_WDT;
        put32(d + 8, (uint32_t)__isr_vector);
        put32(d + 12, boot_resetreas);
        put32(d + 16, ms_ticks);
        put32(d + 20, NRF_FICR->DEVICEID[0]); put32(d + 24, NRF_FICR->DEVICEID[1]);
        if (crash_valid()) {
            put32(d + 28, crash->count); put32(d + 32, crash->type); put32(d + 36, crash->pc);
            put32(d + 40, crash->lr); put32(d + 44, crash->cfsr); put32(d + 48, crash->hfsr);
        }
        d[52] = NRF_POWER->GPREGRET2;
        put32(d + 53, *(volatile uint32_t *)0x3000 == 0x51B1E5DB);   /* SoftDevice magic still in flash? */
        break;
    case CMD_STATS: put32(d, usb_rx); put32(d + 4, usb_tx); put32(d + 8, ms_ticks); break;
    case CMD_ECHO: memcpy(d, in + 2, len - 2 > 61 ? 61 : len - 2); break;
    case CMD_DFU_UF2: case CMD_DFU_SERIAL: case CMD_RESET: case CMD_CRASH_TEST:
        pending_action = in[0]; d[0] = in[2]; break;   /* done after the reply has gone out */
    case CMD_CLEAR_CRASH: crash->magic = 0; NRF_POWER->GPREGRET2 = 0; break;
    case CMD_RADIO_RSSI_SCAN: scan_first = in[2]; pending_scan = 1; return;   /* runs between cycles */
    default: if (!link_cmd(in, d, &st)) st = ST_BAD_CMD;
    }
    reply[2] = st;
    pending_reply = 1;
}

static void do_action(uint8_t a, uint8_t arg) {
    uint32_t t = ms_ticks; while (ms_ticks - t < 20) tud_task();   /* let the reply leave */
    switch (a) {
    case CMD_DFU_UF2:    enter_bootloader(GPREGRET_UF2_DFU); break;
    case CMD_DFU_SERIAL: enter_bootloader(GPREGRET_SERIAL_DFU); break;
    case CMD_RESET:      NVIC_SystemReset(); break;
    case CMD_CRASH_TEST:
        if (arg == 1) { void (*bad)(void) = (void (*)(void))0xFFFFFFF0; bad(); }  /* HardFault */
        if (arg == 2) { __disable_irq(); for (;;) {} }                            /* hang (needs WDT) */
        break;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id, hid_report_type_t t, uint8_t *buf, uint16_t n) {
    (void)itf; (void)id; (void)t; (void)buf; (void)n; return 0;
}
void tud_hid_set_report_cb(uint8_t itf, uint8_t id, hid_report_type_t t, const uint8_t *buf, uint16_t n) {
    (void)itf; (void)t;
    if (id == 0 && n) handle(buf, n);
}

static void usb_service(void)              /* also called while the link waits inside a cycle */
{
    tud_task();
    if (pending_reply && tud_hid_n_ready(0)) { tud_hid_n_report(0, 0, reply, 64); usb_tx++; pending_reply = 0; }
    else if (tud_hid_n_ready(0) && tud_mounted()) {         /* tracker stream: [0x80][slot][n][bytes] */
        static uint8_t rep[64];
        if (link_stream_pop(rep, 0)) tud_hid_n_report(0, 0, rep, 64);
    }
    if (tud_hid_n_ready(1) && tud_mounted()) {              /* odd slots on the second interface */
        static uint8_t rep1[64];
        if (link_stream_pop(rep1, 1)) tud_hid_n_report(1, 0, rep1, 64);
    }
}

int main(void) {
    SCB->VTOR = (uint32_t)__isr_vector;     /* our own table, whatever the bootloader/MBR set */
    __DSB(); __ISB();
    __enable_irq();                         /* don't rely on the bootloader leaving PRIMASK clear */
    NRF_P0->PIN_CNF[LED_PIN] = 1; NRF_P0->OUTSET = 1u << LED_PIN;   /* LED on: main() reached */
    boot_resetreas = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = 0xFFFFFFFF;
    if (boot_resetreas & (POWER_RESETREAS_RESETPIN_Msk) || boot_resetreas == 0) NRF_POWER->GPREGRET2 = 0;
    NRF_POWER->GPREGRET = 0;

    SysTick_Config(SystemCoreClock / 1000);
    NRF_P0->PIN_CNF[LED_PIN] = 1;           /* output */
    NVIC_SetPriority(USBD_IRQn, 2);
    NVIC_SetPriority(POWER_CLOCK_IRQn, 2);

#if USE_WDT
    NRF_WDT->CONFIG = WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos;  /* pause while debugging */
    NRF_WDT->CRV = 4 * 32768;
    NRF_WDT->RREN = 1;
    NRF_WDT->INTENSET = 1; NVIC_EnableIRQ(WDT_IRQn);
    NRF_WDT->TASKS_START = 1;
#endif

    link_init();
    link_set_idle_hook(usb_service);

    tud_init(0);                            /* tusb_init() is a no-op without CFG_TUSB_RHPORT0_MODE */
    NRF_POWER->INTENSET = POWER_INTENSET_USBDETECTED_Msk | POWER_INTENSET_USBREMOVED_Msk | POWER_INTENSET_USBPWRRDY_Msk;
    NVIC_EnableIRQ(POWER_CLOCK_IRQn);
    uint32_t reg = NRF_POWER->USBREGSTATUS;  /* USB may already be up: no event for that */
    if (reg & POWER_USBREGSTATUS_VBUSDETECT_Msk) tusb_hal_nrf_power_event(0);
    if (reg & POWER_USBREGSTATUS_OUTPUTRDY_Msk) tusb_hal_nrf_power_event(2);

    for (;;) {
#if USE_WDT
        NRF_WDT->RR[0] = WDT_RR_RR_Reload;
#endif
        tud_task();
        if (ms_ticks > 10000 && NRF_POWER->GPREGRET2) NRF_POWER->GPREGRET2 = 0;   /* stable: not a quick crash */
        link_poll();                        /* one TDMA cycle; services USB through usb_service() */
        if (pending_scan) { pending_scan = 0; link_rssi_scan(scan_first, reply + 3, 60); reply[2] = ST_OK; pending_reply = 1; }
        usb_service();
        if (pending_action && !pending_reply) { uint8_t a = pending_action; pending_action = 0; do_action(a, reply[3]); }
        uint32_t period = tud_mounted() ? 1000 : 200;
        if (ms_ticks % period < period / 10) NRF_P0->OUTSET = 1u << LED_PIN; else NRF_P0->OUTCLR = 1u << LED_PIN;
    }
}
