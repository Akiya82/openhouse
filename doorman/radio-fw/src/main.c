/* openhouse radio firmware. Default: the Vive Tracker 1.0's nRF51822 (replaces HTC's radio app at 0x400).
 * OHL_IDX: Valve's nRF52 radio module (Index controller and relatives), same updater protocol.
 *
 * SAFETY: this image is installed by HTC's 1 KB bootloader and can only be replaced through its own
 * updater (frames Y/Z/[/!/0x20 below, same protocol and replies as HTC's app). So:
 *   - no interrupts at all (everything is polled): nothing depends on how the bootloader forwards IRQs;
 *   - the SPI slave and the updater are serviced first in every loop iteration;
 *   - the radio link starts 1 s after boot and is skipped entirely in safe mode (3 quick resets in a
 *     row, or the LPC asks for it with frame 'O' 'S'), so a link bug can't lock the updater out;
 *   - a watchdog (8 s) resets any hang; it is fed right before every deliberate reset so the
 *     bootloader always gets the full period to install an image.
 * SPI slave setup copied from HTC's app (FUN_00002de4): SPIS1, SCK P0.0, MISO P0.1, MOSI P0.2,
 * CSN P0.3, mode 0, MSB first, DEF 0xFE ("busy"), ORC 0x00, END->ACQUIRE short. */
#include <stdint.h>
#include <string.h>
#include "nrf.h"
#include "ohlink.h"
#include "ohcodec.h"

#define STAGE_BASE   0x10000u
#define STAGE_MAX    0xF800u           /* 0x10000-0x1F7FF (Index family: 0x1F800 = Valve's record page) */
#ifdef OHL_IDX
/* Valve nRF52 radio (Index controller, Vive Tracker 2018, Vive Pro controller, Tundra tracker), app at
 * 0x1000. Header in the Cortex-M4's reserved vector slots: +0x1C length, +0x20 marker, +0x24 version.
 * SPIS1: MOSI P0.13, MISO P0.12, SCK P0.14, CSN P0.11. P0.20 = "reply waiting" line to the main MCU. */
#define HDR_LEN      0x1Cu
#define HDR_MARK     0x20u
#define ATTN_PIN     20
#else
#define HDR_LEN      0x10u
#define HDR_MARK     0x14u
#endif
#ifdef OHL_IDX
#define S_TXPTR   NRF_SPIS1->TXD.PTR
#define S_MAXTX   NRF_SPIS1->TXD.MAXCNT
#define S_AMTTX   NRF_SPIS1->TXD.AMOUNT
#define S_RXPTR   NRF_SPIS1->RXD.PTR
#define S_MAXRX   NRF_SPIS1->RXD.MAXCNT
#define S_AMTRX   NRF_SPIS1->RXD.AMOUNT
#else
#define S_TXPTR   NRF_SPIS1->TXDPTR
#define S_MAXTX   NRF_SPIS1->MAXTX
#define S_AMTTX   NRF_SPIS1->AMOUNTTX
#define S_RXPTR   NRF_SPIS1->RXDPTR
#define S_MAXRX   NRF_SPIS1->MAXRX
#define S_AMTRX   NRF_SPIS1->AMOUNTRX
#endif
#define MARK_INSTALL 0x01051974u
#define MARK_BOOT    0xF001BA11u

/* ---------------- time (TIMER0 free-running at 1 MHz, polled) ---------------- */
static void timer_init(void)
{
  NRF_TIMER0->MODE = 0; NRF_TIMER0->BITMODE = 3; NRF_TIMER0->PRESCALER = 4;   /* 16 MHz / 16 */
  NRF_TIMER0->TASKS_CLEAR = 1; NRF_TIMER0->TASKS_START = 1;
}
static uint32_t now_us(void) { NRF_TIMER0->TASKS_CAPTURE[0] = 1; return NRF_TIMER0->CC[0]; }

/* ---------------- watchdog / boot counter ---------------- */
typedef struct { uint32_t magic, quick_boots, sum; } boot_rec_t;
static boot_rec_t boot_rec __attribute__((section(".noinit")));
#define BOOT_MAGIC 0x4F48524Fu
static int safe_mode;

static void wdt_feed(void) { NRF_WDT->RR[0] = 0x6E524635u; }
static void wdt_start(void)
{
  if (NRF_WDT->RUNSTATUS) { wdt_feed(); return; }
  NRF_WDT->CONFIG = 1;                 /* keep running while the CPU sleeps, pause on debug halt */
  NRF_WDT->CRV = 8 * 32768;
  NRF_WDT->RREN = 1;
  NRF_WDT->TASKS_START = 1;
}
static void reset_now(void)
{
  wdt_feed();
  NVIC_SystemReset();
}
static void boot_count(void)
{
  if (boot_rec.magic != BOOT_MAGIC || boot_rec.sum != (boot_rec.magic ^ boot_rec.quick_boots)) boot_rec.quick_boots = 0;
  if (NRF_POWER->RESETREAS & POWER_RESETREAS_RESETPIN_Msk) boot_rec.quick_boots = 0;
#ifdef OHL_IDX
  /* Index family: safe mode keeps the link off, and the link is the only way to update this radio, so
     only crashes count (watchdog, lockup). Requested resets (the main MCU's \RESET, the bootloader's
     install restart, our own) are normal: an update alone makes two in a row. */
  if (!(NRF_POWER->RESETREAS & (POWER_RESETREAS_DOG_Msk | POWER_RESETREAS_LOCKUP_Msk))) boot_rec.quick_boots = 0;
#endif
  boot_rec.quick_boots++;
  boot_rec.magic = BOOT_MAGIC; boot_rec.sum = boot_rec.magic ^ boot_rec.quick_boots;
  NRF_POWER->RESETREAS = 0xFFFFFFFF;
  if (boot_rec.quick_boots >= 3) safe_mode = 1;
}
static void boot_stable(void) { boot_rec.quick_boots = 0; boot_rec.sum = boot_rec.magic ^ 0; }

#ifdef OHL_DIAG   /* dev only: make TARGET=idx DIAG=1 */
/* survives resets (not power-off): what the main MCU sent around the last boots, read with air 'X' */
static struct { uint32_t magic; uint16_t boots, ny, nz, nc; uint8_t cst, rr, nop, scrub; uint16_t zbad, ack_q, ack_out, ybad; uint32_t zmax_us; } dg __attribute__((section(".noinit")));

/* SPI transfer trace, armed by the first SPI 'Y': [dt 100us u16][nrx][ntx][rx0][rx1][tx0][tx1][tx2][st] */
#define TR_N 96
static struct { uint32_t magic, n, t_last; uint8_t e[TR_N][10]; } tr __attribute__((section(".noinit")));
#define DG(x) do { x; } while (0)
#else
#define DG(x) do { } while (0)
#endif

