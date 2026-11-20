/* openhouse link v1, dongle side (see common/doorman_link.h): slots, joining/pairing, hopping, go-back-N.
 * Everything is polled from the main loop (no radio interrupts). */
#include <string.h>
#include "nrf.h"
#include "doorman_link.h"
#include "doorman_codec.h"
#include "link.h"
#include "dongle.h"

extern volatile uint32_t ms_ticks;
static uint32_t us_now(void) { NRF_TIMER1->TASKS_CAPTURE[0] = 1; return NRF_TIMER1->CC[0]; }

/* ---------------- paired trackers (persisted in the last app flash page) ---------------- */
#if BOARD_NORDIC_DFU
#define PAIR_PAGE   0xDA000u           /* storage page; Nordic's open bootloader owns 0xE0000+ */
#else
#define PAIR_PAGE   0xEC000u
#endif
#define PAIR_MAGIC  0x5250484Fu        /* "OHPR" */
typedef struct { uint32_t magic, used; uint8_t id[DML_SLOTS][8]; } pair_store_t;
static pair_store_t pairs;

static void nvmc_wait(void) { while (!NRF_NVMC->READY) {} }
static void pairs_load(void)
{
  memcpy(&pairs, (const void *)PAIR_PAGE, sizeof pairs);
  if (pairs.magic != PAIR_MAGIC) { memset(&pairs, 0, sizeof pairs); pairs.magic = PAIR_MAGIC; }
}
static void pairs_save(void)
{
  extern uint32_t __etext;                                  /* never erase our own code */
  if ((uint32_t)&__etext > PAIR_PAGE) return;
  NRF_NVMC->CONFIG = 2; nvmc_wait();
  NRF_NVMC->ERASEPAGE = PAIR_PAGE; nvmc_wait();
  NRF_NVMC->CONFIG = 1; nvmc_wait();
  const uint32_t *w = (const uint32_t *)&pairs;
  for (unsigned i = 0; i < sizeof pairs / 4; i++) { ((volatile uint32_t *)PAIR_PAGE)[i] = w[i]; nvmc_wait(); }
  NRF_NVMC->CONFIG = 0; nvmc_wait();
}

/* ---------------- per-slot state ---------------- */
#define UPQ 1024
typedef struct {
  uint8_t active;                       /* answered recently */
  uint8_t last_seq;                     /* last in-order RESP seq (the ack we send) */
  uint8_t dseq, down_len, down_buf[200];
  uint32_t last_resp_ms;
  uint8_t upq[UPQ]; uint16_t up_head, up_tail;
  /* stats */
  uint32_t polls, resp_ok, crc_err, timeouts, dup, frames_up, bytes_up, drops, down_frames, down_resends;
  uint32_t rssi_d16, rssi_t16, t_missed, t_crc;
  uint8_t last_missed, last_crc, have_tel;
  dmc_state_t codec; uint32_t air_bytes, bad_frames;
  uint16_t dev_drops, dev_reply_drops;   /* the radio's own drop totals (its 'D' frames) */
  uint8_t rssi_last_d, rssi_last_t, rssi_min_d, rssi_max_d;
  uint8_t ident[16];                    /* last 'H' frame, ident[0] = 0: none yet */
  uint16_t frag_left;                   /* bytes of a big frame still to send as 0x81 fragments */   /* raw -dBm; min/max reset on read (0x29) */
} slot_t;
static slot_t sl[DML_SLOTS];
static uint8_t link_on = 1;
static uint32_t net_base;
static uint8_t cycle;
static uint16_t mask = 0xFFFF, next_mask = 0xFFFF;
static uint8_t cur_ch;

/* per hop channel loss statistics (for adaptive hopping) */
static struct { uint16_t polls, lost; uint32_t off_until_ms; uint8_t loss_pct, strikes; } chs[DML_NHOP];
static uint32_t ch_eval_ms;

/* pairing */
static uint32_t pairing_until_ms;
static uint8_t pair_target = 0xFF;   /* slot the next pairing goes to (0xFF: first free) */
static uint8_t assign_id[8], assign_slot = 0xFF, assign_left;
/* The device paired last: its slot and until when its join beacons still say "pairing open". Pairing
   closes here the moment the device is stored, but the device stays in pairing mode until it joins, and
   a device in pairing mode only takes a beacon with pairing open: without this it ignored its own slot
   assignment until its pairing timed out (~35 s), shown here as paired with 100% loss. Ends when it
   answers a poll: a pairing started after that needs pairing open again. */
