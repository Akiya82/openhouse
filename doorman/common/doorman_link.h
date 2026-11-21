/* openhouse link v1: tracker radio (nRF51822) <-> dongle (nRF52840). Shared by both firmwares.
 *
 * Air: Nordic proprietary 2 Mbit, 8-bit preamble, 4+1 byte address, 8-bit length, CRC16 (0x11021,
 * init 0xFFFF), no whitening.
 *
 * Network: the dongle leads. A cycle = one POLL per active slot (each answered by that tracker's RESP),
 * all on one channel; every 20 ms the dongle also sends a JOIN beacon on the join channel. Cycles hop
 * over 16 channels: channel = hop_ch[allowed[cycle % n_allowed]], allowed = set bits of the mask the
 * previous POLL announced. Each slot has its own address (BASE1 = per-dongle network base, prefix
 * DML_SLOT_PREFIX + slot), so a tracker's radio only ever hears polls for its own slot and can answer
 * with a hardware turnaround (END->DISABLE->TXEN shortcut, ~150 us) using a RESP prepared beforehand.
 *
 * Timing: fixed cycle period P; the k-th scheduled slot is polled at cycle_start + k * slot_time, so a
 * tracker that misses polls keeps hopping on its own clock (cycle = last heard + round(dt / P)).
 *
 * POLL  [type][slot][flags][cycle][mask lo][mask hi][period/50us][max resp len][downlink frames...]
 *        flags: bits 0-2 ack (last in-order RESP seq received), bit 3 dseq, bit 4 has downlink
 *        mask: allowed hop channels for the NEXT cycle
 * RESP  [type][slot][flags][rssi][missed][crc_err][uplink frames...]
 *        flags: bits 0-2 seq (go-back-N, window 2), bit 3 dack (last downlink dseq received)
 *        rssi: -dBm of a recent poll; missed/crc_err: tracker counters mod 256
 * JOIN  [type][net base x4][flags][assign id x8][assign slot][mask lo][mask hi][cycle][period/50us]
 *        [us since the assigned slot's poll, u16]           (join address, join channel)
 *        flags bit0: pairing open; assign id = tracker DEVICEID the slot is handed to (0 = none)
 *        the hop fields let a new tracker follow the hops at once (the first hop channel may be off)
 * JREQ  [type][tracker id x8][flags]                               (answer to JOIN)
 * frames = repeated [len][len bytes]: exactly the frames the tracker MCU and its radio exchange over SPI.
 */
#pragma once
#include <stdint.h>

#ifdef DML_IDX                                /* Valve nRF52 radios: SteamVR reads this as a unix time; */
#define DML_VERSION        0x6A000020u        /* keep it newer than Valve's (0x5D27B2A9) so nothing "updates" it back */
#else
#define DML_VERSION        0x0A000011u
#endif        /* radio fw version reported for 'X' (HTC: 0x576B0104) */
#define DML_AIR_UPD        0xF0u              /* downlink/uplink frame op: radio's own updater */
#define DML_JOIN_CHANNEL   80                 /* 2480 MHz */
#define DML_JOIN_BASE      0x4F484A4Eu        /* "OHJN" */
#define DML_JOIN_PREFIX    0x5Au
#define DML_SLOT_PREFIX    0xA0u
#define DML_MAXLEN         250
#define DML_SLOTS          8
#define DML_NHOP           16
static const uint8_t dml_hop_ch[DML_NHOP] = { 4, 9, 14, 19, 24, 29, 34, 39, 44, 49, 54, 59, 64, 69, 74, 79 };

enum { DML_POLL = 0x01, DML_RESP = 0x02, DML_JOIN = 0x03, DML_JREQ = 0x04 };
#define DML_POLL_HDR       8
#define DML_RESP_HDR       6
#define DML_JOIN_LEN       21
/* Tagged feature reads: the host sends [0xC5][tag][report id]; the radio asks the main MCU with
   ['P'][report id] and sends the answer back as [0xC4][tag][answer without the 'P']. The main MCU's
   answers carry no id, so without the tag a late answer was taken for the next request. */
#define DML_TAGGED_GET     0xC5
#define DML_TAGGED_REPLY   0xC4
#define DML_JREQ_LEN       10
#define DML_F_SEQ_MASK     0x07
#define DML_F_DSEQ         0x08
#define DML_F_DOWN         0x10

/* channel for a cycle: the (cycle % n)-th allowed hop channel */
static inline uint8_t dml_channel(uint8_t cycle, uint16_t mask)
{
  int n = 0;
  for (int i = 0; i < DML_NHOP; i++) n += (mask >> i) & 1;
  if (n == 0) return dml_hop_ch[0];
  int k = cycle % n;
  for (int i = 0; i < DML_NHOP; i++)
    if ((mask >> i) & 1) { if (k == 0) return dml_hop_ch[i]; k--; }
  return dml_hop_ch[0];
}
