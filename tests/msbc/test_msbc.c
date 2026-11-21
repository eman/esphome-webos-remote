// Host test for the mSBC encoder used for voice (components/lg_magic_remote/sbc_*). Encodes one second of
// synthesized audio (a 1 kHz tone, then a sweep) and checks the frame format the TV requires, plus a hash of the
// whole output so an encoder change shows up. Run: ./run.sh
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sbc_encoder.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
// FNV-1a of all frames from the encoder as of 2026-11-20; update only after checking a change by ear
// (ffmpeg -f sbc -i out.msbc out.wav).
#define GOLDEN_HASH 0x628aea03u

int main(int argc, char **argv) {
  static SBC_ENC_PARAMS p;
  memset(&p, 0, sizeof p);
  p.sbc_mode = SBC_MODE_MSBC;  // Init sets 16 kHz, mono, 8 subbands, 15 blocks, loudness, bitpool 26
  p.u8NumPacketToEncode = 1;
  SBC_Encoder_Init(&p);
  FILE *out = argc > 1 ? fopen(argv[1], "wb") : NULL;
  uint32_t hash = 2166136261u;
  int frames = 0, bad = 0;
  double phase = 0;
  for (int f = 0; f < 16000 / 120; f++) {  // 120 samples = one mSBC frame = 7.5 ms
    int16_t pcm[120];
    for (int i = 0; i < 120; i++) {
      int n = f * 120 + i;
      double hz = n < 8000 ? 1000.0 : 200.0 + 3000.0 * (n - 8000) / 8000.0;
      phase += 2 * M_PI * hz / 16000.0;
      pcm[i] = (int16_t) lround(12000 * sin(phase));
    }
    memcpy(p.as16PcmBuffer, pcm, sizeof pcm);
    uint8_t frame[64];
    p.pu8Packet = frame;
    SBC_Encoder(&p);
    // mSBC: 57-byte frames, sync 0xAD, then two reserved zero bytes.
    if (p.u16PacketLength != 57 || frame[0] != 0xAD || frame[1] != 0 || frame[2] != 0) {
      if (bad++ < 3)
        fprintf(stderr, "frame %d: length %u, header %02x %02x %02x\n", f, p.u16PacketLength, frame[0], frame[1],
                frame[2]);
    }
    for (int i = 0; i < p.u16PacketLength; i++)
      hash = (hash ^ frame[i]) * 16777619u;
    if (out)
      fwrite(frame, 1, p.u16PacketLength, out);
    frames++;
  }
  if (out)
    fclose(out);
  printf("%d frames, %d malformed, hash %08x\n", frames, bad, hash);
  if (bad)
    return 1;
  if (GOLDEN_HASH && hash != GOLDEN_HASH) {
    printf("hash differs from GOLDEN_HASH %08x\n", GOLDEN_HASH);
    return 1;
  }
  return 0;
}