/* ---------------- flash (staging bank only) ---------------- */
static void nvmc_wait(void) { while (!NRF_NVMC->READY) {} }
static void flash_erase_page(uint32_t a)
{
  if (a < STAGE_BASE || a >= STAGE_BASE + STAGE_MAX) return;   /* never outside the staging bank */
  NRF_NVMC->CONFIG = 2; nvmc_wait();
  NRF_NVMC->ERASEPAGE = a; nvmc_wait();
  NRF_NVMC->CONFIG = 0; nvmc_wait();
}
static void flash_write(uint32_t a, const uint32_t *w, uint32_t n)
{
  if (a < STAGE_BASE || a + 4 * n > STAGE_BASE + STAGE_MAX) return;
  NRF_NVMC->CONFIG = 1; nvmc_wait();
  for (uint32_t i = 0; i < n; i++) { ((volatile uint32_t *)a)[i] = w[i]; nvmc_wait(); }
  NRF_NVMC->CONFIG = 0; nvmc_wait();
}

/* ---------------- updater: HTC's protocol, byte for byte ---------------- */
static uint8_t stg_open;
static uint8_t stg_air;                /* staging opened over the radio link: keep the link up */
static uint32_t stg_off;

static int page_blank(uint32_t a)
{
  uint32_t page = NRF_FICR->CODEPAGESIZE;
  for (uint32_t i = 0; i < page; i += 4) if (*(volatile uint32_t *)(a + i) != 0xFFFFFFFFu) return 0;
  return 1;
}
#ifdef OHL_IDX
/* A 4 KB nRF52 page erase stalls the CPU for up to ~90 ms, and Valve's main firmware gives each staging
 * step ~120 ms (reply line + read included): erasing on the first block of a page made every USB
 * radio update fail at block 0. So staging is cleared ahead of time, a page per idle pass from 10 s
 * after boot, and blocks only write. A committed image that isn't installed yet is left alone. */
#define APP_BASE 0x1000u
static uint32_t scrub_page = STAGE_BASE;
static int staged_is_pending(void)
{
  uint32_t mark = *(volatile uint32_t *)(STAGE_BASE + HDR_MARK);
  if (mark != MARK_INSTALL && mark != MARK_BOOT) return 0;
  uint32_t len = *(volatile uint32_t *)(STAGE_BASE + HDR_LEN);
  if (len == 0 || len > STAGE_MAX) return 0;
  for (uint32_t i = 0; i < len; i += 4)                    /* same as what runs = already installed */
    if (i != HDR_MARK && *(volatile uint32_t *)(STAGE_BASE + i) != *(volatile uint32_t *)(APP_BASE + i)) return 1;
  return 0;
}
static void staging_scrub(void)
{
  if (stg_open || scrub_page >= STAGE_BASE + STAGE_MAX) return;
  if (scrub_page == STAGE_BASE && staged_is_pending()) { scrub_page = STAGE_BASE + STAGE_MAX; DG(dg.scrub = 0xEE); return; }
  if (!page_blank(scrub_page)) flash_erase_page(scrub_page);
  scrub_page += NRF_FICR->CODEPAGESIZE;
}
#endif
static int stage_write(const uint8_t *d, uint32_t n)       /* HTC FUN_000021c8 */
{
  uint32_t page = NRF_FICR->CODEPAGESIZE;
  if (!stg_open || stg_off > STAGE_MAX || n > 64 || (n & 3) || (stg_off & 3)) { stg_off += n; return 0; }
  if (stg_off + n > STAGE_MAX) return 0;
  uint32_t in_page = stg_off & (page - 1);
  if (in_page == 0) { if (!page_blank(STAGE_BASE + stg_off)) flash_erase_page(STAGE_BASE + stg_off); }
  else if (in_page + n > page && !page_blank(STAGE_BASE + stg_off - in_page + page))
    flash_erase_page(STAGE_BASE + stg_off - in_page + page);
  uint32_t w[16];
  memcpy(w, d, n);
  flash_write(STAGE_BASE + stg_off, w, n / 4);
  stg_off += n;
  return 1;
}
static void signature(uint32_t s[4])                       /* HTC FUN_000020cc */
{
  uint32_t len = *(volatile uint32_t *)(STAGE_BASE + HDR_LEN);
  uint32_t end = (len + STAGE_BASE + 15) & ~15u;
  uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  if (len > STAGE_MAX) end = STAGE_BASE;
  for (const volatile uint32_t *p = (const uint32_t *)STAGE_BASE; (uint32_t)p < end; p += 4) {
    uint32_t top = s1 << 31;
    uint32_t n1 = (s1 >> 1) ^ (s2 << 31) ^ p[1];
    uint32_t n2 = p[2] ^ ((s2 >> 1) | (s3 << 31));
    uint32_t t = s0 ^ (p[3] >> 31);
    uint32_t n3 = ((p[3] & 0x7FFFFFFFu) ^ (s3 >> 1)) | (((t ^ (s0 >> 2) ^ (s0 >> 27) ^ (s0 >> 29)) & 1u) << 31);
    s0 = ((s0 >> 1) | top) ^ p[0];
    s1 = n1; s2 = n2; s3 = n3;
  }
  s[0] = s0; s[1] = s1; s[2] = s2; s[3] = s3;
}
static int stage_commit(const uint8_t *sig, uint32_t marker)
{
  if (!stg_open) return 0;
  stg_open = 0;
  uint32_t s[4];
  signature(s);
  if (memcmp(s, sig, 16) != 0) return 0;
  flash_write(STAGE_BASE + HDR_MARK, &marker, 1);
  return 1;
}

/* ---------------- SPI slave link to the LPC ---------------- */
/* Every transaction clocks one LPC frame in and our pending reply out: [len incl. itself][bytes].
 * 0x00 = nothing pending; while we hold the semaphore the master reads DEF = 0xFE and retries. */
static uint8_t spi_rx[80];
static uint8_t spi_tx[80];
#define RQ_BYTES 1024                  /* replies to the LPC (events + downlink frames), [len][frame] */
static uint8_t rq[RQ_BYTES];
static uint16_t rq_head, rq_tail;      /* byte ring */
static uint8_t tx_loaded, tx_used;     /* reply in spi_tx: bytes to clock / ring bytes (0 = none) */