static uint8_t just_paired_slot = 0xFF;
static uint32_t just_paired_until_ms;
static uint32_t last_join_ms, joins_heard;

static uint8_t pkt_tx[2 + DML_MAXLEN], pkt_rx[2 + DML_MAXLEN];
static uint32_t period_us = 2000, slot_us = 1450;
static uint8_t max_resp = DML_MAXLEN;
static void (*idle_hook)(void);
void link_set_idle_hook(void (*f)(void)) { idle_hook = f; }
static void wait_until(uint32_t t) { while ((int32_t)(us_now() - t) < 0) if (idle_hook) idle_hook(); }

static uint16_t upq_used(const slot_t *s) { return (uint16_t)((s->up_tail - s->up_head) & (UPQ - 1)); }

/* ---------------- front-end amplifier (Holyiot 21017: SKY66112, CTX P0.24 / CRX P0.22) ----------------
   Both pins are GPIOTE task outputs. Software raises CTX before TXEN and CRX before RXEN; the TX->RX
   turnaround (DISABLED_RXEN shortcut) is too fast for the CPU, so PPI swaps CTX->CRX on that DISABLED. */
#if BOARD_HOLYIOT
#define FEM_CTX_PIN 24
#define FEM_CRX_PIN 22
#define FEM_GT_CTX 6
#define FEM_GT_CRX 7
#define FEM_PPI 0
static void fem_init(void)
{
  NRF_GPIOTE->CONFIG[FEM_GT_CTX] = 3 | (FEM_CTX_PIN << 8);   /* task mode, starts low */
  NRF_GPIOTE->CONFIG[FEM_GT_CRX] = 3 | (FEM_CRX_PIN << 8);
  NRF_PPI->CH[FEM_PPI].EEP = (uint32_t)&NRF_RADIO->EVENTS_DISABLED;
  NRF_PPI->CH[FEM_PPI].TEP = (uint32_t)&NRF_GPIOTE->TASKS_CLR[FEM_GT_CTX];
  NRF_PPI->FORK[FEM_PPI].TEP = (uint32_t)&NRF_GPIOTE->TASKS_SET[FEM_GT_CRX];
  NRF_PPI->CHENCLR = 1u << FEM_PPI;
}
static void fem_tx(void)  { NRF_GPIOTE->TASKS_CLR[FEM_GT_CRX] = 1; NRF_GPIOTE->TASKS_SET[FEM_GT_CTX] = 1; }
static void fem_rx(void)  { NRF_GPIOTE->TASKS_CLR[FEM_GT_CTX] = 1; NRF_GPIOTE->TASKS_SET[FEM_GT_CRX] = 1; }
static void fem_off(void) { NRF_PPI->CHENCLR = 1u << FEM_PPI;
                            NRF_GPIOTE->TASKS_CLR[FEM_GT_CTX] = 1; NRF_GPIOTE->TASKS_CLR[FEM_GT_CRX] = 1; }
static void fem_turnaround_arm(void)    { NRF_PPI->CHENSET = 1u << FEM_PPI; }
static void fem_turnaround_disarm(void) { NRF_PPI->CHENCLR = 1u << FEM_PPI; }
/* SKY66112 adds ~22 dB: -4 dBm at the chip is ~+18 dBm out, under the 20 dBm EIRP limit */
#define RADIO_TXPOWER RADIO_TXPOWER_TXPOWER_Neg4dBm
#else
static void fem_init(void) {}
static void fem_tx(void) {}
static void fem_rx(void) {}
static void fem_off(void) {}
static void fem_turnaround_arm(void) {}
static void fem_turnaround_disarm(void) {}
#define RADIO_TXPOWER RADIO_TXPOWER_TXPOWER_Pos8dBm
#endif

