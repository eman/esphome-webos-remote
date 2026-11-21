#pragma once
#include "esphome/core/defines.h"
#ifdef USE_LG_MAGIC_REMOTE_VOICE  // set by the lg_magic_remote speaker platform (speaker.py)
// Voice input: plays 16 kHz mono PCM into the TV's voice recognition as a Magic Remote microphone would.
// The PCM is encoded as mSBC (sbc_*.c, from ESP-IDF) and sent two frames (240 samples, 15 ms) per FE report.
#include <memory>
#include "esphome/core/component.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/ring_buffer/ring_buffer.h"

extern "C" {
#include "sbc_encoder.h"
}

namespace esphome {
namespace lg_magic_remote {

class LGMagicRemote;

class LGVoiceSpeaker : public Component, public speaker::Speaker {
 public:
  void set_parent(LGMagicRemote *p) { this->parent_ = p; }
  void set_buffer_ms(uint32_t ms) { this->buffer_ms_ = ms; }
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  size_t play(const uint8_t *data, size_t length) override;
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;

 protected:
  void encode_packet_(const int16_t *pcm240, uint8_t *out120);
  void end_(const char *why);

  LGMagicRemote *parent_{nullptr};
  uint32_t buffer_ms_{500};
  std::unique_ptr<ring_buffer::RingBuffer> ring_;
  SBC_ENC_PARAMS sbc_{};
  uint8_t h2_{0};          // mSBC H2 sequence (0..3)
  bool finishing_{false};  // play everything buffered, then end the session
  bool warned_format_{false};
  uint32_t next_ms_{0};    // when the next 15 ms packet is due
  uint32_t packets_{0};
  bool begun_{false};  // voice_begin() called for this session
  int lead_{0}, tail_{0};
};

}  // namespace lg_magic_remote
}  // namespace esphome

#endif  // USE_LG_MAGIC_REMOTE_VOICE
