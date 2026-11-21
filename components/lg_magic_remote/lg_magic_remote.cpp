#include "lg_magic_remote.h"

#include <cmath>
#include "lgmr_ble.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef LGMR_SERIAL_COMMANDS
#include <driver/usb_serial_jtag.h>
#endif
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace esphome::lg_magic_remote {

static const char *const TAG = "lgmr";
static LGMagicRemote *instance = nullptr;

struct KeyName {
  const char *name;
  uint16_t code;
};
// Magic Remote key codes. Rule (2026-11-20): 0x8000 | the LG IR command byte bit-reversed (IR 0x20DF22DD OK ->
// 0x22 -> 0x44 -> 0x8044). All below verified on the C9 (2026-11-20) in a webOS test app, by the TV's front app, or (settings,
// guide, more, picture, list: system panels) by the panel taking over the arrow keys. Power is 0x0008 (no 0x8000
// prefix; 0x8008, 0x80C5 and a held 0x8008 do nothing); power on from standby is the Bluetooth wake burst (wake_tv).
// HDMI 1-3 have no Bluetooth codes (0x80CE/0x80CC/0x80E9 do nothing), so they are IR. Digits are 0x8010 + n
// (press("0".."9")). tests/check_keys.py checks these against lg_ir_remote's IR codes (docs/protocol/ir-keys.md).
static const KeyName KEYS[] = {
    {"ok", 0x8044},       {"up", 0x8040},     {"down", 0x8041},   {"left", 0x8007},   {"right", 0x8006},
    {"back", 0x8028},     {"home", 0x807C},   {"settings", 0x8043}, {"more", 0x8045}, {"volup", 0x8002},
    {"voldown", 0x8003},  {"mute", 0x8009},   {"chup", 0x8000},   {"chdown", 0x8001}, {"red", 0x8072},
    {"green", 0x8071},    {"yellow", 0x8063}, {"blue", 0x8061},   {"guide", 0x80AB},  {"input", 0x800B},
    {"play", 0x80B0},     {"pause", 0x80BA},  {"stop", 0x80B1},   {"rewind", 0x808F}, {"ff", 0x808E},
    {"exit", 0x805B},     {"info", 0x80AA},   {"picture", 0x804D}, {"sound", 0x8052}, {"subtitles", 0x8039},
    {"list", 0x8053},     {"netflix", 0x8056}, {"prime", 0x805C},
    // From the TV's keymap (docs/protocol/bluetooth.md), each checked on a 2019 C9 (2026-11-20). Power is the only code without the 0x8000 prefix; it turns the TV off
    // (standby has no Bluetooth link, so Power on stays IR).
    {"power", 0x0008},    {"launcher", 0x8042}, {"search", 0x8078}, {"recent", 0x80B5}, {"livetv", 0x800F},
    {"record", 0x80BD},   {"userguide", 0x807A}, {"screenremote", 0x80DE}, {"advsettings", 0x80B8},
    {"inputhub", 0x80B9}, {"aspect", 0x8079},   {"voice", 0x808B},  {"livemenu", 0x809E}, {"dash", 0x804C},
    {"prevtrack", 0x80B2}, {"nexttrack", 0x80B3},
    // Not here on purpose: set-top power 0x8099 and set-top menu 0x8098 open the "control your set-top box with the
    // Magic Remote" dialog, which takes every key until No is chosen. Most others in the keymap are regional
    // (Japanese broadcast keys and the like) and only reach apps as unused key codes.
};

// Motion sensor configuration ("SCD") the TV requests with F9 17 and uses to create its pointer device.
// Byte-identical from two MR20GA remotes (reporting as LGE MR22 and LGE MR23, firmware 02 02 28 34 23),
// posted in github.com/brainrom/lg-magic issue #9: header 00 04 01 05 00, then float32 LE nominal constants (accel full
// scale 78.4 m/s^2 = 8 g, gyro LSB 0.0010653 rad/s = 2000 deg/s / 32768, axis remap). Not per-unit.
static const uint8_t SCD[260] = {
    0x00, 0x04, 0x01, 0x05, 0x00, 0xfb, 0xfa, 0xe1, 0xe7, 0x0a, 0xd7, 0x23, 0x3c, 0x0a, 0xd7, 0xa3,
    0x3c, 0xcd, 0xcc, 0x9c, 0x28, 0xcd, 0xcc, 0x9c, 0xc2, 0x00, 0x00, 0x00, 0x00, 0xcd, 0xcc, 0x9c,
    0x42, 0xcd, 0xcc, 0x9c, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xcd, 0xcc, 0x9c, 0x42, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0xe0, 0x2d, 0x10, 0x41, 0xe0, 0x2d, 0x10,
    0x41, 0xe0, 0x2d, 0x10, 0x41, 0x00, 0x00, 0x80, 0xbf, 0x6f, 0xa1, 0x8b, 0x20, 0x6f, 0xa1, 0x8b,
    0xba, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b, 0x3a, 0x6f, 0xa1, 0x8b, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b, 0x3a, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b, 0x3a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b, 0x3a, 0x00, 0x00, 0x80, 0x3f, 0x6f, 0xa1, 0x8b,
    0x3a, 0x0a, 0x1c, 0x01, 0x40, 0xc2, 0xb8, 0xb2, 0x3e, 0x5d, 0xc4, 0x27, 0x40, 0x5d, 0xc4, 0x27,
    0x40, 0x5d, 0xc4, 0x27, 0x40, 0x00, 0x48, 0x46, 0xc1, 0x64, 0x43, 0xc4, 0x3d, 0xa9, 0x58, 0xd1,
    0x39, 0xe4, 0xba, 0xac};

static std::string hex(const uint8_t *d, size_t n) {
  std::string s;
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), i ? " %02x" : "%02x", d[i]);
    s += b;
  }
  return s;
}

static std::vector<uint8_t> parse_hex(const std::string &s) {
  std::vector<uint8_t> out;
  int nib = -1;
  for (char c : s) {
    int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0)
      continue;
    if (nib < 0) {
      nib = v;
    } else {
      out.push_back((uint8_t) (nib << 4 | v));
      nib = -1;
    }
  }
  return out;
}

}  // namespace esphome::lg_magic_remote

// The last Bluetooth log lines with their uptime, kept so events from before the Wi-Fi log stream attaches
// (a reconnect right after boot) can be read later (dump_ble_log, the "Dump Bluetooth log" button).
static std::mutex ble_log_lock;
static std::deque<std::string> ble_log;

