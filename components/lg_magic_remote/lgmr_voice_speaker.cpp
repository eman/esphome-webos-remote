#include "esphome/core/defines.h"
#ifdef USE_LG_MAGIC_REMOTE_VOICE  // set by the lg_magic_remote speaker platform (speaker.py)
#include "lgmr_voice_speaker.h"
#include "lg_magic_remote.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>
#include "esp_heap_caps.h"

namespace esphome {
namespace lg_magic_remote {

static const char *const TAG = "lgmr.voice";
static const uint32_t PACKET_MS = 15;       // 2 mSBC frames = 240 samples at 16 kHz
static const size_t PACKET_BYTES = 240 * 2;  // 16-bit mono PCM per packet
static const int LEAD_PACKETS = 10;   // 150 ms of silence first
static const int TAIL_PACKETS = 27;   // ~0.4 s of silence before releasing the key
static const uint8_t H2[4] = {0x08, 0x38, 0xc8, 0xf8};  // mSBC (HFP) H2 sequence bytes, after 0x01

void LGVoiceSpeaker::setup() {
  this->ring_ = ring_buffer::RingBuffer::create(this->buffer_ms_ * 32, ring_buffer::RingBuffer::MemoryPreference::INTERNAL_FIRST);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "no memory for a %u ms voice buffer", (unsigned) this->buffer_ms_);
    this->mark_failed();
    return;
  }
  // The media pipeline must hand us 16 kHz mono 16-bit (speaker media_player: format WAV, sample_rate 16000,
  // num_channels 1).
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, 16000);
}