static uint8_t lead0;                  /* Index: 'l' 1 = every reply starts with a 0x00 byte */
static uint16_t rq_used(void) { return (uint16_t)((rq_tail - rq_head) & (RQ_BYTES - 1)); }
static int reply_push(const uint8_t *f, uint8_t n)          /* f = op + payload */
{
  if (n == 0 || n > 63 || rq_used() + n + 1 >= RQ_BYTES) return 0;
  rq[rq_tail] = (uint8_t)(n | (lead0 ? 0x80 : 0));         /* bit 7: send a 0x00 first (Index 'l' mode) */
  rq_tail = (rq_tail + 1) & (RQ_BYTES - 1);
  for (int i = 0; i < n; i++) { rq[rq_tail] = f[i]; rq_tail = (rq_tail + 1) & (RQ_BYTES - 1); }
  return 1;
}
static void reply2(uint8_t op, uint8_t v) { uint8_t b[2] = { op, v }; reply_push(b, 2); }

static void spis_init(void)
{
#ifdef OHL_IDX
  /* stock boot levels (FUN_00004fe4/4fb4/4fa4), then the SPI pins and the reply line */
  for (int i = 0; i <= 2; i++) { NRF_P0->PIN_CNF[i] = 3; NRF_P0->OUTSET = 1u << i; }
  for (int i = 4; i <= 6; i++) { NRF_P0->PIN_CNF[i] = 3; NRF_P0->OUTCLR = 1u << i; }
  NRF_P0->PIN_CNF[3] = 0;
  NRF_P0->PIN_CNF[ATTN_PIN] = 3; NRF_P0->OUTCLR = 1u << ATTN_PIN;
  NRF_P0->PIN_CNF[11] = 0; NRF_P0->PIN_CNF[12] = 0; NRF_P0->PIN_CNF[13] = 0; NRF_P0->PIN_CNF[14] = 0;
  NRF_SPIS1->PSEL.CSN = 11; NRF_SPIS1->PSEL.SCK = 14; NRF_SPIS1->PSEL.MISO = 13; NRF_SPIS1->PSEL.MOSI = 12;   /* Valve 0x9420: 0x50C (MISO) = 13, 0x510 (MOSI) = 12 */
#else
  NRF_GPIO->PIN_CNF[1] = 0; NRF_GPIO->PIN_CNF[3] = 0; NRF_GPIO->PIN_CNF[2] = 0; NRF_GPIO->PIN_CNF[0] = 0;
  NRF_SPIS1->PSELCSN = 3; NRF_SPIS1->PSELSCK = 0; NRF_SPIS1->PSELMOSI = 2; NRF_SPIS1->PSELMISO = 1;
#endif
  S_MAXRX = 0; S_MAXTX = 0;
  NRF_SPIS1->CONFIG = 0;               /* MSB first, CPHA 0, CPOL 0 */
  NRF_SPIS1->DEF = 0xFE;
  NRF_SPIS1->ORC = 0x00;
  NRF_SPIS1->EVENTS_END = 0; NRF_SPIS1->EVENTS_ACQUIRED = 0;
  NRF_SPIS1->SHORTS = SPIS_SHORTS_END_ACQUIRE_Msk;
  NRF_SPIS1->INTENCLR = 0xFFFFFFFF;
  NRF_SPIS1->ENABLE = 2;
  NRF_SPIS1->TASKS_ACQUIRE = 1;        /* first ACQUIRED: load buffers */
}

static void load_reply_and_release(void)
{
  tx_loaded = 0; tx_used = 0;
  if (rq_used()) {
    uint8_t h = rq[rq_head], n = h & 0x7F, o = 0;
    if (h & 0x80) spi_tx[o++] = 0;
    spi_tx[o] = (uint8_t)(n + 1);
    for (int i = 0; i < n; i++) spi_tx[o + 1 + i] = rq[(rq_head + 1 + i) & (RQ_BYTES - 1)];
    tx_loaded = (uint8_t)(o + n + 1);                       /* bytes the master must clock */
    tx_used = (uint8_t)(n + 1);                             /* ring bytes this reply takes */
  } else {
    spi_tx[0] = 0;
  }
  S_TXPTR = (uint32_t)spi_tx; S_MAXTX = tx_loaded ? tx_loaded : 1;
  S_RXPTR = (uint32_t)spi_rx; S_MAXRX = sizeof spi_rx;
  NRF_SPIS1->TASKS_RELEASE = 1;
#ifdef OHL_IDX
  if (tx_loaded) NRF_P0->OUTSET = 1u << ATTN_PIN; else NRF_P0->OUTCLR = 1u << ATTN_PIN;
#endif
}

static void link_from_lpc(const uint8_t *f, uint8_t n);   /* below */
static void addr_check(void);
static int link_safe_request, link_sleep;
/* survives resets (not power-off): what the main MCU sent around the last boots, read with air 'X' */
#ifdef OHL_DIAG
static void dg_boot(void)
{
  if (dg.magic != 0x44474E31u) { memset(&dg, 0, sizeof dg); dg.magic = 0x44474E31u; tr.magic = 0; }
  dg.boots++; dg.rr = (uint8_t)NRF_POWER->RESETREAS; dg.nop = 0;
}
#endif
static uint32_t lpc_frames, lpc_bad;

/* Staging replies: the HTC LPC wants the command echoed ('Y' 0, 'Z' 0, '[' 0); Valve's main firmware
   (Index, LWU radio update FUN_004213b8) counts '!' (0x21) + status replies, one per step, and gives
   up ("LWU: radio err") when one doesn't come within 120 ms. */
/* Staging replies echo the command ('Y' 0, 'Z' st, '[' st), like the HTC LPC wants and like Valve's
   own nRF52 radio does ('Y' reply = const 59 00 @0x9406). Valve's main fw counts 0x59/0x5a/0x5b/0x21
   as acks (FUN_004158a4 -> 0x4159ee) and gives each step ~120 ms (FUN_004213b8). */

#ifdef OHL_IDX
/* GPIO service, like Valve's radio: some of the device's buttons (on the Index, the ones it
   pairs with) are wired to the nRF, not the main MCU. The main MCU sets those pins up with 0x83
   requests ([0x83][op][4 B][pin][arg][u16 debounce ms]) and asks to be told about edges; each
   edge comes back as 84 14 00 00 00 00 [pin] [1 low / 2 high]. Without it the main MCU never
   sees those buttons, so its pairing combination never fires. */