/* ---------------- radio primitives ---------------- */
static void radio_off(void)
{
  NRF_RADIO->SHORTS = 0;
  fem_turnaround_disarm();
  NRF_RADIO->EVENTS_DISABLED = 0; NRF_RADIO->TASKS_DISABLE = 1;
  uint32_t t = us_now(); while (!NRF_RADIO->EVENTS_DISABLED && us_now() - t < 200) {}
  NRF_RADIO->EVENTS_DISABLED = 0;
  fem_off();
}
/* send pkt_tx, then listen for one answer into pkt_rx. Returns answer length (payload) or -1/-2. */
static int txrx(uint8_t ch, uint32_t base, uint8_t prefix, uint32_t wait_us)
{
  NRF_RADIO->FREQUENCY = ch;
  NRF_RADIO->BASE0 = base; NRF_RADIO->PREFIX0 = prefix;
  NRF_RADIO->TXADDRESS = 0; NRF_RADIO->RXADDRESSES = 1;
  NRF_RADIO->PACKETPTR = (uint32_t)pkt_tx;
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk | RADIO_SHORTS_DISABLED_RXEN_Msk;
  NRF_RADIO->EVENTS_DISABLED = 0; NRF_RADIO->EVENTS_ADDRESS = 0;
  fem_tx(); fem_turnaround_arm();                           /* PPI flips to RX on the turnaround */
  NRF_RADIO->TASKS_TXEN = 1;
  uint32_t t0 = us_now();
  while (!NRF_RADIO->EVENTS_ADDRESS) if (us_now() - t0 > 1500) { radio_off(); return -2; }
  NRF_RADIO->EVENTS_ADDRESS = 0;
  NRF_RADIO->PACKETPTR = (uint32_t)pkt_rx;                  /* latched at the RX START */
  while (!NRF_RADIO->EVENTS_DISABLED) if (us_now() - t0 > 2500) { radio_off(); return -2; }
  NRF_RADIO->EVENTS_DISABLED = 0;                           /* TX done, RXEN fired by the shortcut */
  fem_turnaround_disarm();                                  /* the RX end must not re-trigger it */
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk | RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
  uint32_t t1 = us_now();
  for (;;) {
    if (NRF_RADIO->EVENTS_DISABLED) break;
    uint32_t dt = us_now() - t1;
    if (NRF_RADIO->EVENTS_ADDRESS ? dt > 1600 : dt > wait_us) { radio_off(); return -1; }
  }
  NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->SHORTS = 0;
  fem_off();
  if (!NRF_RADIO->CRCSTATUS) return -3;
  return pkt_rx[0];
}

