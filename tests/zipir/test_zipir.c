// Host test of the ZipIR decoder (components/lg_ir_remote/lgir_zipir.c) against synthetic code sets.
// The vectors in vectors.h come from make_vectors.py, which encodes made-up code sets from the format description
// in docs/protocol/universal-control.md and writes the waveforms they must decode to. Build and run: ./run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lgir_zipir.h"
#include "vectors.h"

static int failures, checks;

#define CHECK(cond, ...)                 \
  do {                                   \
    checks++;                            \
    if (!(cond)) {                       \
      failures++;                        \
      printf("FAIL %s: ", case_name);    \
      printf(__VA_ARGS__);               \
      printf("\n");                      \
    }                                    \
  } while (0)

static zipir_wave_t w;  // large: keep it off the stack

static void test_wave_case(const wave_case_t *c) {
  const char *case_name = c->name;
#ifndef TEST_KNOWN_BUGS
  if (c->known_bug) {
    printf("SKIP %s (known decoder issue; build with -DTEST_KNOWN_BUGS to run it)\n", c->name);
    return;
  }
#endif
  memset(&w, 0xA5, sizeof(w));  // the decoder must initialise everything it reports
  int rc = zipir_waveform(c->file, c->len, c->index, c->key, c->toggle, &w);
  CHECK(rc == c->rc, "zipir_waveform returned %d, expected %d", rc, c->rc);
  if (rc != 0 || c->rc != 0)
    return;
  CHECK(w.carrier_hz == c->carrier_hz, "carrier %u Hz, expected %u", (unsigned) w.carrier_hz,
        (unsigned) c->carrier_hz);
  CHECK(w.duty == c->duty, "duty %u%%, expected %u%%", w.duty, c->duty);
  CHECK(w.sqc == c->sqc, "sqc %u, expected %u", w.sqc, c->sqc);
  for (int j = 0; j < 3; j++) {
    CHECK(w.frame_len[j] == c->frame_len[j], "frame %c: %u durations, expected %u", "MRB"[j], w.frame_len[j],
          c->frame_len[j]);
    if (w.frame_len[j] != c->frame_len[j])
      continue;
    for (int m = 0; m < c->frame_len[j]; m++) {
      if (w.frame[j][m] != c->frame[j][m]) {
        CHECK(0, "frame %c duration %d is %d, expected %d", "MRB"[j], m, (int) w.frame[j][m], (int) c->frame[j][m]);
        break;
      }
    }
  }
  int order[3] = {-1, -1, -1};
  int n = zipir_press_order(&w, order);
  CHECK(n == c->order_n, "press order has %d frames, expected %d", n, c->order_n);
  if (n == c->order_n && n > 0)
    CHECK(order[0] == c->order[0] && order[1] == c->order[1] && order[2] == c->order[2],
          "press order %d %d %d, expected %d %d %d", order[0], order[1], order[2], c->order[0], c->order[1],
          c->order[2]);
}

static void test_codeset_case(const codeset_case_t *c) {
  char case_name[64];
  snprintf(case_name, sizeof(case_name), "zipir_codeset %s[%d]", c->file_name, c->index);
  char type = 0;
  uint16_t id = 0;
  const uint8_t *proto = NULL, *keys = NULL;
  size_t proto_len = 0, keys_len = 0;
  int rc = zipir_codeset(c->file, c->len, c->index, &type, &id, &proto, &proto_len, &keys, &keys_len);
  CHECK(rc == c->rc, "returned %d, expected %d", rc, c->rc);
  if (rc != 0 || c->rc != 0)
    return;
  CHECK(type == c->type, "type '%c', expected '%c'", type, c->type);
  CHECK(id == c->id, "id 0x%04X, expected 0x%04X", id, c->id);
  CHECK(proto_len == c->proto_len, "protocol part %zu bytes, expected %zu", proto_len, c->proto_len);
  CHECK(keys_len == c->keys_len, "key part %zu bytes, expected %zu", keys_len, c->keys_len);
  CHECK(proto >= c->file && proto + proto_len <= c->file + c->len, "protocol part outside the file");
  CHECK(keys == proto + proto_len && keys + keys_len <= c->file + c->len, "key part outside the file");
}

