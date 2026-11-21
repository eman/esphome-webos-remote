#pragma once
#include "lgmr_ble.h"

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#ifdef LGMR_HAVE_IR
#include "esphome/components/lg_ir_remote/lg_ir_remote.h"
#endif

#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace esphome::lg_magic_remote {

class LGMagicRemote : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

#ifdef LGMR_HAVE_IR
  void set_ir_remote(lg_ir_remote::LGIRRemote *ir) { this->ir_ = ir; }
#endif
  void set_device_name(const std::string &name) { this->name_ = name; }
  void set_manufacturer_string(const std::string &mfg) { this->mfg_ = mfg; }
  void set_advertise_on_boot(bool on) { this->advertise_on_boot_ = on; }
  void set_serial_commands(bool on) { this->serial_commands_ = on; }
  void set_address(uint64_t a) {
    for (int i = 0; i < 6; i++)
      this->address_[i] = (uint8_t) (a >> (8 * (5 - i)));
    this->has_address_ = true;
  }

  /// Press and release a Magic Remote key (FD report code, e.g. 0x8044 = OK).
  void press(uint16_t code, uint32_t hold_ms = 80);
  /// Press a key by name ("ok", "up", "home", "volup", ...). Returns false for an unknown name.
  bool press(const std::string &name, uint32_t hold_ms = 80);
  void wheel(int8_t notches);
  /// Pointer: stream motion reports (100 Hz) with these gyro rates for `ms`, then hold still. The pointer
  /// session ends (0x803F) after `pointer_timeout_ms` without pointer activity, or on a navigation key.
  void motion(int16_t g0, int16_t g1, int16_t g2, uint32_t ms);
  /// Move the pointer by roughly (dx, dy) screen pixels (1920x1080 UI); positive = right/down.
  void move_pointer(int dx, int dy);
  void set_pointer_axes(int x_axis, int x_sign, int y_axis, int y_sign, float px_per_count_s) {
    this->px_axis_ = x_axis; this->px_sign_ = x_sign; this->py_axis_ = y_axis; this->py_sign_ = y_sign;
    this->px_per_count_s_ = px_per_count_s;
  }
  bool pointer_active() const { return this->motion_; }
  /// Wake gesture: alternating strokes of +-`rate` counts on gyro `axis`, `stroke_ms` each, `strokes` times.
  void shake(int axis, int rate = 2500, uint32_t stroke_ms = 120, int strokes = 6);
  // Test: 0 = answer SCD (17) and version (11), 1 = leave 17 unanswered, 2 = leave 11 unanswered.
  void set_scd_mode(int m) { this->scd_mode_ = m; }
  void set_motion_period(uint32_t ms) { this->motion_period_ms_ = ms < 5 ? 5 : ms; }
  /// Noise added to every motion report (counts, peak). A steady turn with little gyro noise is learned by the TV as
  /// gyro drift and cancelled (accelerometer noise doesn't prevent it); noise at rest stops it from re-learning zero.
  void set_jitter(int gyro, int accel) { this->jitter_gyro_ = gyro; this->jitter_accel_ = accel; }
  /// move_pointer tuning: K in px per radian, rate = mean +- amp counts/s at hz.
  void set_glide(float k, float mean, float amp, float hz) {
    this->pointer_k_ = k; this->glide_mean_ = mean; this->glide_amp_ = amp; this->glide_hz_ = hz;
  }
  /// Gravity in accelerometer counts (about 4096 per g) reported while still.
  void set_gravity(int x, int y, int z) { this->grav_[0] = x; this->grav_[1] = y; this->grav_[2] = z; }
  /// Drop the Bluetooth link; the TV reconnects to the directed advertising within seconds.
  void bt_reconnect();
  // Capture mode: act as the TV for a real Magic Remote and log what it sends (lgmr_capture.c).
  void capture(const std::string &what) {
    if (what == "stop") lgmr_cap_stop();
    else if (what == "dump") lgmr_cap_dump();
    else if (what == "forget") lgmr_cap_forget();
    else lgmr_cap_start();
  }
  void capture_cmd(const std::string &hex);
  /// A key by name ("ok", "home", "hdmi1", "poweron", ...): over Bluetooth while the TV is connected, otherwise
  /// (or for keys without a Bluetooth code) over infrared when an lg_ir_remote is linked. "power" and "poweron"
  /// wake a TV in standby over Bluetooth; "poweroff" only ever turns it off.
  void key(const std::string &name);
  /// Open the TV's registration for this remote: advertise, and send the IR pairing code if IR is linked.
  void pair();
  // Logs the last Bluetooth log lines (kept from boot on).
  void dump_ble_log();
  // Deletes the TV's bond on our side (the TV keeps its own until it re-registers).
  void forget();
  // Voice (lgmr_voice_speaker): open a microphone session, send one 120-byte mSBC payload, close it.
  bool voice_begin();
  bool voice_packet(const uint8_t *payload120);
  void voice_end();
  // Wake the TV from standby over Bluetooth ("Turn on via Bluetooth"); the TV must have our address registered.
  void wake_tv(uint32_t ms, int pdu = 0);
  void adv_burst(const std::string &hex, uint32_t ms, int pdu);  // any advertising data, for wake experiments
  void uei_status();  // log the stored universal-control code sets
  void inject_f9(const std::string &hex);  // test: handle hex as if the TV had written it to F9
  bool voice_streaming() const { return this->voice_state_ == VOICE_STREAMING; }
  bool voice_idle() const { return this->voice_state_ == VOICE_IDLE; }
  bool is_connected() const;

  // Called from the NimBLE host task.
  void on_tv_write_(const char *what, const uint8_t *data, uint16_t len);
  void on_subscribed_(int report, bool on);

 protected:
  struct Write {
    std::string what;
    std::vector<uint8_t> data;
  };
  struct Press {
    uint16_t code;
    int8_t wheel;
    uint32_t hold_ms;
    uint32_t queued_at{0};  // millis() when queued; a key waits up to KEY_WAIT_MS for the TV to reconnect
  };
  // Like a bonded Magic Remote, which keeps a key pressed while the link was down and sends it once the TV has
  // reconnected (it falls back to IR only when its reconnect advertising times out).
  static const uint32_t KEY_WAIT_MS = 8000;

  void handle_write_(const Write &w);
  void handle_f9_(const std::vector<uint8_t> &cmd);
  void send_f9_(const std::vector<uint8_t> &data);
  void send_fd_(uint16_t code, int8_t wheel = 0);
  void send_report_(uint8_t mode, uint16_t code, int8_t wheel, const int16_t *gyro, const int16_t *accel = nullptr);
  void start_motion_();
  void stop_motion_(const char *why);
  void motion_tick_(uint32_t now);
  uint32_t glide_n_pending_() const;
  void wake_if_idle_();
  void poll_serial_();
  void run_command_(const std::string &line);
  void apply_identity_();