/* ---------------- one poll ---------------- */
static void poll_slot(uint8_t i)
{
  slot_t *s = &sl[i];
  uint8_t *p = pkt_tx + 1;
  int len = DML_POLL_HDR;
  p[0] = DML_POLL; p[1] = i;
  p[2] = (uint8_t)((s->last_seq & DML_F_SEQ_MASK) | (s->dseq ? DML_F_DSEQ : 0));
  p[3] = cycle; p[4] = (uint8_t)next_mask; p[5] = (uint8_t)(next_mask >> 8);
  p[6] = (uint8_t)(period_us / 50); p[7] = max_resp;
  if (s->down_len) { p[2] |= DML_F_DOWN; memcpy(p + len, s->down_buf, s->down_len); len += s->down_len; }
  pkt_tx[0] = (uint8_t)len;
  s->polls++;
  int hop = -1;
  for (int k = 0; k < DML_NHOP; k++) if (dml_hop_ch[k] == cur_ch) hop = k;
  if (hop >= 0) chs[hop].polls++;
  int n = txrx(cur_ch, net_base, (uint8_t)(DML_SLOT_PREFIX + i), 400);
  const uint8_t *r = pkt_rx + 1;
  if (n < DML_RESP_HDR || r[0] != DML_RESP || r[1] != i) {
    if (n == -3) s->crc_err++; else s->timeouts++;
    if (hop >= 0) chs[hop].lost++;
    if (s->active && ms_ticks - s->last_resp_ms > 1000) s->active = 0;
    return;
  }
  s->resp_ok++;
  s->active = 1; s->last_resp_ms = ms_ticks;
  if (i == just_paired_slot) just_paired_slot = 0xFF;     /* it joined: its pairing is over, the next one needs pairing open */
  uint8_t rd = (uint8_t)NRF_RADIO->RSSISAMPLE;
  s->rssi_last_d = rd; s->rssi_last_t = r[3];
  if (!s->rssi_min_d || rd < s->rssi_min_d) s->rssi_min_d = rd;   /* strongest (smallest -dBm) */
  if (rd > s->rssi_max_d) s->rssi_max_d = rd;                      /* weakest */
  s->rssi_d16 = s->rssi_d16 ? s->rssi_d16 - s->rssi_d16 / 16 + rd : (uint32_t)rd * 16;
  s->rssi_t16 = s->rssi_t16 ? s->rssi_t16 - s->rssi_t16 / 16 + r[3] : (uint32_t)r[3] * 16;
  if (s->have_tel) { s->t_missed += (uint8_t)(r[4] - s->last_missed); s->t_crc += (uint8_t)(r[5] - s->last_crc); }
  s->last_missed = r[4]; s->last_crc = r[5]; s->have_tel = 1;
  uint8_t dack = (r[2] & DML_F_DSEQ) ? 1 : 0;
  if (s->down_len) { if (dack == s->dseq) { s->down_len = 0; s->dseq ^= 1; } else s->down_resends++; }
  uint8_t seq = r[2] & DML_F_SEQ_MASK;
  if (seq != ((s->last_seq + 1) & DML_F_SEQ_MASK)) { s->dup++; return; }   /* duplicate / out of order */
  s->last_seq = seq;
  s->air_bytes += (uint32_t)(n - DML_RESP_HDR);
  for (int k = DML_RESP_HDR; k < n; ) {                     /* decode every frame (keeps codec state in sync) */
    uint8_t fl = r[k];
    if (fl == 0 || k + 1 + fl > n) { s->bad_frames++; break; }
    const uint8_t *f = r + k + 1;
    uint8_t g[64]; int gl = fl;
    if (f[0] >= DMC_IMU && f[0] <= DMC_LH2) { gl = dmc_decode(&s->codec, f, fl, g); f = g; if (!gl) { s->bad_frames++; k += 1 + fl; continue; } }
    else dmc_observe(&s->codec, f, fl);
    k += 1 + fl;
    if (f[0] == 'H' && gl <= 16) { memset(s->ident, 0, 16); memcpy(s->ident, f, gl); }
    if (f[0] == 'D' && gl >= 5) { s->dev_drops = (uint16_t)(f[1] | f[2] << 8); s->dev_reply_drops = (uint16_t)(f[3] | f[4] << 8); continue; }
    s->frames_up++; s->bytes_up += (uint32_t)gl;
    if (upq_used(s) + gl + 1 >= UPQ) { s->drops++; continue; }
    s->upq[s->up_tail] = (uint8_t)gl; s->up_tail = (s->up_tail + 1) & (UPQ - 1);
    for (int i = 0; i < gl; i++) { s->upq[s->up_tail] = f[i]; s->up_tail = (s->up_tail + 1) & (UPQ - 1); }
  }
}