#define GW_MAX 6
static struct { uint8_t used, pin, mode, last; uint16_t deb_ms; uint8_t pend; uint32_t since; } gw[GW_MAX];
static int gpio_ours(uint8_t pin) { return pin > 31 || (pin >= 11 && pin <= 14) || pin == ATTN_PIN; }
static void gpio_edge(uint8_t pin, uint8_t level)
{
  uint8_t ev[8] = { 0x84, 0x14, 0, 0, 0, 0, pin, (uint8_t)(level ? 2 : 1) };
  reply_push(ev, 8);
}
static void gpio_watch_poll(void)
{
  for (int i = 0; i < GW_MAX; i++) {
    if (!gw[i].used) continue;
    uint8_t lv = (uint8_t)((NRF_P0->IN >> gw[i].pin) & 1);
    if (lv == gw[i].last) { gw[i].pend = 0; continue; }
    if (!gw[i].pend || lv != (gw[i].pend & 1)) { gw[i].pend = (uint8_t)(2 | lv); gw[i].since = now_us(); }
    if (now_us() - gw[i].since < (uint32_t)gw[i].deb_ms * 1000u) continue;
    gw[i].last = lv; gw[i].pend = 0;
    if (gw[i].mode == 3 || (gw[i].mode == 1 && lv) || (gw[i].mode == 2 && !lv)) gpio_edge(gw[i].pin, lv);
  }
}
static void gpio_request(const uint8_t *f, uint8_t n)
{
  if (n < 6) { reply2(0x80, 0x04); return; }
  const uint8_t op = f[1], pin = f[6], arg = n > 7 ? f[7] : 0;
  uint8_t st = 2;
  if (gpio_ours(pin)) { reply2(0x80, 2); return; }
  volatile uint32_t *cnf = &NRF_P0->PIN_CNF[pin];
  switch (op) {
  case 0x00:                                               /* direction */
    if (n < 8) break;
    st = 0;
    if (arg == 0) *cnf |= 2; else if (arg == 1) *cnf = 0x303; else if (arg == 2) *cnf = 0; else st = 2;
    break;
  case 0x01: {                                             /* get direction */
    uint8_t v = (*cnf & 1) ? 1 : ((*cnf & 2) ? 0 : 2);
    uint8_t b[8] = { 0x84, 2, 0, 0, 0, 0, pin, v }; reply_push(b, 8); return;
  }
  case 0x03:                                               /* pull: 1 up, 2 down */
    if (n < 8) break;
    *cnf = (*cnf & ~0xCu) | ((uint32_t)(arg == 1 ? 3 : arg == 2 ? 1 : 0) << 2); st = 0; break;
  case 0x04: {
    uint32_t pl = (*cnf & 0xF) >> 2;
    uint8_t b[8] = { 0x84, 5, 0, 0, 0, 0, pin, (uint8_t)(pl == 1 ? 2 : pl == 3 ? 1 : 0) }; reply_push(b, 8); return;
  }
  case 0x06:                                               /* drive strength, same code */
    if (n < 8 || arg > 7) break;
    *cnf = (*cnf & ~0x700u) | ((uint32_t)arg << 8); st = 0; break;
  case 0x07: {
    uint8_t b[8] = { 0x84, 8, 0, 0, 0, 0, pin, (uint8_t)((*cnf >> 8) & 7) }; reply_push(b, 8); return;
  }
  case 0x09:                                               /* output level */
    if (n < 8 || pin == 0) break;
    if (arg == 0) { NRF_P0->OUTCLR = 1u << pin; st = 0; } else if (arg == 1) { NRF_P0->OUTSET = 1u << pin; st = 0; }
    break;
  case 0x0a: {                                             /* read level */
    uint8_t b[8] = { 0x84, 0x0b, 0, 0, 0, 0, pin, (uint8_t)((NRF_P0->IN >> pin) & 1) }; reply_push(b, 8); return;
  }
  case 0x0c: {                                             /* watch: arg 1 falling, 2 rising, 3 both, 0 off */
    if (n < 10) break;
    int k = -1;
    for (int i = 0; i < GW_MAX; i++) if (gw[i].used && gw[i].pin == pin) k = i;
    if (k >= 0) gw[k].used = 0;
    if (arg == 0) { st = 0; break; }
    if ((*cnf & 3) != 0) break;                            /* must be a connected input */
    if (k < 0) for (int i = 0; i < GW_MAX; i++) if (!gw[i].used) { k = i; break; }
    if (k < 0) break;
    gw[k].pin = pin; gw[k].mode = arg == 1 ? 2 : arg == 2 ? 1 : 3;   /* mode: 1 reports high, 2 low */
    gw[k].deb_ms = (uint16_t)(f[8] | f[9] << 8); gw[k].last = 0xFF; gw[k].pend = 0; gw[k].used = 1;
    st = 0;
    reply2(0x80, st);
    gpio_watch_poll();                                     /* like stock: report the level right away */
    return;
  }
  case 0x0d: {
    int k = -1;
    for (int i = 0; i < GW_MAX; i++) if (gw[i].used && gw[i].pin == pin) k = i;
    uint8_t m = k < 0 ? 0 : gw[k].mode == 1 ? 2 : gw[k].mode == 2 ? 1 : 3;
    uint16_t d = k < 0 ? 0xFFFF : gw[k].deb_ms;
    uint8_t b[10] = { 0x84, 0x0e, 0, 0, 0, 0, pin, m, (uint8_t)d, (uint8_t)(d >> 8) }; reply_push(b, 10); return;
  }
  default: break;
  }
  reply2(0x80, st);
}
#endif