#ifdef LGMR_HAVE_IR
  lg_ir_remote::LGIRRemote *ir_{nullptr};
#endif
  bool ir_send_(const std::string &name);
  int wake_burst_(uint32_t ms, int pdu);  // wake advert with our id: the configured address, or the chip's own
  std::string name_{"LGE MR23"};
  std::string mfg_{"SCD 21.2,BA 35,webOS"};
  bool advertise_on_boot_{true};
  bool serial_commands_{true};
  bool adv_hid_{false};
  uint8_t address_[6]{};
  bool has_address_{false};
  bool adv_name_main_{false};

  std::mutex lock_;
  std::deque<Write> writes_;
  bool fd_subscribed_event_{false};

  std::deque<Press> presses_;
  bool pressing_{false};
  uint32_t press_start_{0};
  uint32_t last_report_{0};
  uint8_t seq_{0};
  bool idle_{false};
  uint32_t last_reconnect_{0};

  // Pointer (motion) session.
  HighFrequencyLoopRequester high_freq_;
  bool motion_{false};
  uint32_t motion_last_tx_{0};
  uint32_t motion_activity_{0};
  uint32_t motion_start_{0};
  int scd_mode_{0};
  uint32_t motion_period_ms_{10};
  int jitter_gyro_{6}, jitter_accel_{40};  // gyro noise above about 20 at rest makes the pointer drift
  int grav_[3]{0, 0, -4087};
  uint32_t rng_{0x12345678};
  int shake_axis_{-1}, shake_rate_{0};
  uint32_t shake_start_{0}, shake_stroke_ms_{120}, shake_end_{0};
  uint32_t motion_reports_{0};
  uint32_t gyro_until_{0};
  int16_t gyro_[3]{};
  uint16_t stream_key_{0};
  int8_t stream_wheel_{0};
  uint32_t pointer_timeout_ms_{8000};
  int px_axis_{2}, px_sign_{1}, py_axis_{0}, py_sign_{1};
  // move_pointer glide (see move_pointer): unit direction in gyro axes, angle still to turn (count-seconds).
  struct GlideSeg {
    float u[3];  // unit direction in gyro axes
    float left;  // angle still to turn, count-seconds
  };
  std::deque<GlideSeg> glide_q_;
  bool glide_{false};
  uint32_t glide_n_{0};        // glide reports sent (phase of the rate variation)
  uint32_t session_turns_{0};  // reports with rotation since the session started (the TV's K uses 1994 for 100)
  // TV's pointer alignment gain once past its first 100 moving samples, px per radian: the mean gain of this glide
  // on the TV's pointer speed setting (ME speed 1), from the firmware model and confirmed on the C9 (2026-11-20).
  float pointer_k_{2156.0f};
  float glide_mean_{210.0f}, glide_amp_{35.0f}, glide_hz_{5.0f};  // counts/s
  int glide_jitter_{6};
  bool pending_move_{false};  // move_pointer waiting for its wake shake to finish
  int pending_dx_{0}, pending_dy_{0};
  uint32_t pending_at_{0};
  uint32_t wake_settle_ms_{600};  // after the wake shake, before the move
  float px_per_count_s_{0.05f};

  std::string line_;
  enum VoiceState : uint8_t { VOICE_IDLE, VOICE_WAIT_START, VOICE_STREAMING, VOICE_WAIT_STOP };
  void voice_tick_(uint32_t now);
  VoiceState voice_state_{VOICE_IDLE};
  uint32_t voice_since_{0}, voice_fd_at_{0};
  uint8_t voice_seq_{0};

  // Universal control (UEI ZipIR, F9 0x61..0x68; docs/protocol/universal-control.md).
  struct UeiSlot {
    uint8_t used;
    uint8_t id[3];  // type letter, id hi, id lo
    uint16_t len;
    uint8_t data[1600];
  };
  struct UeiPrimary {
    uint8_t id[3];
  };
  void uei_setup_();
  void uei_command_(const std::vector<uint8_t> &cmd);
  int uei_find_(const uint8_t *id) const;
  void uei_save_(int i);
  void uei_ids_(std::vector<uint8_t> &r) const;
  UeiSlot uei_slot_[2]{};
  UeiPrimary uei_primary_{{0xff, 0xff, 0xff}};
  ESPPreferenceObject uei_pref_[2], uei_pref_primary_;
  std::vector<uint8_t> uei_rx_[2];  // fragments of 0x61 and 0x62 being collected
};

}  // namespace esphome::lg_magic_remote