/* ---------------- joining ---------------- */
static void slot_reset(uint8_t i)
{
  slot_t *s = &sl[i];
  s->last_seq = 7; s->dseq = 0; s->down_len = 0; s->have_tel = 0;
  dmc_reset(&s->codec);
  memset(s->ident, 0, sizeof s->ident);
  s->up_head = s->up_tail; s->frag_left = 0;
  s->active = 1; s->last_resp_ms = ms_ticks;               /* poll it right away */
}
static uint32_t t_c;                                       /* current cycle start */
static void join_beacon(void)
{
  uint8_t *p = pkt_tx + 1;
  p[0] = DML_JOIN; memcpy(p + 1, &net_base, 4);
  p[5] = ms_ticks < pairing_until_ms ||
         (assign_left && assign_slot == just_paired_slot && (int32_t)(just_paired_until_ms - ms_ticks) > 0) ? 1 : 0;
  if (assign_left) { memcpy(p + 6, assign_id, 8); p[14] = assign_slot; assign_left--; }
  else { memset(p + 6, 0, 8); p[14] = 0xFF; }
  p[15] = (uint8_t)next_mask; p[16] = (uint8_t)(next_mask >> 8); p[17] = cycle; p[18] = (uint8_t)(period_us / 50);
  uint32_t since = 0;
  if (p[14] < DML_SLOTS) {                                  /* the assigned slot's place in this cycle */
    int k = 0;
    for (int i = 0; i < p[14]; i++) k += (pairs.used >> i) & 1;
    since = us_now() - (t_c + (uint32_t)k * slot_us);
    if (since > 0xFFFF) since = 0xFFFF;
  }
  p[19] = (uint8_t)since; p[20] = (uint8_t)(since >> 8);
  pkt_tx[0] = DML_JOIN_LEN;
  int n = txrx(DML_JOIN_CHANNEL, DML_JOIN_BASE, DML_JOIN_PREFIX, 400);
  const uint8_t *r = pkt_rx + 1;
  if (n < DML_JREQ_LEN || r[0] != DML_JREQ) return;
  joins_heard++;
  const uint8_t *id = r + 1;
  const int pairing = r[9] & 1;                             /* the device is in pairing mode */
  int slot = -1;
  for (int i = 0; i < DML_SLOTS; i++) if ((pairs.used >> i) & 1 && memcmp(pairs.id[i], id, 8) == 0) slot = i;
  /* Like stock, a new pairing takes both sides: pairing open here, and the device in pairing mode
     (its pairing buttons, or a host's pair command). Known devices just reconnect. */
  if (pairing && ms_ticks < pairing_until_ms) {
    /* paired here: a new device, one already paired here (re-paired), or one moving to the chosen slot */
    int to = slot;
    if (pair_target < DML_SLOTS) to = pair_target;          /* the chosen slot: replaces what was there */
    else if (to < 0) for (int i = 0; i < DML_SLOTS && to < 0; i++) if (!((pairs.used >> i) & 1)) to = i;
    if (to < 0) return;                                     /* no free slot */
    if (to != slot) {
      if (slot >= 0) pairs.used &= ~(1u << slot);
      slot = to;
      memcpy(pairs.id[slot], id, 8); pairs.used |= 1u << slot; pairs_save();
    }
    pairing_until_ms = 0;
    just_paired_slot = (uint8_t)slot; just_paired_until_ms = ms_ticks + 35000u;
  } else if (pairing && !(slot >= 0 && slot == just_paired_slot && (int32_t)(just_paired_until_ms - ms_ticks) > 0)) {
    /* in pairing mode but not pairing here: it only joins a beacon with pairing open, so assigning it
       would show it connected at 100% loss until its pairing times out. Left alone until then. */
    return;
  }
  if (slot < 0) return;
  memcpy(assign_id, id, 8); assign_slot = (uint8_t)slot; assign_left = 5;
  slot_reset((uint8_t)slot);
}

/* ---------------- adaptive hopping ---------------- */
/* Every 2 s: drop the WORST channel over the threshold (repeatedly, down to CH_MIN_ON), bring back at
 * most one channel whose time off is over, and keep repeat offenders off longer (30 s doubling to 5 min;
 * a channel that stays clean for a while is forgiven). Dropping worst-first matters when most of the band
 * is busy (Wi-Fi): dropping in frequency order kept the worst ones once the minimum was reached. */
#define CH_MIN_ON   8
#define CH_BAD_PCT  8
/* A channel is bad when it loses clearly more than the channels around it. Loss a device has on
   every channel (weak signal, body in the way) is not the channel's fault: judged on absolute loss,
   one weak device struck clean channels until only a few were left to hop on. */
