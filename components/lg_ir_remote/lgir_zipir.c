#include "lgir_zipir.h"
#include <string.h>

typedef struct {
  const uint8_t *d;
  size_t pos, n;  // bits
  int err;
} bits_t;

static uint32_t get(bits_t *b, int w) {
  if (b->err || b->pos > b->n || (size_t) w > b->n - b->pos) {
    b->err = 1;
    return 0;
  }
  uint32_t v = 0;
  for (int i = 0; i < w; i++, b->pos++)
    v = (v << 1) | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
  return v;
}

static int bitlen(uint32_t n) {  // bits for values 0..n, at least 1 (libqsi)
  int b = 0;
  do {
    n >>= 1;
    b++;
  } while (n);
  return b;
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t) p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]; }

int zipir_codeset(const uint8_t *f, size_t len, int index, char *type, uint16_t *id, const uint8_t **proto,
                  size_t *proto_len, const uint8_t **keys, size_t *keys_len) {
  if (len < 12)
    return -1;
  int tc = f[9];
  size_t cc_at = 10 + 5 * (size_t) tc;
  if (cc_at + 2 > len)
    return -1;
  int cc = (int) be16(f + cc_at);
  size_t rec0 = cc_at + 2, data0 = rec0 + 8 * (size_t) cc;
  if (index < 0 || index >= cc - 1 || data0 > len)
    return -1;
  const uint8_t *r = f + rec0 + 8 * (size_t) index;
  uint32_t off = be32(r + 2), plen = be16(r + 6), nxt = be32(r + 8 + 2);
  if (index == cc - 2 && nxt == 0)
    nxt = (uint32_t) (len - data0);
  if (nxt > len - data0 || off > nxt || plen > nxt - off)  // no wrap-around on 32-bit size_t
    return -1;
  *type = '?';
  for (int t = 0; t < tc; t++) {
    const uint8_t *e = f + 10 + 5 * t;
    uint32_t cnt = be16(e + 1), first = be16(e + 3);
    if ((uint32_t) index >= first && (uint32_t) index < first + cnt) {
      *type = (char) e[0];
      break;
    }
  }
  *id = (uint16_t) be16(r);
  *proto = f + data0 + off;
  *proto_len = plen;
  *keys = f + data0 + off + plen;
  *keys_len = nxt - off - plen;
  return 0;
}

int zipir_waveform(const uint8_t *file, size_t len, int index, int key, int toggle, zipir_wave_t *w) {
  char type;
  uint16_t id;
  const uint8_t *p, *k;
  size_t pl, kl;
  if (zipir_codeset(file, len, index, &type, &id, &p, &pl, &k, &kl))
    return -1;
  memset(w, 0, sizeof(*w));
  bits_t b = {p, 0, pl * 8, 0};
  get(&b, 4);  // VER
  uint32_t em = get(&b, 4), cp = get(&b, 12), dc = get(&b, 4);
  if (b.err)
    return -1;
  if (em != 0)
    return -2;
  int nct = (int) get(&b, 6) + 1, nlt = (int) get(&b, 4);
  if (nct > ZIPIR_MAX_CT)
    return -1;
  uint32_t ct[ZIPIR_MAX_CT];
  for (int i = 0; i < nct; i++)
    ct[i] = i < nlt ? get(&b, 16) * 8 : get(&b, 12) * 4;
  // Repetition sequences: lengths, then CT indices. Remember where each starts in the bit stream.
  int nrs = (int) get(&b, 6) + 1, lrs = (int) get(&b, 2);
  uint16_t srs[64];
  for (int i = 0; i < nrs; i++)
    srs[i] = (uint16_t) (get(&b, lrs + 3) + 1);
  int wct = bitlen((uint32_t) nct - 1);
  size_t rs_at[64];
  for (int i = 0; i < nrs; i++) {
    rs_at[i] = b.pos;
    b.pos += (size_t) srs[i] * wct;
  }
  if (b.pos > b.n)
    return -1;
  // Waveform substitutions: lengths, then RS indices.
  int nws = (int) get(&b, 10) + 1, lws = (int) get(&b, 3);
  if (b.err)
    return -1;
  // WS lengths are needed to find WS starts; read them into a small table (NWS <= 1024).
  static uint16_t sws[1024];
  for (int i = 0; i < nws; i++)
    sws[i] = (uint16_t) (get(&b, lws + 3) + 1);
  int wrs = bitlen((uint32_t) nrs - 1);
  size_t ws0 = b.pos;
  if (b.err)
    return -1;
  // Key part.
  bits_t kb = {k, 0, kl * 8, 0};
  int bws = (int) get(&kb, 4), sqc = (int) get(&kb, 4) + 1;
  if (kb.err)
    return -1;
  w->sqc = (uint8_t) sqc;
  int want = key * sqc + (toggle % sqc), found = 0;
  int fws[3] = {-1, -1, -1};
  for (int n = 0; kb.n - kb.pos >= 3; n++) {
    uint32_t wsp = get(&kb, 3);
    int idx[3] = {-1, -1, -1}, bad = 0;
    for (int j = 0; j < 3; j++) {
      if (wsp & (4u >> j)) {
        uint32_t i = get(&kb, bws);
        if (kb.err || i >= (uint32_t) nws) {
          bad = 1;
          break;
        }
        idx[j] = (int) i;
      }
    }
    if (bad)
      break;
    if (n == key * sqc && wsp)  // toggle 0, the fallback when the wanted toggle state is empty
      memcpy(fws, idx, sizeof(fws)), found = 1;
    if (n == want && wsp) {
      memcpy(fws, idx, sizeof(fws));
      found = 1;
      break;
    }
    if (n > want)
      break;
  }
  if (!found)
    return 1;
  w->carrier_hz = cp ? (uint32_t) (100000000u / cp) : 0;
  w->duty = (uint8_t) (dc * 10);
  for (int j = 0; j < 3; j++) {
    if (fws[j] < 0)
      continue;
    size_t at = ws0;  // start of WS fws[j]
    for (int i = 0; i < fws[j]; i++)
      at += (size_t) sws[i] * wrs;
    bits_t wb = {p, at, pl * 8, 0};
    int m = 0;
    for (int s = 0; s < sws[fws[j]]; s++) {
      uint32_t rs = get(&wb, wrs);
      if (wb.err || rs >= (uint32_t) nrs)
        return -1;
      bits_t rb = {p, rs_at[rs], pl * 8, 0};
      for (int t = 0; t < srs[rs]; t++) {
        uint32_t c = get(&rb, wct);
        if (rb.err || c >= (uint32_t) nct || m >= ZIPIR_MAX_FRAME)
          return -1;
        w->frame[j][m] = (m % 2 == 0) ? (int32_t) ct[c] : -(int32_t) ct[c];
        m++;
      }
    }
    w->frame_len[j] = (uint16_t) m;
  }
  return 0;
}

int zipir_press_order(const zipir_wave_t *w, int o[3]) {
  int a = w->frame_len[0] > 0, r = w->frame_len[1] > 0, c = w->frame_len[2] > 0;
  if (a && r && c) { o[0] = 0; o[1] = 1; o[2] = 2; }
  else if (a && r) { o[0] = 0; o[1] = 1; o[2] = 1; }
  else if (a) { o[0] = o[1] = o[2] = 0; }
  else if (r) { o[0] = o[1] = o[2] = 1; }
  else if (c) { o[0] = o[1] = o[2] = 2; }
  else return 0;
  return 3;
}