static void test_no_frames(void) {
  const char *case_name = "zipir_press_order with no frames";
  zipir_wave_t empty;
  memset(&empty, 0, sizeof(empty));
  int order[3];
  CHECK(zipir_press_order(&empty, order) == 0, "expected 0 frames");
}

// Robustness: every truncation and every single-bit flip of a valid file must decode to a defined result (0, 1,
// -1 or -2) without reading outside the buffer (run under AddressSanitizer by run.sh). A truncated file that still
// decodes must decode to exactly what the whole file gives.
static int same_wave(const zipir_wave_t *a, const zipir_wave_t *b) {
  if (a->carrier_hz != b->carrier_hz || a->duty != b->duty || a->sqc != b->sqc)
    return 0;
  for (int j = 0; j < 3; j++)
    if (a->frame_len[j] != b->frame_len[j] || memcmp(a->frame[j], b->frame[j], a->frame_len[j] * sizeof(int32_t)))
      return 0;
  return 1;
}

static void test_robustness(const char *case_name, const uint8_t *file, size_t len, int ncodesets) {
  static zipir_wave_t ref, got;
  uint8_t *buf = malloc(len);
  int odd = 0, mismatch = 0;
  for (size_t cut = 0; cut < len; cut++) {
    memcpy(buf, file, cut);  // exact-size copy, so ASan catches reads past the end
    uint8_t *exact = malloc(cut ? cut : 1);
    memcpy(exact, buf, cut);
    for (int i = 0; i < ncodesets; i++)
      for (int key = 0; key < 10; key++) {
        int rc = zipir_waveform(exact, cut, i, key, 0, &got);
        if (rc != 0 && rc != 1 && rc != -1 && rc != -2)
          odd++;
        if (rc == 0 && (zipir_waveform(file, len, i, key, 0, &ref) != 0 || !same_wave(&ref, &got)))
          mismatch++;
      }
    free(exact);
  }
  CHECK(odd == 0, "%d truncated decodes returned an undefined value", odd);
  CHECK(mismatch == 0, "%d truncated decodes differ from the whole file", mismatch);
  odd = 0;
  for (size_t bit = 0; bit < len * 8; bit++) {
    memcpy(buf, file, len);
    buf[bit / 8] ^= (uint8_t) (0x80 >> (bit % 8));
    for (int i = 0; i < ncodesets; i++)
      for (int key = 0; key < 10; key++)
        for (int toggle = 0; toggle < 2; toggle++) {
          int rc = zipir_waveform(buf, len, i, key, toggle, &got);
          if (rc != 0 && rc != 1 && rc != -1 && rc != -2)
            odd++;
        }
  }
  CHECK(odd == 0, "%d bit-flipped decodes returned an undefined value", odd);
  free(buf);
}


int main(void) {
  size_t nw = sizeof(WAVE_CASES) / sizeof(WAVE_CASES[0]), nc = sizeof(CODESET_CASES) / sizeof(CODESET_CASES[0]);
  for (size_t i = 0; i < nw; i++)
    test_wave_case(&WAVE_CASES[i]);
  for (size_t i = 0; i < nc; i++)
    test_codeset_case(&CODESET_CASES[i]);
  test_no_frames();
  test_robustness("robustness zf_nec", zf_nec, sizeof(zf_nec), 1);
  test_robustness("robustness zf_multi", zf_multi, sizeof(zf_multi), 3);
  printf("%s: %d checks, %d failed (%zu waveform cases, %zu code set cases)\n", failures ? "FAILED" : "OK", checks,
         failures, nw, nc);
  return failures ? 1 : 0;
}