static uint8_t channels_typical_loss(uint16_t m)
{
  uint8_t v[DML_NHOP]; int n = 0;
  for (int k = 0; k < DML_NHOP; k++)
    if ((m >> k) & 1 && chs[k].polls >= 20) {
      uint8_t x = chs[k].loss_pct; int i = n++;
      while (i > 0 && v[i - 1] > x) { v[i] = v[i - 1]; i--; }
      v[i] = x;
    }
  return n ? v[n / 2] : 0;
}
static void channels_eval(void)
{
  if (ms_ticks - ch_eval_ms < 2000) return;
  ch_eval_ms = ms_ticks;
  uint16_t m = mask;
  int on = 0;
  for (int k = 0; k < DML_NHOP; k++) {
    chs[k].loss_pct = chs[k].polls ? (uint8_t)(100u * chs[k].lost / chs[k].polls) : 0;
    if ((m >> k) & 1) {
      on++;
      if (chs[k].polls >= 20 && chs[k].loss_pct <= 2 + channels_typical_loss(m) && chs[k].strikes && ms_ticks - chs[k].off_until_ms > 120000)
        chs[k].strikes--;                                    /* clean for 2 min since it came back */
    }
  }
  const unsigned bad = CH_BAD_PCT + channels_typical_loss(m);
  for (;;) {                                                 /* drop the worst bad channel, again and again */
    int worst = -1;
    for (int k = 0; k < DML_NHOP; k++)
      if ((m >> k) & 1 && chs[k].polls >= 20 && chs[k].loss_pct > bad &&
          (worst < 0 || chs[k].loss_pct > chs[worst].loss_pct)) worst = k;
    if (worst < 0 || on <= CH_MIN_ON) break;
    m &= (uint16_t)~(1u << worst); on--;
    uint32_t off = 30000u << (chs[worst].strikes < 4 ? chs[worst].strikes : 4);
    if (off > 300000u) off = 300000u;
    chs[worst].off_until_ms = ms_ticks + off;
    if (chs[worst].strikes < 8) chs[worst].strikes++;
  }
  int back = -1;                                             /* one expired channel back per round */
  for (int k = 0; k < DML_NHOP; k++)
    if (!((m >> k) & 1) && (int32_t)(ms_ticks - chs[k].off_until_ms) >= 0 &&
        (back < 0 || (int32_t)(chs[k].off_until_ms - chs[back].off_until_ms) < 0)) back = k;
  if (back >= 0 || on == 0) { if (back < 0) back = 0; m |= (uint16_t)(1u << back); chs[back].off_until_ms = ms_ticks; }
  for (int k = 0; k < DML_NHOP; k++) { chs[k].polls = 0; chs[k].lost = 0; }
  next_mask = m;
}