static void handle_lpc_frame(const uint8_t *f, uint32_t n)
{
  uint8_t op = f[0];
  DG(if (op == 'Y') dg.ny++; else if (op == 'Z') dg.nz++);
  switch (op) {
  case 0x00: case 0xFF: return;                            /* filler: only clocks our reply out */
  case 'X': {
    uint8_t b[5] = { 'X' }; uint32_t v = OHL_VERSION; memcpy(b + 1, &v, 4); reply_push(b, 5); return;
  }
  case 'c': {
    uint8_t b[9] = { 'c' }; memcpy(b + 1, (const void *)NRF_FICR->DEVICEID, 8); reply_push(b, 9); return;
  }
  case 'Y':                                                /* drop pending replies: during staging only
                                                              2-byte updater replies are ever pending, so
                                                              they never stretch a Z frame with fill */
    if (tx_loaded) { rq_head = (rq_head + tx_used) & (RQ_BYTES - 1); tx_loaded = 0; }
    rq_head = rq_tail;
    stg_open = 1; stg_air = 0; stg_off = 0; reply2('Y', 0); return;
  case 'Z': reply2('Z', stage_write(f + 1, n - 1) ? 0 : 1); return;
  case '[': reply2('[', (n == 17 && stage_commit(f + 1, MARK_INSTALL)) ? 0 : 1); return;
  case '!': reply2('!', (n == 17 && stage_commit(f + 1, MARK_BOOT)) ? 0 : 1); return;
  case 0x20: {                                             /* HTC: reboot into the bootloader to install */
    if (stg_open) { reply2(0x20, 1); return; }
    if (*(volatile uint32_t *)(STAGE_BASE + HDR_LEN) != 0xFFFFFFFFu &&
        *(volatile uint32_t *)(STAGE_BASE + HDR_MARK) == MARK_BOOT) {
      NRF_POWER->GPREGRET = 0x71;
      reset_now();
    }
    reply2(0x20, 2); return;
  }
  case '\\':
    if (n >= 6 && memcmp(f, "\\RESET", 6) == 0) reset_now();
    return;
  case 'O':                                                /* ours: 'O' 'S' = stay in safe mode */
    if (n >= 2 && f[1] == 'S') link_safe_request = 1;
    reply2('O', (uint8_t)(safe_mode | link_safe_request << 1));
    return;
#ifdef OHL_IDX
  case 'l': reply2('l', n >= 2 ? f[1] : 0); lead0 = n >= 2 && f[1] != 0; return;   /* reply, then switch */
  case 0x81: reply2(0x82, 0x01); return;                   /* stock constant (0x9434) */
  case 0x83: gpio_request(f, (uint8_t)n); return;
  case 0xA0: case 'o': case 'W': case '^': case 'a': case 'b': case 'e': case 'k': case '_': case '`':
  case '"': case 0x90: case 'f': case 'd': case 'm':
    return;                                                /* Valve link key / pairing store / test mode */
#endif
  default:
    link_from_lpc(f, (uint8_t)n);                          /* p/q/s/r/Q/n/V/P data, S/R/T link control */
    return;
  }
}

static uint8_t dbg_rx[12], dbg_nrx, dbg_ntx;   /* last SPI frame from the main MCU (air 'X' diagnostics) */
static void spis_poll(void)
{
  if (!NRF_SPIS1->EVENTS_ACQUIRED) return;
  NRF_SPIS1->EVENTS_ACQUIRED = 0;
  if (NRF_SPIS1->EVENTS_END) {
    NRF_SPIS1->EVENTS_END = 0;
    uint32_t nrx = S_AMTRX, ntx = S_AMTTX;
#ifdef OHL_IDX
    NRF_P0->OUTCLR = 1u << ATTN_PIN;
#endif
    if (tx_loaded && ntx >= tx_loaded) {                   /* the master clocked our whole reply */
      rq_head = (rq_head + tx_used) & (RQ_BYTES - 1);
      tx_loaded = 0;
    }
    addr_check();
#ifdef OHL_DIAG
    /* dev: log every unusual transfer (not the 0x70/0x71/0x83 data stream, or one where we sent more
       than a byte), kept across resets until full: what the main MCU does around power-off */
    if (tr.magic != 0x54524332u) { tr.magic = 0x54524332u; tr.n = 0; tr.t_last = now_us(); }
    const uint8_t c = nrx > 0 ? spi_rx[0] : 0xff;
    const int unusual = (c != 0x70 && c != 0x71 && c != 0x83 && c != 0x6e && c != 0xff) || ntx > 1;
    if (unusual && tr.n < TR_N) {
      uint32_t t = now_us(), dt = (t - tr.t_last) / 100; tr.t_last = t;
      uint8_t *e = tr.e[tr.n++];
      e[0] = (uint8_t)dt; e[1] = (uint8_t)(dt > 0xFFFF ? 0xFF : dt >> 8);
      e[2] = (uint8_t)nrx; e[3] = (uint8_t)ntx; e[4] = spi_rx[0]; e[5] = spi_rx[1];
      e[6] = spi_tx[0]; e[7] = spi_tx[1]; e[8] = spi_tx[2]; e[9] = (uint8_t)NRF_SPIS1->STATUS;
      NRF_SPIS1->STATUS = 3;
    }
#endif
    if (nrx > 0) { memcpy(dbg_rx, spi_rx, 12); dbg_nrx = (uint8_t)nrx; dbg_ntx = (uint8_t)ntx; lpc_frames++; handle_lpc_frame(spi_rx, nrx); } else lpc_bad++;
    addr_check();
  }
  load_reply_and_release();
  addr_check();
}

/* ---------------- radio link v1 (tracker side, see common/ohlink.h) ---------------- */
#define UQ_BYTES 1024                  /* uplink frames from the LPC, [len][frame] */
static uint8_t uq[UQ_BYTES];
static uint16_t uq_head, uq_tail;
static uint32_t uq_drops;
static ohc_state_t ohc;                 /* air codec state, reset on every join */
static uint16_t uq_used(void) { return (uint16_t)((uq_tail - uq_head) & (UQ_BYTES - 1)); }

static uint8_t pkt_rx[2 + OHL_MAXLEN];
static uint8_t pb[3][2 + OHL_MAXLEN];  /* RESP buffers: [len][payload] */
static uint8_t ob[3], on;              /* outstanding (sent, unacked) buffers, oldest first */
static uint8_t send_i;                 /* next ob[] entry to (re)send */
static uint8_t next_seq;
static uint8_t armed;                  /* buffer the next auto-TX sends */
static uint8_t jreq[2 + OHL_JREQ_LEN];
static uint8_t last_dseq = 1;

static uint8_t link_up, link_enabled, joined, my_slot;
static uint32_t net_base;
static uint16_t mask = 0xFFFF;
static uint8_t cur_ch;
static uint32_t last_poll_us;
static uint8_t poll_rssi, poll_missed, poll_crc_err, last_cycle, have_cycle;
static uint8_t got_addr, heard, poll_ok, poll_ack, answer_join;   /* got_addr: answer pointer set */
/* At ADDRESS, point the radio at the prepared answer before the hardware turnaround
   (END -> DISABLED -> TXEN, ~200 us later) starts the TX. No interrupts (HTC's bootloader owns the
   vector table), so addr_check() is also called from inside the main loop's longer steps. */
static uint32_t ans_ptr, isr_t;
static uint8_t isr_heard;
static void addr_check(void)
{
  if (!NRF_RADIO->EVENTS_ADDRESS) return;
  NRF_RADIO->EVENTS_ADDRESS = 0;
  if (ans_ptr) NRF_RADIO->PACKETPTR = ans_ptr;
  NRF_TIMER0->TASKS_CAPTURE[1] = 1; isr_t = NRF_TIMER0->CC[1];
  isr_heard = 1;
}
static uint32_t period_us = 2000, t_addr, t_last_poll;    /* cycle period (from POLLs), poll timing */
static uint8_t max_resp = OHL_MAXLEN;
static uint32_t rng = 0x12345678u;
enum { R_RX, R_TX } rstate;

