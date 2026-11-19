/* Lossless air compression of the tracker's two big frame types (used by radio/ and dongle/).
 *
 *  'p' + 17 B IMU (acc[3], gyro[3] s16, t u32, seq u8)  ->  'j' [flags] 6 x zz(d16) [t delta] [seq?]
 *  'q' + n x 8 B light records                           ->  'k' n x ([b0][b1][A][zz(dt)])
 *     record: b0 = sensor (0xFF = sync), b1; non-sync: A = u16 @2, t = u32 @4; sync: t = u32 @2, A = u16 @6
 *  numbers are LEB128 varints, zz = zigzag of the delta from the previous value.
 *
 * The encoder and decoder each keep "previous value" state. Both see every 'p'/'q' frame in the same
 * order (the link delivers in order and never drops inside the link), and both update their state from
 * the ORIGINAL frame whether it went over the air encoded or raw. Reset both on (re)join.
 * Decoding returns the exact original bytes, including any bytes the host ignores. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct { int16_t imu[6]; uint32_t imu_t, light_t; uint8_t seq; } ohc_state_t;

static inline int ohc_put(uint8_t *o, uint32_t v)
{
  int n = 0;
  while (v >= 0x80) { o[n++] = (uint8_t)(v | 0x80); v >>= 7; }
  o[n++] = (uint8_t)v;
  return n;
}
static inline int ohc_get(const uint8_t *i, int len, int *pos, uint32_t *v)
{
  uint32_t r = 0; int sh = 0;
  while (*pos < len && sh < 35) {
    uint8_t b = i[(*pos)++];
    r |= (uint32_t)(b & 0x7F) << sh; sh += 7;
    if (!(b & 0x80)) { *v = r; return 1; }
  }
  return 0;
}
static inline uint32_t ohc_zz(int32_t v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
static inline int32_t ohc_unzz(uint32_t v) { return (int32_t)(v >> 1) ^ -(int32_t)(v & 1); }
static inline uint16_t ohc_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t ohc_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void ohc_w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void ohc_w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* update the state from an original frame (called by both sides for every 'p'/'q' frame) */
static inline void ohc_observe(ohc_state_t *s, const uint8_t *f, int n)
{
  if (f[0] == 'p' && n == 18) {
    for (int k = 0; k < 6; k++) s->imu[k] = (int16_t)ohc_u16(f + 1 + 2 * k);
    s->imu_t = ohc_u32(f + 13); s->seq = f[17];
  } else if (f[0] == 'q' && n >= 1 && (n - 1) % 8 == 0) {
    for (int r = 1; r + 8 <= n; r += 8) s->light_t = ohc_u32(f + r + (f[r] == 0xFF ? 2 : 4));
  }
}

/* encode f (n bytes) into o; returns the encoded length, or 0 = send f as is. Updates the state. */
static inline int ohc_encode(ohc_state_t *s, const uint8_t *f, int n, uint8_t *o)
{
  int m = 0;
  if (f[0] == 'p' && n == 18) {
    o[m++] = 'j';
    uint8_t seq_next = (uint8_t)(s->seq + 1);
    o[m++] = f[17] == seq_next ? 1 : 0;
    for (int k = 0; k < 6; k++) m += ohc_put(o + m, ohc_zz((int16_t)(ohc_u16(f + 1 + 2 * k) - (uint16_t)s->imu[k])));
    m += ohc_put(o + m, ohc_zz((int32_t)(ohc_u32(f + 13) - s->imu_t)));
    if (!(o[1] & 1)) o[m++] = f[17];
  } else if (f[0] == 'q' && n > 1 && (n - 1) % 8 == 0) {
    o[m++] = 'k';
    uint32_t prev = s->light_t;
    for (int r = 1; r + 8 <= n; r += 8) {
      const uint8_t *e = f + r;
      int sync = e[0] == 0xFF;
      uint16_t a = ohc_u16(e + (sync ? 6 : 2));
      uint32_t t = ohc_u32(e + (sync ? 2 : 4));
      o[m++] = e[0]; o[m++] = e[1];
      m += ohc_put(o + m, a);
      m += ohc_put(o + m, ohc_zz((int32_t)(t - prev)));
      prev = t;
    }
  }
  ohc_observe(s, f, n);
  return m && m < n ? m : 0;
}

/* decode an encoded frame ('j'/'k') into the original; returns its length or 0 on a malformed frame.
 * Updates the state. Raw 'p'/'q' frames must be passed to ohc_observe instead. */
static inline int ohc_decode(ohc_state_t *s, const uint8_t *i, int n, uint8_t *f)
{
  int pos = 1, m = 0;
  uint32_t v;
  if (i[0] == 'j') {
    if (n < 2) return 0;
    uint8_t flags = i[pos++];
    f[m++] = 'p';
    for (int k = 0; k < 6; k++) {
      if (!ohc_get(i, n, &pos, &v)) return 0;
      ohc_w16(f + m, (uint16_t)((uint16_t)s->imu[k] + (uint16_t)ohc_unzz(v))); m += 2;
    }
    if (!ohc_get(i, n, &pos, &v)) return 0;
    ohc_w32(f + m, s->imu_t + (uint32_t)ohc_unzz(v)); m += 4;
    if (flags & 1) f[m++] = (uint8_t)(s->seq + 1);
    else { if (pos >= n) return 0; f[m++] = i[pos++]; }
    if (pos != n) return 0;
  } else if (i[0] == 'k') {
    f[m++] = 'q';
    uint32_t prev = s->light_t;
    while (pos < n) {
      if (pos + 2 > n || m + 8 > 63) return 0;
      uint8_t *e = f + m;
      e[0] = i[pos++]; e[1] = i[pos++];
      int sync = e[0] == 0xFF;
      if (!ohc_get(i, n, &pos, &v)) return 0;
      ohc_w16(e + (sync ? 6 : 2), (uint16_t)v);
      if (!ohc_get(i, n, &pos, &v)) return 0;
      prev += (uint32_t)ohc_unzz(v);
      ohc_w32(e + (sync ? 2 : 4), prev);
      m += 8;
    }
  } else return 0;
  ohc_observe(s, f, m);
  return m;
}