void LGVoiceSpeaker::start() {
  if (this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STARTING)
    return;
  this->ring_->reset();
  memset(&this->sbc_, 0, sizeof(this->sbc_));
  this->sbc_.sbc_mode = SBC_MODE_MSBC;  // 16 kHz, mono, 8 subbands, 15 blocks, loudness, bitpool 26: 57-byte frames
  this->sbc_.u8NumPacketToEncode = 1;
  SBC_Encoder_Init(&this->sbc_);
  this->h2_ = 0;
  this->finishing_ = false;
  this->warned_format_ = false;
  this->packets_ = 0;
  this->begun_ = false;
  this->state_ = speaker::STATE_STARTING;
  ESP_LOGI(TAG, "voice audio starting; free heap %u, largest block %u", (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

size_t LGVoiceSpeaker::play(const uint8_t *data, size_t length) { return this->play(data, length, 0); }

size_t LGVoiceSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->ring_ == nullptr)
    return 0;
  if (this->state_ == speaker::STATE_STOPPED)
    this->start();
  auto &info = this->audio_stream_info_;
  if ((info.get_sample_rate() != 16000 || info.get_channels() != 1 || info.get_bits_per_sample() != 16) &&
      !this->warned_format_) {
    ESP_LOGE(TAG, "audio is %u Hz, %u ch, %u bit; voice needs 16000 Hz mono 16-bit (set the media pipeline's "
                  "sample_rate: 16000, num_channels: 1, format: WAV)",
             (unsigned) info.get_sample_rate(), info.get_channels(), info.get_bits_per_sample());
    this->warned_format_ = true;
  }
  this->finishing_ = false;
  return this->ring_->write_without_replacement(data, length, ticks_to_wait);
}

void LGVoiceSpeaker::finish() {
  if (this->state_ != speaker::STATE_STOPPED)
    this->finishing_ = true;
}

void LGVoiceSpeaker::stop() {
  if (this->state_ != speaker::STATE_STOPPED)
    this->end_("stopped");
}

bool LGVoiceSpeaker::has_buffered_data() const { return this->ring_ != nullptr && this->ring_->available() > 0; }

void LGVoiceSpeaker::encode_packet_(const int16_t *pcm, uint8_t *out) {
  // 120 bytes: two 60-byte mSBC packets, each 01 <H2> + 57-byte frame (starts with the AD sync byte) + 1 pad byte.
  // The TV checks bytes 0 and 2 of each half (01 .. AD) and decodes 58 bytes from offset 2 (docs/protocol/voice.md).
  for (int half = 0; half < 2; half++) {
    uint8_t *o = out + half * 60;
    memcpy(this->sbc_.as16PcmBuffer, pcm + half * 120, 120 * sizeof(int16_t));
    this->sbc_.pu8Packet = o + 2;
    SBC_Encoder(&this->sbc_);
    o[0] = 0x01;
    o[1] = H2[this->h2_];
    this->h2_ = (this->h2_ + 1) & 3;
    o[59] = 0x00;
  }
}

void LGVoiceSpeaker::end_(const char *why) {
  ESP_LOGI(TAG, "voice session ended (%s) after %u packets (%.2f s)", why, (unsigned) this->packets_,
           this->packets_ * PACKET_MS / 1000.0f);
  if (this->begun_)
    this->parent_->voice_end();
  this->begun_ = false;
  if (this->ring_ != nullptr)
    this->ring_->reset();
  this->finishing_ = false;
  this->state_ = speaker::STATE_STOPPED;
}

void LGVoiceSpeaker::loop() {
  if (this->state_ == speaker::STATE_STARTING) {
    if (!this->begun_) {
      // Wait for some audio before holding the Voice key, so the microphone doesn't open on an empty buffer.
      if (this->ring_->available() < PACKET_BYTES * 4 && !this->finishing_)
        return;
      if (!this->parent_->voice_begin()) {
        ESP_LOGW(TAG, "TV not connected over Bluetooth: voice dropped");
        this->end_("not connected");
        return;
      }
      this->begun_ = true;
      return;
    }
    if (this->parent_->voice_idle()) {  // the TV never opened the microphone
      this->end_("TV didn't open the microphone");
      return;
    }
    if (!this->parent_->voice_streaming())
      return;  // audio keeps buffering (the media pipeline waits while the buffer is full)
    ESP_LOGI(TAG, "voice session started");
    this->state_ = speaker::STATE_RUNNING;
    this->next_ms_ = millis();
    this->lead_ = LEAD_PACKETS;
    this->tail_ = 0;
  }
  if (this->state_ != speaker::STATE_RUNNING)
    return;
  uint32_t now = millis();
  // Real-time pacing: one packet per 15 ms, catching up at most 4 packets if the main loop was late.
  for (int n = 0; n < 4 && (int32_t) (now - this->next_ms_) >= 0; n++) {
    int16_t pcm[240];
    if (this->lead_ > 0) {
      // Lead with silence: the real remote drops its first buffers, and the recogniser gets a clean start.
      memset(pcm, 0, sizeof(pcm));
      this->lead_--;
    } else {
      size_t got = this->ring_->read(pcm, PACKET_BYTES, 0);
      if (got < PACKET_BYTES) {
        if (!this->finishing_ && got == 0) {
          // Underrun mid-stream: send silence so the TV's 1 s gap timeout doesn't end the session.
          memset(pcm, 0, sizeof(pcm));
        } else {
          memset(((uint8_t *) pcm) + got, 0, PACKET_BYTES - got);
          if (got == 0 && this->finishing_ && ++this->tail_ > TAIL_PACKETS) {
            this->end_("finished");
            return;
          }
        }
      }
    }
    uint8_t payload[120];
    this->encode_packet_(pcm, payload);
    if (!this->parent_->voice_packet(payload)) {
      this->end_("the TV closed the microphone or the link dropped");
      return;
    }
    this->packets_++;
    this->next_ms_ += PACKET_MS;
  }
  if ((int32_t) (now - this->next_ms_) > 100)
    this->next_ms_ = now;  // fell far behind (blocking elsewhere): don't burst
}

}  // namespace lg_magic_remote
}  // namespace esphome

#endif  // USE_LG_MAGIC_REMOTE_VOICE