static void uplink_push(const uint8_t *f, uint8_t n);
static void go_unjoined(void);
/* Pairing, like stock: the main MCU starts it ('T', from its pairing buttons or the host's pair
   command) and ends it ('R', or its own timeout). While pairing, the radio leaves its dongle, says
   so in its join requests, and only joins a dongle that has pairing open, so the dongle it was on
   can't take it back; a dongle only takes a device it doesn't know when it asks to pair. */
static uint8_t pair_mode;
static uint32_t pair_since;
static void pair_start(void)
{
  pair_mode = 1; pair_since = now_us();
  if (link_enabled && (joined || link_up)) go_unjoined();  /* reports 0x53: the LED shows pairing */
  else if (link_up) { link_up = 0; uint8_t ev[1] = { 0x53 }; reply_push(ev, 1); }
  jreq[10] = 1;
}
static void pair_end(void) { pair_mode = 0; jreq[10] = 0; }
#ifdef OHL_IDX
/* Holding the system button turns the device off. Valve's main firmware leaves that to the
   radio side: it only reports the button (bit 0x2000 of the 0x6e input frame's first word), and
   stock turns it off after ~4 s, with or without a host. We hand the main MCU the same power-off
   command a host sends ('P' 9f 04 "off!"), once per hold. */
static uint32_t sys_since;
static uint8_t sys_held, sys_off_sent;
static void sys_button(const uint8_t *f, uint8_t n)
{
  if (n < 5) return;
  uint32_t b; memcpy(&b, f + 1, 4);
  if (!(b & 0x2000u)) { sys_held = 0; sys_off_sent = 0; return; }
  if (!sys_held) { sys_held = 1; sys_since = now_us(); }
}
static void sys_button_poll(void)
{
  if (!sys_held || sys_off_sent || now_us() - sys_since < 4000000u) return;
  static const uint8_t off[7] = { 'P', 0x9f, 4, 'o', 'f', 'f', '!' };
  if (reply_push(off, sizeof off)) sys_off_sent = 1;
}
#endif
static void link_from_lpc(const uint8_t *f, uint8_t n)
{
#ifdef OHL_IDX
  if (f[0] == 0x6e) sys_button(f, n);
  if (f[0] == 'S') {                                       /* main MCU sleeps: radio off until 'R'/'T' */
    /* Valve's radio answers every 'S' with 0x53 (link down), connected or not, and the main
       firmware waits for it before it powers the device off: without it, holding the system
       button never turned the controller off. */
    link_sleep = 1; link_up = 0;
    uint8_t ev[1] = { 0x53 }; reply_push(ev, 1);
    return;
  }
  if (f[0] == 'T') { link_sleep = 0; pair_start(); return; }
  if (f[0] == 'R') { link_sleep = 0; pair_end(); return; }
#else
  if (f[0] == 'T') { pair_start(); return; }               /* HTC link control: T pair, R connect */
  if (f[0] == 'R') { pair_end(); return; }
  if (f[0] == 'S') return;
#endif
  if (!link_up) return;                                    /* like HTC: data only while connected */
  uplink_push(f, n);
}
static void uplink_push(const uint8_t *f, uint8_t n)
{
  if (uq_used() + n + 1 >= UQ_BYTES) { uq_drops++; return; }   /* drop BEFORE the codec sees it */
  static uint8_t enc[80];
  addr_check();
  int m = ohc_encode(&ohc, f, n, enc);
  addr_check();
  if (m) { f = enc; n = (uint8_t)m; }
  uq[uq_tail] = n; uq_tail = (uq_tail + 1) & (UQ_BYTES - 1);
  for (int i = 0; i < n; i++) { uq[uq_tail] = f[i]; uq_tail = (uq_tail + 1) & (UQ_BYTES - 1); }
}

static void set_addr(uint32_t base, uint8_t prefix)
{
  NRF_RADIO->BASE0 = base; NRF_RADIO->PREFIX0 = prefix;
  NRF_RADIO->TXADDRESS = 0; NRF_RADIO->RXADDRESSES = 1;
}
static void radio_setup(void)
{
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0; NRF_CLOCK->TASKS_HFCLKSTART = 1;
  uint32_t t = now_us();
  while (!NRF_CLOCK->EVENTS_HFCLKSTARTED && now_us() - t < 5000) {}
  NRF_RADIO->POWER = 1;
  NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
  NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_Pos4dBm;
  NRF_RADIO->PCNF0 = 8 << RADIO_PCNF0_LFLEN_Pos;
  NRF_RADIO->PCNF1 = (OHL_MAXLEN << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) |
                     (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos);
  NRF_RADIO->CRCCNF = 2; NRF_RADIO->CRCINIT = 0xFFFF; NRF_RADIO->CRCPOLY = 0x11021;
  NRF_RADIO->INTENCLR = 0xFFFFFFFF;
  memcpy(jreq + 2, (const void *)NRF_FICR->DEVICEID, 8);
  rng ^= NRF_FICR->DEVICEID[0];
}
static void radio_stop(void)
{
  ans_ptr = 0;
  NRF_RADIO->SHORTS = 0;
  NRF_RADIO->EVENTS_DISABLED = 0; NRF_RADIO->TASKS_DISABLE = 1;
  uint32_t t = now_us(); while (!NRF_RADIO->EVENTS_DISABLED && now_us() - t < 200) {}
  NRF_RADIO->EVENTS_DISABLED = 0;
}
/* listen; if auto_tx, the radio answers the next packet on its own with pb[armed] / jreq */
static void arm_rx(int auto_tx)
{
  NRF_RADIO->FREQUENCY = cur_ch;
  NRF_RADIO->PACKETPTR = (uint32_t)pkt_rx;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk |
                      RADIO_SHORTS_ADDRESS_RSSISTART_Msk | (auto_tx ? RADIO_SHORTS_DISABLED_TXEN_Msk : 0);
  NRF_RADIO->EVENTS_DISABLED = 0; NRF_RADIO->EVENTS_ADDRESS = 0; NRF_RADIO->EVENTS_READY = 0;
  got_addr = 0; heard = 0; isr_heard = 0;
  ans_ptr = auto_tx ? (joined ? (uint32_t)pb[armed] : (uint32_t)jreq) : 0;
  NRF_RADIO->TASKS_RXEN = 1;
  rstate = R_RX;
}