/* ---------------- main-loop entry points ---------------- */
void link_init(void)
{
  NRF_TIMER1->MODE = 0; NRF_TIMER1->BITMODE = 3; NRF_TIMER1->PRESCALER = 4;   /* 1 MHz */
  NRF_TIMER1->TASKS_CLEAR = 1; NRF_TIMER1->TASKS_START = 1;
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0; NRF_CLOCK->TASKS_HFCLKSTART = 1;
  uint32_t t = us_now(); while (!NRF_CLOCK->EVENTS_HFCLKSTARTED && us_now() - t < 5000) {}
  NRF_RADIO->POWER = 1;
  NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
  NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;   /* 40 us ramp-up */
  NRF_RADIO->TXPOWER = RADIO_TXPOWER;
  NRF_RADIO->PCNF0 = (8 << RADIO_PCNF0_LFLEN_Pos) | (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
  NRF_RADIO->PCNF1 = (DML_MAXLEN << RADIO_PCNF1_MAXLEN_Pos) | (4 << RADIO_PCNF1_BALEN_Pos) |
                     (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos);
  NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Two; NRF_RADIO->CRCINIT = 0xFFFF; NRF_RADIO->CRCPOLY = 0x11021;
  NRF_RADIO->INTENCLR = 0xFFFFFFFF;
  uint32_t h = NRF_FICR->DEVICEID[0] * 2654435761u ^ NRF_FICR->DEVICEID[1];
  net_base = (h & 0x7F7F7F7Fu) | 0x01010101u;               /* per-dongle network address */
  fem_init();
  pairs_load();
  for (int i = 0; i < DML_SLOTS; i++) { sl[i].last_seq = 7; sl[i].dseq = 0; }
}

void link_poll(void)
{
  if (!link_on) { radio_off(); return; }
  int n = 0;
  for (int i = 0; i < DML_SLOTS; i++) n += (pairs.used >> i) & 1;
  /* fixed TDMA plan: slot k polled at t_c + k*slot_us, then the join window */
  /* bigger responses amortise the ~450 us per-slot overhead: 6 x 120 B = 6.3 ms cycle, ~18 kB/s per device
     (72 B gave 12.9 kB/s, too little headroom for resends; queues overflowed) */
  max_resp = n <= 1 ? DML_MAXLEN : n <= 2 ? 160 : n <= 4 ? 96 : 120;
  slot_us = 450u + 4u * max_resp;
  period_us = n * slot_us + 700u;
  t_c += period_us;
  if ((int32_t)(us_now() - t_c) > (int32_t)period_us || (int32_t)(t_c - us_now()) > (int32_t)period_us)
    t_c = us_now();                                         /* fell behind / first run: restart the plan */
  wait_until(t_c);
  cycle++;
  mask = next_mask;
  cur_ch = dml_channel(cycle, mask);
  channels_eval();                                          /* sets next_mask (announced in this cycle) */
  int k = 0;
  for (uint8_t i = 0; i < DML_SLOTS; i++) {
    if (!((pairs.used >> i) & 1)) continue;
    wait_until(t_c + (uint32_t)k * slot_us);
    k++;
    if (!sl[i].active && (cycle & 15) != (i & 15)) continue; /* absent trackers: every 16th cycle */
    poll_slot(i);
  }
  if (ms_ticks - last_join_ms >= 20) {
    wait_until(t_c + (uint32_t)k * slot_us);
    last_join_ms = ms_ticks; join_beacon();
  }
}

/* 0x80 [slot][n][whole [len][frame] items]; a frame too big for one report goes as consecutive
   0x81 [slot][n][raw bytes of [len][frame]] fragments */
int link_stream_pop(uint8_t *rep, int itf)
{
  static uint8_t rrs[2];
  uint8_t rr = rrs[itf];
  for (int k = 0; k < DML_SLOTS; k++) {
    uint8_t si = (uint8_t)((rr + k) % DML_SLOTS);
    if ((si & 1) != itf) continue;               /* each slot stays on one interface: fragments in order */
    slot_t *s = &sl[si];
    int n = 0;
    if (!s->frag_left) {
      while (upq_used(s)) {
        uint8_t fl = s->upq[s->up_head];
        if (n + 1 + fl > 61) break;
        for (int i = 0; i <= fl; i++) rep[3 + n + i] = s->upq[(s->up_head + i) & (UPQ - 1)];
        s->up_head = (s->up_head + 1 + fl) & (UPQ - 1);
        n += 1 + fl;
      }
      if (!n && upq_used(s)) s->frag_left = (uint16_t)(1 + s->upq[s->up_head]);
    }
    if (s->frag_left) {
      n = s->frag_left > 61 ? 61 : s->frag_left;
      for (int i = 0; i < n; i++) rep[3 + i] = s->upq[(s->up_head + i) & (UPQ - 1)];
      s->up_head = (s->up_head + n) & (UPQ - 1);
      s->frag_left = (uint16_t)(s->frag_left - n);
      rep[0] = 0x81; rep[1] = si; rep[2] = (uint8_t)n;
      rrs[itf] = (uint8_t)(si + 1);
      return 1;
    }
    if (!n) continue;
    rep[0] = 0x80; rep[1] = si; rep[2] = (uint8_t)n;
    rrs[itf] = (uint8_t)(si + 1);
    return 1;
  }
  return 0;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

int link_cmd(const uint8_t *in, uint8_t *d, uint8_t *st)    /* 0x20..0x2F; d = 61 bytes */
{
  uint8_t i = in[2] < DML_SLOTS ? in[2] : 0;
  slot_t *s = &sl[i];
  switch (in[0]) {
  case 0x20: if (in[2] != 0xFF) link_on = in[2]; d[0] = link_on; return 1;   /* radio_off happens in link_poll */
  case 0x21: {                                              /* down: [slot][len][frame] */
    uint8_t n = in[3];
    if (n == 0 || n > 63 || s->down_len + 1u + n > sizeof s->down_buf) { *st = 2; return 1; }
    s->down_buf[s->down_len++] = n; memcpy(s->down_buf + s->down_len, in + 4, n); s->down_len += n;
    s->down_frames++;
    return 1;
  }
  case 0x23: {                                              /* per-slot stats */
    uint32_t v[15] = { s->polls, s->resp_ok, s->crc_err, s->timeouts, s->dup, s->frames_up, s->bytes_up,
                       s->drops, s->down_frames, s->down_resends, s->rssi_d16 / 16, s->rssi_t16 / 16,
                       s->t_missed, s->t_crc, ms_ticks - s->last_resp_ms };
    memcpy(d, v, sizeof v);
    return 1;
  }
  case 0x29:                                                /* live: per slot 10 B, all slots */
    for (int k = 0; k < DML_SLOTS; k++) {
      uint8_t *e = d + k * 10; slot_t *q = &sl[k];
      e[0] = (uint8_t)(((pairs.used >> k) & 1) | (q->active << 1));
      e[1] = q->rssi_last_d; e[2] = q->rssi_last_t; e[3] = q->rssi_min_d; e[4] = q->rssi_max_d;
      e[5] = cur_ch;
      put32(e + 6, ms_ticks - q->last_resp_ms);
      q->rssi_min_d = 0; q->rssi_max_d = 0;
    }
    return 1;
  case 0x2A: memcpy(d, s->ident, 16); return 1;            /* identity ('H' frame) of a slot */
  case 0x28:                                                /* codec, and the radio's own drops */
    put32(d, s->air_bytes); put32(d + 4, s->bytes_up); put32(d + 8, s->bad_frames);
    put32(d + 12, s->dev_drops); put32(d + 16, s->dev_reply_drops); return 1;
  case 0x24:                                                /* pair SECS [SLOT+1] (0/absent: first free slot) */
    pairing_until_ms = in[2] ? ms_ticks + 1000u * in[2] : 0;
    pair_target = in[3] >= 1 && in[3] <= DML_SLOTS ? (uint8_t)(in[3] - 1) : 0xFF;
    d[0] = in[2]; d[1] = pair_target; return 1;
  case 0x25:                                                /* slots: used, active, id x8 per slot */
    for (int k = 0; k < DML_SLOTS; k++) {
      d[k * 10] = (pairs.used >> k) & 1; d[k * 10 + 1] = sl[k].active;
      memcpy(d + k * 10 + 2, pairs.id[k], 8);
    }
    d[60] = ms_ticks < pairing_until_ms ? (uint8_t)(1 | (pair_target < DML_SLOTS ? (pair_target + 1) << 1 : 0)) : 0;
    return 1;
  case 0x26:                                                /* forget slot (0xFF = all) */
    if (in[2] == 0xFF) pairs.used = 0; else if (in[2] < DML_SLOTS) pairs.used &= ~(1u << in[2]);
    pairs_save();
    return 1;
  case 0x27:                                                /* channels: mask, loss % per hop channel */
    d[0] = (uint8_t)mask; d[1] = (uint8_t)(mask >> 8);
    for (int k = 0; k < DML_NHOP; k++) { d[2 + k] = dml_hop_ch[k]; d[18 + k] = chs[k].loss_pct; }
    put32(d + 34, joins_heard); put32(d + 38, net_base);
    return 1;
  }
  return 0;
}

void link_rssi_scan(uint8_t first, uint8_t *out, int n)    /* -dBm per channel, 0xFF = no sample */
{
  uint8_t was_on = link_on;
  link_on = 0; radio_off();
  for (int i = 0; i < n; i++) {
    uint8_t ch = (uint8_t)(first + i);
    if (ch > 100) { out[i] = 0xFF; continue; }
    NRF_RADIO->FREQUENCY = ch;
    NRF_RADIO->SHORTS = 0;
    NRF_RADIO->EVENTS_READY = 0; fem_rx(); NRF_RADIO->TASKS_RXEN = 1;
    uint32_t t = us_now(); while (!NRF_RADIO->EVENTS_READY && us_now() - t < 2000) {}
    uint8_t best = 0xFF;
    for (int k = 0; k < 8; k++) {
      NRF_RADIO->EVENTS_RSSIEND = 0; NRF_RADIO->TASKS_RSSISTART = 1;
      uint32_t g = 0; while (!NRF_RADIO->EVENTS_RSSIEND && ++g < 2000) {}
      uint8_t v = (uint8_t)NRF_RADIO->RSSISAMPLE;
      if (v < best) best = v;
    }
    out[i] = best;
    radio_off();
  }
  link_on = was_on;
}
