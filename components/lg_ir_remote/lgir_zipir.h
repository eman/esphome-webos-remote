#pragma once
// UEI ZipIR code-set decoder. LG TVs send universal-control IR codes to the Magic Remote in this format (F9 0x61 /
// 0x62), for the remote to transmit (docs/protocol/universal-control.md). Plain C with no ESPHome dependencies, so
// tests/zipir/ builds it on the host.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIPIR_MAX_CT 64
#define ZIPIR_MAX_FRAME 512  // durations per frame

typedef struct {
  uint32_t carrier_hz;  // 0 = baseband
  uint8_t duty;         // percent
  // Up to three frames (M main, R repeat, B third): durations in microseconds, + = mark (carrier on), - = space.
  int32_t frame[3][ZIPIR_MAX_FRAME];
  uint16_t frame_len[3];
  uint8_t sqc;  // toggle states of this key's code set
} zipir_wave_t;

// Code-set record of a ZipIR file: index-th entry (0-based). Returns 0 and fills type letter, id, and pointers to
// its protocol and key parts; nonzero if the file is malformed or index is out of range.
int zipir_codeset(const uint8_t *file, size_t len, int index, char *type, uint16_t *id, const uint8_t **proto,
                  size_t *proto_len, const uint8_t **keys, size_t *keys_len);

// Waveform of function `key` (toggle state `toggle`) of the index-th code set. Returns 0 on success, 1 if the key
// isn't in the code set, negative on a decode error (-2: EM other than 0, not supported).
int zipir_waveform(const uint8_t *file, size_t len, int index, int key, int toggle, zipir_wave_t *out);

// Frame order for one press, as the TV's software blaster sends it: M R R, M R B, or the only frame three times.
// Fills order[0..2] with frame numbers (0 M, 1 R, 2 B) and returns how many (0 if the key has no frames).
int zipir_press_order(const zipir_wave_t *w, int order[3]);

#ifdef __cplusplus
}
#endif