static void go_unjoined(void)
{
  if (link_up) { uint8_t ev[1] = { 0x53 }; reply_push(ev, 1); }
  link_up = 0; joined = 0; on = 0; send_i = 0; uq_head = uq_tail;
  radio_stop();
  set_addr(OHL_JOIN_BASE, OHL_JOIN_PREFIX);
  cur_ch = OHL_JOIN_CHANNEL;
  jreq[0] = OHL_JREQ_LEN; jreq[1] = OHL_JREQ; jreq[10] = pair_mode;
  answer_join = 0;
  arm_rx(0);
}
static void go_joined(uint32_t base, uint8_t slot)
{
  radio_stop();
  joined = 1; my_slot = slot; net_base = base;
  memset(&ohc, 0, sizeof ohc); uq_head = uq_tail;
  on = 0; send_i = 0; next_seq = 0; last_dseq = 1; have_cycle = 0;
  mask = 0xFFFF; cur_ch = ohl_hop_ch[0]; period_us = 2000; max_resp = OHL_MAXLEN;
  last_poll_us = now_us();
  set_addr(base, (uint8_t)(OHL_SLOT_PREFIX + slot));
#ifdef OHL_IDX
  if (!link_up) { link_up = 1; uint8_t ev[1] = { 0x52 }; reply_push(ev, 1); }
#else
  if (!link_up) { link_up = 1; uint8_t ev[6] = { 0x52, slot, 0, 0, 0, 0 }; reply_push(ev, 6); }
#endif
}

static uint8_t build_new(void)                             /* fill a free buffer from the uplink queue */
{
  uint8_t b = 0;
  while ((on > 0 && ob[0] == b) || (on > 1 && ob[1] == b)) b++;
  uint8_t *p = pb[b] + 1;
  int len = OHL_RESP_HDR;
  while (uq_used()) {
    uint8_t n = uq[uq_head];
    if (len + 1 + n > max_resp) break;
    p[len++] = n;
    for (int i = 0; i < n; i++) p[len++] = uq[(uq_head + 1 + i) & (UQ_BYTES - 1)];
    uq_head = (uq_head + 1 + n) & (UQ_BYTES - 1);
  }
  pb[b][0] = (uint8_t)len;
  p[0] = OHL_RESP; p[1] = my_slot; p[2] = next_seq & OHL_F_SEQ_MASK;
  next_seq = (next_seq + 1) & OHL_F_SEQ_MASK;
  ob[on++] = b;
  return b;
}
static void arm_next(void)                                 /* pick what the next auto-TX sends, then listen */
{
  uint8_t b;
  if (send_i < on) b = ob[send_i];
  else if (on < 2) { b = build_new(); send_i = (uint8_t)(on - 1); }
  else { send_i = 0; b = ob[0]; }
  uint8_t *p = pb[b] + 1;
  p[2] = (uint8_t)((p[2] & OHL_F_SEQ_MASK) | (last_dseq ? OHL_F_DSEQ : 0));
  p[3] = poll_rssi; p[4] = poll_missed; p[5] = poll_crc_err;
  armed = b;
  arm_rx(1);
}
static void exchange_done(void)                            /* our RESP pb[armed] just went out */
{
  if (poll_ok) {                                           /* drop everything the poll acknowledged */
    while (on && ((poll_ack - (pb[ob[0]][3] & OHL_F_SEQ_MASK)) & 7) < 4) {
      ob[0] = ob[1]; ob[1] = ob[2]; on--;
    }
  }
  int pos = -1;
  for (int i = 0; i < on; i++) if (ob[i] == armed) pos = i;
  if (poll_ok && on && ob[0] != armed) send_i = 0;         /* an older one is still unacked: lost, go back */
  else send_i = (uint8_t)(pos + 1);
}

/* Recovery updater over the radio link (works without the main MCU): a downlink frame starting
 * with OHL_AIR_UPD is handled here and never reaches the main MCU. Same staging bank, signature and
 * install marker as the SPI updater, so any radio image (ours or Valve's) can be put back.
 *   F0 'X'          -> F0 'X' ver[4] open off[4]
 *   F0 'Y'          -> F0 'Y' 0          open staging (link stays up)
 *   F0 'Z' off[4] d -> F0 'Z' st off[4]  write d at off (must equal the next offset; resends are acked)
 *   F0 '[' sig[16]  -> F0 '[' st         check + commit (installs at the next reset)
 *   F0 'R'          -> F0 'R' 0          reset ~300 ms later */
static uint32_t air_reset_at;
static void air_reply(const uint8_t *b, uint8_t n) { uplink_push(b, n); }
static void air_update(const uint8_t *f, uint8_t n)
{
  uint8_t r[12] = { OHL_AIR_UPD, n > 1 ? f[1] : 0 };
  if (n < 2) return;
  switch (f[1]) {
  case 'X': {
    uint8_t x[60] = { OHL_AIR_UPD, 'X' }; uint32_t v = OHL_VERSION;
    memcpy(x + 2, &v, 4); x[6] = stg_open; memcpy(x + 7, &stg_off, 4);
    memcpy(x + 11, &lpc_frames, 4); memcpy(x + 15, &lpc_bad, 4);
    x[19] = (uint8_t)(safe_mode | link_safe_request << 1 | link_sleep << 2 | lead0 << 3);
    x[20] = dbg_nrx; x[21] = dbg_ntx; memcpy(x + 22, dbg_rx, 12);
    x[34] = (uint8_t)rq_used(); x[35] = (uint8_t)boot_rec.quick_boots;
#ifdef OHL_DIAG
    memcpy(x + 36, &dg.boots, 8); x[44] = dg.cst; x[45] = dg.rr; x[46] = dg.scrub; memcpy(x + 47, &dg.zbad, 12);
    air_reply(x, 59); return;
#else
    air_reply(x, 36); return;
#endif
  }
#ifdef OHL_DIAG
  case 'D': {                                              /* trace dump, 5 entries per request */
    uint8_t x[60] = { OHL_AIR_UPD, 'D' }; uint8_t i0 = n > 2 ? f[2] : 0;
    x[2] = i0; x[3] = (uint8_t)(tr.magic == 0x54524332u ? tr.n : 0);
    for (int k = 0; k < 5 && i0 + k < TR_N; k++) memcpy(x + 4 + 10 * k, tr.e[i0 + k], 10);
    air_reply(x, 54); return;
  }
#endif
  case 'Y': stg_open = 1; stg_air = 1; stg_off = 0; r[2] = 0; air_reply(r, 3); return;
  case 'Z': {
    uint32_t off; if (n < 6) return;
    memcpy(&off, f + 2, 4);
    if (!stg_open || !stg_air) r[2] = 2;
    else if (off + (n - 6) == stg_off) r[2] = 0;                     /* resend of the last block */
    else if (off != stg_off) r[2] = 3;
    else r[2] = stage_write(f + 6, n - 6) ? 0 : 1;
    memcpy(r + 3, &stg_off, 4); air_reply(r, 7); return;
  }
  case '[': r[2] = (n == 18 && stg_air && stage_commit(f + 2, MARK_INSTALL)) ? 0 : 1; stg_air = 0; air_reply(r, 3); return;
  case 'R': r[2] = 0; air_reply(r, 3); air_reset_at = now_us() + 300000u; if (!air_reset_at) air_reset_at = 1; return;
  }
}