extern "C" void lgmr_logf(int level, const char *fmt, ...) {
  char line[200];
  int n = snprintf(line, sizeof(line), "%7.3f ", esphome::millis() / 1000.0);
  va_list args;
  va_start(args, fmt);
  vsnprintf(line + n, sizeof(line) - n, fmt, args);
  va_end(args);
  if (!strstr(line, "scan request")) {
    std::lock_guard<std::mutex> g(ble_log_lock);
    ble_log.emplace_back(line);
    if (ble_log.size() > 60)
      ble_log.pop_front();
  }
  esphome::esp_log_printf_(level, "lgmr.ble", __LINE__, "%s", line + n);
}

static void c_on_write(const char *what, const uint8_t *data, uint16_t len) {
  if (esphome::lg_magic_remote::instance)
    esphome::lg_magic_remote::instance->on_tv_write_(what, data, len);
}
static void c_on_sub(int report, bool on) {
  if (esphome::lg_magic_remote::instance)
    esphome::lg_magic_remote::instance->on_subscribed_(report, on);
}

namespace esphome::lg_magic_remote {

void LGMagicRemote::setup() {
  instance = this;
  this->uei_setup_();
  if (this->has_address_)
    lgmr_set_address(this->address_);
  lgmr_init(this->name_.c_str(), (const uint8_t *) this->mfg_.data(), (uint8_t) this->mfg_.size(), c_on_write,
            c_on_sub);
  lgmr_advertise(this->advertise_on_boot_);
}

void LGMagicRemote::dump_config() {
  ESP_LOGCONFIG(TAG, "LG Magic Remote emulation:\n  Name: %s\n  Manufacturer data: %s\n  Serial commands: %s",
                this->name_.c_str(), this->mfg_.c_str(), YESNO(this->serial_commands_));
}

void LGMagicRemote::on_tv_write_(const char *what, const uint8_t *data, uint16_t len) {
  std::lock_guard<std::mutex> g(this->lock_);
  this->writes_.push_back(Write{what, std::vector<uint8_t>(data, data + len)});
}

void LGMagicRemote::on_subscribed_(int report, bool on) {
  if (report == LGMR_FD_IN && on && lgmr_subscribed(LGMR_FD_IN)) {  // only once the link is encrypted
    std::lock_guard<std::mutex> g(this->lock_);
    this->fd_subscribed_event_ = true;
  }
}

bool LGMagicRemote::is_connected() const { return lgmr_connected(); }

void LGMagicRemote::capture_cmd(const std::string &hex) {
  std::vector<uint8_t> d;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    while (i < hex.size() && hex[i] == ' ') i++;
    if (i + 1 >= hex.size()) break;
    d.push_back((uint8_t) strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
  }
  if (!d.empty()) lgmr_cap_cmd(d.data(), d.size());
}

void LGMagicRemote::loop() {
  std::deque<Write> writes;
  bool fd_sub;
  {
    std::lock_guard<std::mutex> g(this->lock_);
    writes.swap(this->writes_);
    fd_sub = this->fd_subscribed_event_;
    this->fd_subscribed_event_ = false;
  }
  if (fd_sub) {
    // The real remote announces itself with the "active" pseudo-key once the TV subscribes.
    ESP_LOGI(TAG, "TV subscribed to FD: sending 0x803E (active)");
    this->idle_ = false;
    this->send_fd_(0x803E);
    this->send_fd_(0x0000);
  }
  for (auto &w : writes)
    this->handle_write_(w);

  uint32_t now = millis();
  if (this->motion_ && !lgmr_subscribed(LGMR_FD_IN)) {
    this->motion_ = false;
    this->high_freq_.stop();
  }
  if (this->pressing_ && this->motion_) {
    // A key held during a pointer session rides in the motion stream.
    Press &p = this->presses_.front();
    if (now - this->press_start_ >= p.hold_ms) {
      this->stream_key_ = 0;
      this->presses_.pop_front();
      this->pressing_ = false;
      this->last_report_ = now;
    }
  } else if (this->pressing_) {
    Press &p = this->presses_.front();
    if (now - this->press_start_ >= p.hold_ms) {
      this->send_fd_(0x0000);
      this->presses_.pop_front();
      this->pressing_ = false;
      this->last_report_ = now;
    } else if (now - this->last_report_ >= 15) {
      this->send_fd_(p.code);  // the real remote repeats the code at 100 Hz while a key is held
      this->last_report_ = now;
    }
  } else if (!this->presses_.empty() && now - this->last_report_ >= 30) {
    Press &p = this->presses_.front();
    if (!lgmr_subscribed(LGMR_FD_IN)) {
      ESP_LOGW(TAG, "key 0x%04X dropped: TV not connected/subscribed", p.code);
      this->presses_.pop_front();
    } else {
      bool in_stream = p.wheel || p.code == 0x8044;  // OK (wheel press) clicks at the pointer
      if (this->motion_ && !in_stream)
        this->stop_motion_("navigation key");  // like the real remote: a navigation key hides the pointer
      if (this->motion_) {
        this->motion_activity_ = now;
        if (p.wheel) {
          this->stream_wheel_ = p.wheel;
          this->presses_.pop_front();
        } else {
          ESP_LOGI(TAG, "key 0x%04X down (pointer)", p.code);
          this->stream_key_ = p.code;
          this->pressing_ = true;
          this->press_start_ = now;
        }
        this->last_report_ = now;
      } else {
      this->wake_if_idle_();
      if (p.wheel) {
        this->send_fd_(0x0000, p.wheel);
        this->presses_.pop_front();
        this->last_report_ = now;
      } else {
        ESP_LOGI(TAG, "key 0x%04X down", p.code);
        this->send_fd_(p.code);
        this->pressing_ = true;
        this->press_start_ = now;
        this->last_report_ = now;
      }
      }
    }
  }
  if (this->motion_)
    this->motion_tick_(now);
  if (this->voice_state_ != VOICE_IDLE)
    this->voice_tick_(now);
#ifdef LGMR_HAVE_IR
  // Universal control: a repeating IR key stops when the Bluetooth key that triggered it is released.
  if (this->ir_ != nullptr && this->ir_->repeating() && !this->pressing_)
    this->ir_->stop_repeat();
#endif

  if (this->pending_move_ && (int32_t) (millis() - this->pending_at_) >= 0) {
    this->pending_move_ = false;
    this->move_pointer(this->pending_dx_, this->pending_dy_);
  }
  lgmr_poll();
  lgmr_cap_poll();

  // Fragments of an interrupted 0x61/0x62 transfer must not end up in front of the next one.
  if (!lgmr_connected() && (!this->uei_rx_[0].empty() || !this->uei_rx_[1].empty())) {
    this->uei_rx_[0].clear();
    this->uei_rx_[1].clear();
  }
  // A bonded remote that lost its link retries now and then, so the TV comes back without a key press.
  if (!lgmr_connected() && now - this->last_reconnect_ >= 60000) {
    this->last_reconnect_ = now;
    lgmr_reconnect();
  }

#ifdef LGMR_SERIAL_COMMANDS
  if (this->serial_commands_)
    this->poll_serial_();
#endif
}

void LGMagicRemote::wake_if_idle_() {
  if (this->idle_) {
    this->idle_ = false;
    this->send_fd_(0x803E);
  }
}

void LGMagicRemote::send_fd_(uint16_t code, int8_t wheel) {
  static const int16_t still[3] = {0, 0, 0};
  this->send_report_(2, code, wheel, still);
}

void LGMagicRemote::send_report_(uint8_t mode, uint16_t code, int8_t wheel, const int16_t *gyro, const int16_t *accel) {
  static const int16_t flat[3] = {0, 0, -4087};
  if (accel == nullptr)
    accel = flat;
  // 19-byte FD report (layout per github.com/Haplois/uc3-airmouse docs/lg-tv.md, from MR23 captures): header c4, motion
  // sequence, battery << 2 | mode, header ff, gyro xyz and accel xyz (big-endian int16), key code, wheel.
  // Mode 2 = navigation: arrows move the focus and OK selects it. Mode 1 = pointing: reports carry the
  // gyro rates (about 0.07 deg/s per count) the TV turns into pointer movement, and OK clicks at the pointer.
  // With mode 1 outside a motion stream, OK clicked at the hidden pointer and arrows did nothing (C9, 2026-11-19).
  // Accel: still, gravity along -z (-4087 counts, about 4096 per g), as in the MR25 recording.
  uint8_t r[19] = {0xc4, this->seq_, (uint8_t) ((39 << 2) | mode), 0xff,
                   (uint8_t) (gyro[0] >> 8), (uint8_t) gyro[0], (uint8_t) (gyro[1] >> 8), (uint8_t) gyro[1],
                   (uint8_t) (gyro[2] >> 8), (uint8_t) gyro[2],
                   (uint8_t) (accel[0] >> 8), (uint8_t) accel[0], (uint8_t) (accel[1] >> 8), (uint8_t) accel[1],
                   (uint8_t) (accel[2] >> 8), (uint8_t) accel[2],
                   (uint8_t) (code >> 8), (uint8_t) code, (uint8_t) wheel};
  int rc = lgmr_notify(LGMR_FD_IN, r, sizeof(r));
  if (rc != 0)
    ESP_LOGW(TAG, "FD report not sent: %d", rc);
}

void LGMagicRemote::start_motion_() {
  if (this->motion_)
    return;
  if (!lgmr_subscribed(LGMR_FD_IN)) {
    ESP_LOGW(TAG, "pointer: TV not connected/subscribed");
    return;
  }
  // As the MR23 does: a navigation report with event 0x803E at sequence 0, then motion reports from sequence 1.
  ESP_LOGI(TAG, "pointer session start");
  this->idle_ = false;
  this->seq_ = 0;
  this->session_turns_ = 0;
  this->send_fd_(0x803E);
  this->motion_ = true;
  this->motion_last_tx_ = millis();
  this->motion_activity_ = this->motion_last_tx_;
  this->motion_start_ = this->motion_last_tx_;
  this->motion_reports_ = 0;
  this->high_freq_.start();
}

void LGMagicRemote::stop_motion_(const char *why) {
  if (!this->motion_)
    return;
  uint32_t ms = millis() - this->motion_start_;
  ESP_LOGI(TAG, "pointer session end (%s): %u motion reports in %u ms (%.0f/s)", why, (unsigned) this->motion_reports_,
           (unsigned) ms, ms ? this->motion_reports_ * 1000.0f / ms : 0.0f);
  this->motion_ = false;
  this->glide_ = false;
  this->glide_q_.clear();
  this->stream_key_ = 0;
  this->stream_wheel_ = 0;
  this->gyro_until_ = 0;
  this->high_freq_.stop();
  this->send_fd_(0x803F);  // keeps the last motion sequence number
}

void LGMagicRemote::motion_tick_(uint32_t now) {
  // 100 Hz: the TV counts arrivals per second and flags a weak signal below about 80/s.
  const uint32_t period = this->motion_period_ms_;
  if (now - this->motion_last_tx_ < period)
    return;
  this->motion_last_tx_ = (now - this->motion_last_tx_ > 3 * period) ? now : this->motion_last_tx_ + period;
  static const int16_t still[3] = {0, 0, 0};
  bool moving = (int32_t) (this->gyro_until_ - now) > 0;
  int16_t shake[3] = {0, 0, 0};
  bool shaking = this->shake_axis_ >= 0 && (int32_t) (this->shake_end_ - now) > 0;
  if (shaking)
    shake[this->shake_axis_] = (int16_t) ((((now - this->shake_start_) / this->shake_stroke_ms_) & 1) ? -this->shake_rate_
                                                                                                      : this->shake_rate_);
  else
    this->shake_axis_ = -1;
  ++this->seq_;
  ++this->motion_reports_;
  int16_t glide[3] = {0, 0, 0};
  if (!this->glide_q_.empty() && !shaking) {
    // Rate varies sinusoidally (glide_mean_ +- glide_amp_) so the TV never mistakes the turn for gyro drift; the
    // last sample of a segment turns only what is left of it.
    GlideSeg &s = this->glide_q_.front();
    float rate = this->glide_mean_ +
                 this->glide_amp_ * sinf(2.0f * 3.14159265f * this->glide_hz_ * this->glide_n_ * 0.01f);
    if (rate * 0.01f >= s.left)
      rate = s.left / 0.01f;
    s.left -= rate * 0.01f;
    this->glide_n_++;
    for (int i = 0; i < 3; i++)
      glide[i] = (int16_t) lroundf(rate * s.u[i]);
    if (s.left <= 0.001f)
      this->glide_q_.pop_front();
    this->motion_activity_ = now;
  }
  this->glide_ = !this->glide_q_.empty();
  bool gliding = glide[0] || glide[1] || glide[2];
  const int16_t *g = shaking ? shake : gliding ? glide : moving ? this->gyro_ : still;
  // Gyro noise (jitter_gyro_, default +-6 counts) on every report, at rest too: exactly zero gyro makes the TV
  // decide the remote was put down and hide the pointer within about a second, and a steady turn with too little
  // noise is learned as gyro drift and cancelled. Above about 20 counts at rest the TV stops re-learning zero drift
  // and the pointer slides (docs/protocol/pointer.md). Turns use at least glide_jitter_.
  bool turning = shaking || gliding || moving;
  if (gliding || (moving && (this->gyro_[0] || this->gyro_[1] || this->gyro_[2])))
    this->session_turns_++;
  auto rnd = [this](int amp) -> int {
    if (amp <= 0)
      return 0;
    this->rng_ = this->rng_ * 1664525u + 1013904223u;
    return (int) ((this->rng_ >> 8) % (uint32_t) (2 * amp + 1)) - amp;
  };
  int jg = turning ? std::max(this->jitter_gyro_, gliding ? this->glide_jitter_ : 0) : this->jitter_gyro_;
  int16_t gj[3], aj[3];
  for (int i = 0; i < 3; i++) {
    gj[i] = (int16_t) std::max(-32768, std::min(32767, g[i] + rnd(jg)));
    aj[i] = (int16_t) (this->grav_[i] + rnd(this->jitter_accel_));
  }
  this->send_report_(1, this->stream_key_, this->stream_wheel_, gj, aj);
  this->stream_wheel_ = 0;
  if ((int32_t) (now - this->motion_activity_) > (int32_t) this->pointer_timeout_ms_ && !this->pressing_)
    this->stop_motion_("timeout");
}

void LGMagicRemote::motion(int16_t g0, int16_t g1, int16_t g2, uint32_t ms) {
  this->start_motion_();
  if (!this->motion_)
    return;
  ESP_LOGI(TAG, "pointer gyro %d %d %d for %u ms", g0, g1, g2, (unsigned) ms);
  this->gyro_[0] = g0;
  this->gyro_[1] = g1;
  this->gyro_[2] = g2;
  uint32_t now = millis();
  this->gyro_until_ = now + ms;
  this->motion_activity_ = now + ms;
}

void LGMagicRemote::shake(int axis, int rate, uint32_t stroke_ms, int strokes) {
  if (axis < 0 || axis > 2)
    return;
  this->start_motion_();
  if (!this->motion_)
    return;
  uint32_t now = millis();
  ESP_LOGI(TAG, "pointer wake shake: axis %d, +-%d, %u ms x %d", axis, rate, (unsigned) stroke_ms, strokes);
  this->shake_axis_ = axis;
  this->shake_rate_ = rate;
  this->shake_stroke_ms_ = stroke_ms;
  this->shake_start_ = now;
  this->shake_end_ = now + stroke_ms * strokes;
  this->motion_activity_ = this->shake_end_;
}

void LGMagicRemote::move_pointer(int dx, int dy) {
  // How the C9 moves the pointer (MotionEngine ExtendedCursor, docs/protocol/pointer.md): each report moves it by an accelerated amount,
  // then pointer alignment rescales that by up to +-20% (r clamped to 1/1.2..1.2) towards anchor + K * (turned angle),
  // K being 1994 px/rad at first and then the running mean of the speeds used. While every sample's own gain stays
  // within 20% of K the alignment wins, so the pointer moves K * angle whatever the speed. On this TV's speed setting
  // that holds for about 130 to 290 counts/s. The glide turns at 210 +- 35 counts/s, so the TV's K settles at this
  // glide's mean gain, 2156 px/rad (pointer_k_), and every move is planned as pixels / K. Measured on the C9
  // (2026-11-20): repeatable to a few pixels, most moves within about 10%; 30 px moves come out about 20.
  if (dx == 0 && dy == 0) {
    this->glide_q_.clear();
    this->glide_ = false;
    this->motion(0, 0, 0, 0);
    return;
  }
  // After the TV hides the pointer (about 5.5 s without motion, or a navigation key; both end the session here)
  // motion alone doesn't bring it back; a gentle wake shake does (the TV's wake-on-shake: yaw above 1500 counts in
  // alternating strokes), and the pointer reappears at the screen centre. The move follows.
  if (!this->motion_ && !this->pending_move_) {
    this->shake(2, 1600, 80, 4);
    this->pending_move_ = true;
    this->pending_dx_ = dx;
    this->pending_dy_ = dy;
    this->pending_at_ = millis() + 4 * 80 + this->wake_settle_ms_;
    return;
  }
  this->start_motion_();
  if (!this->motion_)
    return;
  // The TV's target is anchor + K * (angle turned since the anchor), with K = 1994 for the first 100 moving samples of
  // a session and the running mean of the gains used after that (2156 px/rad with this glide). A move that
  // crosses sample 100 would be re-mapped with the new K, so a fresh session first
  // turns back and forth, 8 strokes netting zero angle, to get past it with the pointer where it was.
  if (this->session_turns_ + this->glide_n_pending_() < 110) {
    for (int k = 0; k < 8; k++) {
      GlideSeg s{};
      s.u[this->px_axis_] = (k & 1) ? -this->px_sign_ : this->px_sign_;
      s.left = 26.0f;
      this->glide_q_.push_back(s);
    }
  }
  float d = sqrtf((float) dx * dx + (float) dy * dy);
  GlideSeg s{};
  s.u[this->px_axis_] += this->px_sign_ * dx / d;
  s.u[this->py_axis_] += this->py_sign_ * dy / d;
  // angle in gyro count-seconds: pixels / (px per radian) / (radians per count)
  s.left = d / this->pointer_k_ / 0.0010653f;
  this->glide_q_.push_back(s);
  this->glide_ = true;
  this->gyro_until_ = 0;
  ESP_LOGI(TAG, "move_pointer %d,%d: glide %.0f count-s at %.0f+-%.0f counts/s (K %.0f px/rad, %u turns so far)", dx,
           dy, s.left, this->glide_mean_, this->glide_amp_, this->pointer_k_, (unsigned) this->session_turns_);
}

uint32_t LGMagicRemote::glide_n_pending_() const {
  float a = 0;
  for (const auto &s : this->glide_q_)
    a += s.left;
  return (uint32_t) (a / (this->glide_mean_ * 0.01f));
}

void LGMagicRemote::send_f9_(const std::vector<uint8_t> &data) {
  ESP_LOGI(TAG, "F9 in  <- %s", hex(data.data(), std::min<size_t>(data.size(), 24)).c_str());
  int rc = lgmr_notify(LGMR_F9_IN, data.data(), data.size());
  if (rc != 0)
    ESP_LOGW(TAG, "F9 answer not sent: %d", rc);
}

void LGMagicRemote::handle_write_(const Write &w) {
  ESP_LOGI(TAG, "TV wrote %s (%u bytes): %s", w.what.c_str(), (unsigned) w.data.size(),
           hex(w.data.data(), std::min<size_t>(w.data.size(), 48)).c_str());
  // The 2019 C9 sends commands to the feature report as well as to the output report.
  if ((w.what == "F9 out" || w.what == "F9 feature") && !w.data.empty())
    this->handle_f9_(w.data);
}

void LGMagicRemote::handle_f9_(const std::vector<uint8_t> &cmd) {
  // Answers as the MR25GB gave them to a 2025 TV (github.com/wittenbe/magic-remote-bridge docs/protocol.md). Unknown commands are only logged.
  switch (cmd[0]) {
    case 0x17: {
      if (this->scd_mode_ == 1) {
        ESP_LOGI(TAG, "SCD request left unanswered (scd_mode 1)");
        break;
      }
      // Framing as the MR23 sent it (github.com/Haplois/uc3-airmouse lg-protocol.md): two 154-byte values,
      // 17 <more: 01/00> <data bytes> 00 <data>. The terminal fragment makes the TV create the motion device.
      // Zero-length fragments (as sent until 2026-11-20) left buttons working but the pointer unable to move.
      // SCD holds lg-magic's dump, which starts one byte early (a leading 00) and lacks the last byte. The C9's
      // MotionEngine (docs/protocol/bluetooth.md) needs size 260 and version 5 in the
      // first two little-endian u16 and a zero sum over the 65 little-endian u32 words: drop the 00 and append ab,
      // the only last byte that zeroes the sum. The shifted block was rejected, so no motion device was created.
      uint8_t scd[260];
      memcpy(scd, SCD + 1, 259);
      scd[259] = 0xab;
      std::vector<uint8_t> a(154, 0), b(154, 0);
      a[0] = 0x17; a[1] = 0x01; a[2] = 150;
      memcpy(&a[4], scd, 150);
      b[0] = 0x17; b[1] = 0x00; b[2] = 110;
      memcpy(&b[4], scd + 150, 110);
      this->send_f9_(a);
      this->send_f9_(b);
      break;
    }
    case 0x11:
      if (this->scd_mode_ == 2) {
        ESP_LOGI(TAG, "version request 11 left unanswered (scd_mode 2)");
        break;
      }
      // Firmware version (older TVs; the C9 asks on every pointer start). Reply 12 <major> <zone> <version>, as an
      // MR20GA answered it (github.com/brainrom/lg-magic PR #8: major 21, zone 97, 02 02 28 34 23 + ff padding). The TV
      // gates further commands on the major byte.
      {
        // Bytes 8-10 and 11-13: the universal-control code sets we hold, 14-16 the primary (set-top power) one;
        // ff ff ff = none. The TV syncs them (0x64/0x67/0x62) from this.
        std::vector<uint8_t> r = {0x12, 21, 97, 0x02, 0x02, 0x28, 0x34, 0x23};
        this->uei_ids_(r);
        for (int j = 0; j < 3; j++)
          r.push_back(this->uei_primary_.id[j]);
        this->send_f9_(r);
      }
      break;
    case 0x13:
      this->send_f9_({0x14, 0x18, 0x52, 0x05, 0x38, 0x0c, 0x00, 0x01, 0x08, 0x03, 0xe0, 0x01});
      break;
    case 0x6d:
      this->send_f9_({0x6d, 0x00});
      break;
    case 0x6c:
      this->send_f9_({0x6c, 0x00});
      break;
    case 0x19: {
      this->send_f9_({0x19, 0x34, 0x23, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
      break;
    }
    case 0x90:
      this->send_f9_({0x90, 0x00, 0x00});
      break;
    case 0x01:
      ESP_LOGI(TAG, "TV: pointer active");
      this->idle_ = false;
      break;
    case 0x02:
      // Sleep request: the real remote answers 0x803F and drops the link ~15 s later. We stay connected.
      if (this->motion_) {
        this->stop_motion_("TV sleep request");
        this->idle_ = true;
      } else if (!this->idle_) {
        ESP_LOGI(TAG, "TV: idle/sleep, answering 0x803F");
        this->idle_ = true;
        this->send_fd_(0x803F);
        this->send_fd_(0x0000);
      }
      break;
    case 0x61:
    case 0x62:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
    case 0x68:
      this->uei_command_(cmd);
      break;
    case 0x03:  // voice: the TV opened the microphone
      if (this->voice_state_ == VOICE_WAIT_START) {
        ESP_LOGI(TAG, "voice: TV opened the microphone (F9 03) after %u ms; streaming",
                 (unsigned) (millis() - this->voice_since_));
        this->send_fd_(0x800A);
        this->voice_fd_at_ = 0;  // back to 0x808B on the next tick
        this->voice_state_ = VOICE_STREAMING;
      } else {
        ESP_LOGW(TAG, "voice: F9 03 outside a voice session; answering 0x800A anyway");
        this->send_fd_(0x800A);
        this->send_fd_(0x0000);
      }
      break;
    case 0x04:  // voice: the TV closed the microphone (after our release, or its own 10 s limit)
      ESP_LOGI(TAG, "voice: TV closed the microphone (F9 04)");
      this->voice_state_ = VOICE_IDLE;
      this->send_fd_(0x800D);
      this->send_fd_(0x0000);
      break;
    case 0x05:  // voice keep-alive, no answer
      ESP_LOGD(TAG, "voice: keep-alive (F9 05)");
      break;
    default:
      ESP_LOGW(TAG, "unknown F9 command 0x%02x, not answered", cmd[0]);
      break;
  }
}

void LGMagicRemote::press(uint16_t code, uint32_t hold_ms) {
  this->presses_.push_back(Press{code, 0, hold_ms});
}

bool LGMagicRemote::press(const std::string &name, uint32_t hold_ms) {
  if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
    this->press((uint16_t) (0x8010 + name[0] - '0'), hold_ms);
    return true;
  }
  for (auto &k : KEYS) {
    if (name == k.name) {
      this->press(k.code, hold_ms);
      return true;
    }
  }
  return false;
}

void LGMagicRemote::wheel(int8_t notches) {
  int8_t step = notches > 0 ? 1 : -1;
  for (int i = 0; i < abs(notches); i++)
    this->presses_.push_back(Press{0, step, 0});
}

void LGMagicRemote::forget() {
  // F9 40 ("unplug", docs/protocol/bluetooth.md) makes the TV unpair this remote itself: it clears the
  // connection at once, removes the pairing and shows a toast. Without it only our side forgot the bond and the TV
  // kept trying the old one. Our bond is deleted right away, before the TV drops the link: with a bond left, the
  // disconnect starts directed advertising to the TV, it connects, and the failed encryption that follows trips its
  // pairing rate limit (SMP 0x09 Repeated Attempts, seen 2026-11-20).
  bool tell = lgmr_subscribed(LGMR_F9_IN);
  if (tell) {
    ESP_LOGI(TAG, "telling the TV to unpair this remote (F9 40)");
    this->send_f9_({0x40});
  }
  lgmr_forget();
  if (tell)
    this->set_timeout("forget", 3000, []() { lgmr_disconnect(); });  // in case the TV keeps the link
}

// Voice session, as the MR19 firmware and the C9's voiceinput/voiceconductor run it (docs/protocol/voice.md): hold KEY_VOICE 0x808B (hold-to-talk) -> the TV opens the microphone with F9 03 -> answer FD
// 0x800A within 500 ms -> stream FE reports (one per 15 ms, no gap of 1 s) -> release the key -> the TV closes with
// F9 04 -> answer FD 0x800D within 500 ms. F9 05 is a keep-alive every ~3 s.
bool LGMagicRemote::voice_begin() {
  if (!lgmr_subscribed(LGMR_FD_IN) || !lgmr_subscribed(LGMR_FE_IN))
    return false;
  if (this->motion_)
    this->stop_motion_("voice");
  this->wake_if_idle_();
  ESP_LOGI(TAG, "voice: holding the Voice key, waiting for the TV to open the microphone (F9 03)");
  this->voice_state_ = VOICE_WAIT_START;
  this->voice_since_ = this->voice_fd_at_ = millis();
  this->send_fd_(0x808B);
  return true;
}

bool LGMagicRemote::voice_packet(const uint8_t *payload120) {
  if (this->voice_state_ != VOICE_STREAMING)
    return false;
  // 123-byte FE value: sequence, key held (big-endian 0x808B), two 60-byte mSBC units. The report map says 124,
  // but every Realtek LG remote sends 123 and lginput2 rejects anything else for voice.
  uint8_t r[123];
  r[0] = this->voice_seq_++;
  r[1] = 0x80;
  r[2] = 0x8B;
  memcpy(r + 3, payload120, 120);
  int rc = lgmr_notify(LGMR_FE_IN, r, sizeof(r));
  if (rc != 0) {
    ESP_LOGW(TAG, "voice: FE report not sent: %d", rc);
    return lgmr_subscribed(LGMR_FE_IN);  // a full buffer drops one packet; a lost link ends the session
  }
  return true;
}

void LGMagicRemote::voice_end() {
  if (this->voice_state_ == VOICE_STREAMING) {
    ESP_LOGI(TAG, "voice: releasing the Voice key, waiting for the TV to close the microphone (F9 04)");
    this->send_fd_(0x0000);
    this->voice_state_ = VOICE_WAIT_STOP;
    this->voice_since_ = millis();
  } else if (this->voice_state_ == VOICE_WAIT_START) {
    this->send_fd_(0x0000);
    this->voice_state_ = VOICE_IDLE;
  }
}

void LGMagicRemote::voice_tick_(uint32_t now) {
  switch (this->voice_state_) {
    case VOICE_WAIT_START:
      if (now - this->voice_since_ > 4000) {
        ESP_LOGW(TAG, "voice: the TV didn't open the microphone (no F9 03 in 4 s); giving up");
        this->send_fd_(0x0000);
        this->voice_state_ = VOICE_IDLE;
        return;
      }
      [[fallthrough]];  // keep the key held
    case VOICE_STREAMING:
      if (now - this->voice_fd_at_ >= 50) {  // lginput2 releases a key not refreshed for 400 ms
        this->send_fd_(0x808B);
        this->voice_fd_at_ = now;
      }
      break;
    case VOICE_WAIT_STOP:
      if (now - this->voice_since_ > 1500) {
        ESP_LOGW(TAG, "voice: no F9 04 from the TV after the release");
        this->voice_state_ = VOICE_IDLE;
      }
      break;
    default:
      break;
  }
}

// ---- Universal control ----------------------------------------------------------------------------------------
// The TV sends set-top box (or other device) IR codes as UEI ZipIR data: 0x61 = one key to transmit now, 0x62 = a
// whole code set to keep, then 0x65 = transmit key N of a kept code set. 0x61/0x62 arrive in fragments
// "op more data..." (more = 1 while another follows). Replies "op status" (0 = ok); 0x65 replies with id and key.

void LGMagicRemote::uei_setup_() {
  for (int i = 0; i < 2; i++) {
    this->uei_pref_[i] = global_preferences->make_preference<UeiSlot>(fnv1_hash("lgmr_uei_slot") + i);
    if (!this->uei_pref_[i].load(&this->uei_slot_[i]) || this->uei_slot_[i].len > sizeof(this->uei_slot_[i].data))
      memset(&this->uei_slot_[i], 0, sizeof(UeiSlot));
  }
  this->uei_pref_primary_ = global_preferences->make_preference<UeiPrimary>(fnv1_hash("lgmr_uei_primary"));
  if (!this->uei_pref_primary_.load(&this->uei_primary_))
    memset(this->uei_primary_.id, 0xff, 3);
}

void LGMagicRemote::uei_save_(int i) { this->uei_pref_[i].save(&this->uei_slot_[i]); }

void LGMagicRemote::uei_ids_(std::vector<uint8_t> &r) const {
  // The ids of the held code sets, held ones first (id1, id2), ff ff ff for none: the layout of the 0x66 reply and
  // of F9 12 bytes 8-13 (docs/protocol/universal-control.md).
  int n = 0;
  for (int i = 0; i < 2; i++)
    if (this->uei_slot_[i].used) {
      r.insert(r.end(), this->uei_slot_[i].id, this->uei_slot_[i].id + 3);
      n++;
    }
  for (; n < 2; n++)
    r.insert(r.end(), {0xff, 0xff, 0xff});
}

int LGMagicRemote::uei_find_(const uint8_t *id) const {
  for (int i = 0; i < 2; i++)
    if (this->uei_slot_[i].used && memcmp(this->uei_slot_[i].id, id, 3) == 0)
      return i;
  return -1;
}

void LGMagicRemote::uei_status() {
  for (int i = 0; i < 2; i++) {
    auto &s = this->uei_slot_[i];
    if (s.used)
      ESP_LOGI(TAG, "universal control slot %d: code set %c%04u, %u bytes%s", i, s.id[0], (unsigned) ((s.id[1] << 8) | s.id[2]),
               s.len, memcmp(s.id, this->uei_primary_.id, 3) == 0 ? " (primary)" : "");
    else
      ESP_LOGI(TAG, "universal control slot %d: empty", i);
  }
}

void LGMagicRemote::uei_command_(const std::vector<uint8_t> &cmd) {
  uint8_t op = cmd[0];
  if (op == 0x61 || op == 0x62) {
    if (cmd.size() < 2)
      return;
    auto &buf = this->uei_rx_[op - 0x61];
    buf.insert(buf.end(), cmd.begin() + 2, cmd.end());
    if (cmd[1]) {  // more fragments follow: acknowledge this one
      this->send_f9_({op, 0x00});
      return;
    }
    std::vector<uint8_t> data;
    data.swap(buf);
#ifndef LGMR_HAVE_IR
    // No infrared: refuse code sets, so the TV doesn't count on us to transmit them.
    ESP_LOGW(TAG, "universal control: no lg_ir_remote linked, refusing 0x%02x", op);
    this->send_f9_({op, 0x01});
    return;
#else
    if (op == 0x61) {
      // repeat flag, then a one-key ZipIR file with the key as function 0
      bool ok = data.size() > 1 && this->ir_ != nullptr &&
                this->ir_->transmit_zipir(data.data() + 1, data.size() - 1, 0, data[0] != 0 && this->pressing_);
      this->send_f9_({0x61, (uint8_t) (ok ? 0x00 : 0x01)});
      return;
    }
    // 0x62: code set id (3 bytes), then its ZipIR file
    if (data.size() < 4 || data.size() - 3 > sizeof(UeiSlot::data) || this->ir_ == nullptr ||
        !this->ir_->zipir_usable(data.data() + 3, data.size() - 3)) {
      ESP_LOGW(TAG, "universal control: code set not usable (%u bytes)", (unsigned) data.size());
      this->send_f9_({0x62, 0x01});
      return;
    }
    int i = this->uei_find_(data.data());
    if (i < 0)
      i = !this->uei_slot_[0].used ? 0 : !this->uei_slot_[1].used ? 1 : -1;
    if (i < 0) {
      // Both slots in use (the TV normally erases unused sets first, 0x64): replace the one that doesn't hold the
      // primary (set-top power) code set, so the newest set is always kept. The spec doesn't say what the real
      // remote does here.
      i = memcmp(this->uei_slot_[0].id, this->uei_primary_.id, 3) == 0 ? 1 : 0;
      ESP_LOGW(TAG, "universal control: both slots in use; replacing slot %d (%c%04u)", i, this->uei_slot_[i].id[0],
               (unsigned) ((this->uei_slot_[i].id[1] << 8) | this->uei_slot_[i].id[2]));
    }
    auto &s = this->uei_slot_[i];
    s.used = 1;
    memcpy(s.id, data.data(), 3);
    s.len = (uint16_t) (data.size() - 3);
    memcpy(s.data, data.data() + 3, s.len);
    this->uei_save_(i);
    ESP_LOGI(TAG, "universal control: stored code set %c%04u (%u bytes) in slot %d", s.id[0],
             (unsigned) ((s.id[1] << 8) | s.id[2]), s.len, i);
    this->send_f9_({0x62, 0x00});
    return;
#endif
  }
  switch (op) {
    case 0x64: {  // erase one code set, or all (ff ff ff)
      bool all = cmd.size() < 4 || (cmd[1] == 0xff && cmd[2] == 0xff && cmd[3] == 0xff);
      for (int i = 0; i < 2; i++)
        if (all || (this->uei_slot_[i].used && memcmp(this->uei_slot_[i].id, &cmd[1], 3) == 0)) {
          memset(&this->uei_slot_[i], 0, sizeof(UeiSlot));
          this->uei_save_(i);
        }
      ESP_LOGI(TAG, "universal control: erased %s", all ? "all code sets" : "a code set");
      this->send_f9_({0x64, 0x00});
      break;
    }
    case 0x65: {  // transmit key of a stored code set: id(3) key repeat
      if (cmd.size() < 5)
        break;
      int i = this->uei_find_(&cmd[1]);
      bool ok = false;
#ifdef LGMR_HAVE_IR
      // repeat (cmd[5]) keeps the key's repeat frame going while the Bluetooth key that caused this is held
      ok = i >= 0 && this->ir_ != nullptr &&
           this->ir_->transmit_zipir(this->uei_slot_[i].data, this->uei_slot_[i].len, cmd[4],
                                     cmd.size() > 5 && cmd[5] && this->pressing_);
#endif
      if (i < 0)
        ESP_LOGW(TAG, "universal control: TV asked for a code set we don't hold");
      this->send_f9_({0x65, (uint8_t) (ok ? 0x00 : 0x01), cmd[1], cmd[2], cmd[3], cmd[4]});
      break;
    }
    case 0x66: {  // which code sets do we hold
      std::vector<uint8_t> r = {0x66, (uint8_t) ((this->uei_slot_[0].used ? 1 : 0) + (this->uei_slot_[1].used ? 1 : 0))};
      this->uei_ids_(r);
      this->send_f9_(r);
      break;
    }
    case 0x67:  // primary (set-top power) code set
      if (cmd.size() >= 4) {
        memcpy(this->uei_primary_.id, &cmd[1], 3);
        this->uei_pref_primary_.save(&this->uei_primary_);
      }
      this->send_f9_({0x67, 0x00});
      break;
    case 0x68:
      this->send_f9_({0x68, this->uei_primary_.id[0], this->uei_primary_.id[1], this->uei_primary_.id[2]});
      break;
  }
}

void LGMagicRemote::adv_burst(const std::string &hex, uint32_t ms, int pdu) {
  std::vector<uint8_t> d;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    if (hex[i] == ' ' || hex[i] == ':') {
      i--;
      continue;
    }
    d.push_back((uint8_t) strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
  }
  int rc = lgmr_adv_burst(d.data(), d.size(), ms, pdu);
  if (rc != 0)
    ESP_LOGW(TAG, "advertising burst not sent (%d)", rc);
}

void LGMagicRemote::inject_f9(const std::string &hex) {
  std::vector<uint8_t> d;
  for (size_t i = 0; i + 1 < hex.size();) {
    if (hex[i] == ' ') {
      i++;
      continue;
    }
    d.push_back((uint8_t) strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
    i += 2;
  }
  if (d.empty())
    return;
  ESP_LOGI(TAG, "test: F9 command injected (%u bytes)", (unsigned) d.size());
  this->handle_f9_(d);
}

void LGMagicRemote::wake_tv(uint32_t ms, int pdu) {
  // Our Bluetooth address is the id the TV must have registered (docs/protocol/wake.md).
  int rc = this->wake_burst_(ms, pdu);
  if (rc != 0)
    ESP_LOGW(TAG, "wake burst not sent (%d)%s", rc, rc == LGMR_WOBLE_CONNECTED ? ": the TV is connected, so it's on" : "");
}

void LGMagicRemote::bt_reconnect() {
  ESP_LOGI(TAG, "dropping the Bluetooth link so the TV reconnects");
  lgmr_disconnect();
}

void LGMagicRemote::dump_ble_log() {
  std::deque<std::string> copy;
  {
    std::lock_guard<std::mutex> g(ble_log_lock);
    copy = ble_log;
  }
  ESP_LOGI(TAG, "Bluetooth log, last %u lines (seconds since boot):", (unsigned) copy.size());
  for (auto &l : copy)
    ESP_LOGI(TAG, "  %s", l.c_str());
}

int LGMagicRemote::wake_burst_(uint32_t ms, int pdu) {
  uint8_t id[6];
  if (this->has_address_)
    memcpy(id, this->address_, 6);
  else
    lgmr_own_address(id);
  return lgmr_woble_burst(id, ms, pdu);
}

bool LGMagicRemote::ir_send_(const std::string &name) {
#ifdef LGMR_HAVE_IR
  if (this->ir_ != nullptr && lg_ir_remote::LGIRRemote::code_for(name) != 0) {
    this->ir_->press(name);
    return true;
  }
#endif
  return false;
}

void LGMagicRemote::key(const std::string &name) {
  bool connected = lgmr_subscribed(LGMR_FD_IN);
  // Discrete power. Connected means the TV is on: Power on has nothing to do, Power off is Bluetooth Power. In
  // standby there is no link: Power and Power on wake the TV over Bluetooth (IR only if the burst can't be sent),
  // and Power off sends the discrete IR off, which leaves a TV in standby alone.
  if (name == "poweron" && connected) {
    ESP_LOGI(TAG, "Power on: the TV is connected, so it is on already");
    return;
  }
  if (name == "poweroff" && connected) {
    this->press((uint16_t) 0x0008, 0);
    return;
  }
  if (connected && this->press(name))
    return;
  if (!connected && (name == "power" || name == "poweron") && this->wake_burst_(4000, 0) == 0)
    return;
  if (!this->ir_send_(name))
    ESP_LOGW(TAG, "key %s: %s", name.c_str(),
             connected ? "no Bluetooth code and no IR code" : "the TV is not connected and there is no IR code to send");
  if (!connected)
    lgmr_reconnect();  // like a bonded remote woken by a key: ask the TV to reconnect
}

void LGMagicRemote::pair() {
  lgmr_advertise(true);
#ifdef LGMR_HAVE_IR
  if (this->ir_ != nullptr) {
    ESP_LOGI(TAG, "pairing: advertising, sending the IR pairing code");
    this->ir_->send_pairing();
    return;
  }
#endif
  ESP_LOGW(TAG, "pairing: advertising; no lg_ir_remote linked, so send LG IR 0x%08X to the TV with any IR blaster now",
           (unsigned) 0x20DFC13E);
}

void LGMagicRemote::apply_identity_() {
  lgmr_set_identity(this->name_.c_str(), (const uint8_t *) this->mfg_.data(), (uint8_t) this->mfg_.size());
}

#ifdef LGMR_SERIAL_COMMANDS
void LGMagicRemote::poll_serial_() {
  uint8_t buf[64];
  int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
  for (int i = 0; i < n; i++) {
    char c = (char) buf[i];
    if (c == '\r' || c == '\n') {
      if (!this->line_.empty())
        this->run_command_(this->line_);
      this->line_.clear();
    } else if (this->line_.size() < 200) {
      this->line_ += c;
    }
  }
}

void LGMagicRemote::run_command_(const std::string &line) {
  std::string cmd = line, arg;
  size_t sp = line.find(' ');
  if (sp != std::string::npos) {
    cmd = line.substr(0, sp);
    arg = line.substr(sp + 1);
  }
  ESP_LOGI(TAG, "> %s", line.c_str());
  if (cmd == "cap") {
    if (arg == "stop") lgmr_cap_stop();
    else if (arg == "dump") lgmr_cap_dump();
    else if (arg == "forget") lgmr_cap_forget();
    else lgmr_cap_start();
  } else if (cmd == "capcmd") {
    this->capture_cmd(arg);
  } else if (cmd == "status") {
    lgmr_status();
    ESP_LOGI(TAG, "idle %d, pending keys %u", this->idle_, (unsigned) this->presses_.size());
  } else if (cmd == "pair") {
    this->pair();
#ifdef LGMR_HAVE_IR
  } else if (cmd == "ir" && this->ir_ != nullptr) {
    // ir <hex code> [hold ms]
    char *end = nullptr;
    uint32_t code = (uint32_t) strtoul(arg.c_str(), &end, 16);
    this->ir_->send(code, end && *end ? (uint32_t) strtoul(end, nullptr, 10) : 0);
#endif
  } else if (cmd == "reconnect") {
    lgmr_reconnect();
  } else if (cmd == "power") {
    this->key("power");
  } else if (cmd == "adv") {
    lgmr_advertise(arg != "off");
  } else if (cmd == "name") {
    this->name_ = arg;
    this->apply_identity_();
  } else if (cmd == "mfg") {
    this->mfg_ = arg == "-" ? "" : arg;  // "mfg -" = no LG manufacturer data
    this->apply_identity_();
  } else if (cmd == "hid" || cmd == "nameadv") {
    (cmd == "hid" ? this->adv_hid_ : this->adv_name_main_) = arg != "off";
    lgmr_set_layout(this->adv_hid_, this->adv_name_main_, (const uint8_t *) this->mfg_.data(),
                    (uint8_t) this->mfg_.size());
  } else if (cmd == "flags") {
    lgmr_set_flags((uint8_t) strtoul(arg.c_str(), nullptr, 16), (const uint8_t *) this->mfg_.data(),
                   (uint8_t) this->mfg_.size());
  } else if (cmd == "forget") {
    lgmr_disconnect();
    lgmr_forget();
  } else if (cmd == "disconnect") {
    lgmr_disconnect();
  } else if (cmd == "key") {
    // key <hex code> [hold ms]
    char *end = nullptr;
    uint16_t code = (uint16_t) strtoul(arg.c_str(), &end, 16);
    uint32_t hold = end && *end ? (uint32_t) strtoul(end, nullptr, 10) : 80;
    this->press(code, hold ? hold : 80);
  } else if (cmd == "wheel") {
    this->wheel((int8_t) atoi(arg.c_str()));
  } else if (cmd == "gyro") {
    // gyro <g0> <g1> <g2> <ms>
    int g0 = 0, g1 = 0, g2 = 0, ms = 500;
    sscanf(arg.c_str(), "%d %d %d %d", &g0, &g1, &g2, &ms);
    this->motion((int16_t) g0, (int16_t) g1, (int16_t) g2, (uint32_t) ms);
  } else if (cmd == "move") {
    int dx = 0, dy = 0;
    sscanf(arg.c_str(), "%d %d", &dx, &dy);
    this->move_pointer(dx, dy);
  } else if (cmd == "f9") {
    this->send_f9_(parse_hex(arg));
  } else if (cmd == "fd") {
    auto d = parse_hex(arg);
    lgmr_notify(LGMR_FD_IN, d.data(), d.size());
  } else {
    // A key name, optionally with a hold time: "ok", "home 1500".
    uint32_t hold = arg.empty() ? 80 : (uint32_t) strtoul(arg.c_str(), nullptr, 10);
    if (!this->press(cmd, hold ? hold : 80))
      ESP_LOGW(TAG, "unknown command '%s'. Commands: status, pair, ir <hex> [hold ms], power, adv on|off, name <s>, "
                    "mfg <s>, flags <hex>, hid on|off, nameadv on|off, forget, disconnect, key <hex> [ms], wheel <n>, f9 <hex>, fd <hex>, or a key name "
                    "(ok up down left right back home settings more volup voldown mute chup chdown 0-9 red green "
                    "yellow blue guide input play pause stop rewind ff exit info picture sound subtitles list netflix prime) [ms]",
               cmd.c_str());
  }
}
#endif  // LGMR_SERIAL_COMMANDS

}  // namespace esphome::lg_magic_remote