__attribute__((noipa)) static void handle_poll(const uint8_t *p, int len)   /* noinline: radio_emu calls it */
{
  uint8_t cyc = p[3];
  if (have_cycle) poll_missed += (uint8_t)(cyc - last_cycle - 1);
  last_cycle = cyc; have_cycle = 1;
  poll_rssi = (uint8_t)NRF_RADIO->RSSISAMPLE;
  poll_ack = p[2] & OHL_F_SEQ_MASK;
  mask = (uint16_t)(p[4] | p[5] << 8);
  if (p[6]) period_us = p[6] * 50u;
  max_resp = p[7] >= OHL_RESP_HDR + 2 && p[7] <= OHL_MAXLEN ? p[7] : OHL_MAXLEN;
  last_poll_us = now_us();
  t_last_poll = t_addr;
  if ((p[2] & OHL_F_DOWN) && ((p[2] & OHL_F_DSEQ) ? 1 : 0) != last_dseq) {
    last_dseq = (p[2] & OHL_F_DSEQ) ? 1 : 0;              /* new downlink frames: hand them to the LPC */
    for (int i = OHL_POLL_HDR; i + 1 <= len; ) {
      uint8_t n = p[i];
      if (n == 0 || i + 1 + n > len) break;
      if (p[i + 1] == OHL_AIR_UPD) air_update(p + i + 1, n);
      else reply_push(p + i + 1, n);
      i += 1 + n;
    }
  }
  cur_ch = ohl_channel((uint8_t)(cyc + 1), mask);          /* where the next cycle will be */
}

static void radio_poll(void)
{
  if (!link_enabled) return;
  addr_check();
  if (isr_heard && rstate == R_RX) {
    isr_heard = 0;
    t_addr = isr_t;
    heard = 1;
    got_addr = ans_ptr != 0;
  }
  if (!NRF_RADIO->EVENTS_DISABLED) {
    uint32_t t = now_us();
    if (joined && t - last_poll_us > 1000000) go_unjoined();          /* dongle gone */
    else if (joined && have_cycle && rstate == R_RX && !heard) {
      /* missed polls: keep hopping on our own clock. The poll for cycle last+j is due at
         t_last_poll + j*P; be on its channel from half a period before until half after. */
      uint32_t j = (t - t_last_poll + period_us / 2) / period_us;
      if (j >= 1) {
        uint8_t ch = ohl_channel((uint8_t)(last_cycle + j), mask);
        if (ch != cur_ch) { radio_stop(); cur_ch = ch; arm_next(); }
      }
    }
    return;
  }
  NRF_RADIO->EVENTS_DISABLED = 0;
  if (rstate == R_RX) {
    const uint8_t *p = pkt_rx + 1;
    int len = pkt_rx[0];
    int crc = NRF_RADIO->CRCSTATUS;
    if (joined) {
      /* TXEN already fired through the shortcut: stop it from firing again after the TX */
      NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
      if (!got_addr) { radio_stop(); arm_next(); return; } /* never answer with the wrong buffer */
      poll_ok = crc && len >= OHL_POLL_HDR && p[0] == OHL_POLL && p[1] == my_slot;
      if (!crc) poll_crc_err++;
      if (poll_ok) handle_poll(p, len);
      rstate = R_TX;
      return;
    }
    /* unjoined: JOIN beacons */
    if (answer_join) {
      NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
      rstate = R_TX;                                       /* our JREQ is going out */
    }
    if (crc && len >= OHL_JOIN_LEN && p[0] == OHL_JOIN) {
      uint32_t base; memcpy(&base, p + 1, 4);
      if (memcmp(p + 6, jreq + 2, 8) == 0 && p[14] < OHL_SLOTS && (!pair_mode || p[5])) {
        pair_end();                                         /* paired (or back on its dongle) */
        if (rstate == R_TX) { while (!NRF_RADIO->EVENTS_DISABLED) {} NRF_RADIO->EVENTS_DISABLED = 0; }
        go_joined(base, p[14]);
        mask = (uint16_t)(p[15] | p[16] << 8);                 /* follow the hops from the start */
        if (p[18]) period_us = p[18] * 50u;
        last_cycle = p[17]; have_cycle = 1;
        t_last_poll = t_addr - (uint32_t)(p[19] | p[20] << 8);
        cur_ch = ohl_channel((uint8_t)(last_cycle + (now_us() - t_last_poll + period_us / 2) / period_us), mask);
        arm_next();
        return;
      }
      rng = rng * 1664525u + 1013904223u;
      answer_join = (rng >> 24) & 1;                       /* answer half the beacons: two new trackers
                                                              won't collide forever */
    }
    if (rstate == R_RX) arm_rx(answer_join);
    return;
  }
  /* R_TX finished */
  if (joined) { exchange_done(); arm_next(); }
  else arm_rx(answer_join);
}

/* ---------------- main ---------------- */
int main(void)
{
  DG(dg_boot());
  boot_count();
  wdt_start();
  timer_init();
  spis_init();
  uint32_t t0 = now_us();
  for (;;) {
    wdt_feed();
    spis_poll();                                           /* updater lives here: always first */
    if (!link_enabled && !safe_mode && !link_safe_request && !link_sleep && !(stg_open && !stg_air) && now_us() - t0 > 1000000) {
      radio_setup(); link_enabled = 1; go_unjoined();
    }
    if (link_enabled && ((stg_open && !stg_air) || link_safe_request || link_sleep)) { /* updating / safe / asleep */
      radio_stop(); link_enabled = 0; joined = 0;
      if (link_up) { link_up = 0; uint8_t ev[1] = { 0x53 }; reply_push(ev, 1); }
    }
    radio_poll();
#ifdef OHL_IDX
    sys_button_poll();
    gpio_watch_poll();
#endif
    if (pair_mode && now_us() - pair_since > 35000000u) pair_end();   /* the main MCU gives up at 30 s */
    if (air_reset_at && (int32_t)(now_us() - air_reset_at) > 0) reset_now();
    if (boot_rec.quick_boots && now_us() - t0 > 5000000) boot_stable();
#ifdef OHL_IDX
    if (now_us() - t0 > 10000000) staging_scrub();
#endif
  }
}
