#include "lg_ir_remote.h"
#include "lgir_zipir.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>

namespace esphome::lg_ir_remote {

static const char *const TAG = "lg_ir_remote";

struct IRKey {
  const char *name;
  uint32_t code;
};
// LG TV codes: NEC, address 0x04 (written 20 DF), then the command byte and its complement. The command byte,
// bit-reversed, is the TV's key index, and the Bluetooth Magic Remote sends 0x8000 | that index for the same key
// (docs/protocol/ir-keys.md). Names match lg_magic_remote's Bluetooth key names.
static const IRKey KEYS[] = {
    {"power", 0x20DF10EF},     {"poweron", 0x20DF23DC},   {"poweroff", 0x20DFA35C}, {"up", 0x20DF02FD},
    {"down", 0x20DF827D},      {"left", 0x20DFE01F},      {"right", 0x20DF609F},    {"ok", 0x20DF22DD},
    {"back", 0x20DF14EB},      {"exit", 0x20DFDA25},      {"home", 0x20DF3EC1},     {"settings", 0x20DFC23D},
    {"more", 0x20DFA25D},      {"input", 0x20DFD02F},     {"info", 0x20DF55AA},     {"guide", 0x20DFD52A},
    {"volup", 0x20DF40BF},     {"voldown", 0x20DFC03F},   {"mute", 0x20DF906F},     {"chup", 0x20DF00FF},
    {"chdown", 0x20DF807F},    {"netflix", 0x20DF6A95},   {"prime", 0x20DF3AC5},    {"list", 0x20DFCA35},
    {"play", 0x20DF0DF2},      {"pause", 0x20DF5DA2},     {"stop", 0x20DF8D72},     {"rewind", 0x20DFF10E},
    {"ff", 0x20DF718E},        {"red", 0x20DF4EB1},       {"green", 0x20DF8E71},    {"yellow", 0x20DFC639},
    {"blue", 0x20DF8679},      {"picture", 0x20DFB24D},   {"sound", 0x20DF4AB5},    {"subtitles", 0x20DF9C63},
    {"launcher", 0x20DF42BD},  {"search", 0x20DF1EE1},    {"recent", 0x20DFAD52},   {"livetv", 0x20DFF00F},
    {"livemenu", 0x20DF7986},  {"record", 0x20DFBD42},    {"prevtrack", 0x20DF4DB2}, {"nexttrack", 0x20DFCD32},
    {"aspect", 0x20DF9E61},    {"screenremote", 0x20DF6996}, {"userguide", 0x20DF5EA1}, {"dash", 0x20DF32CD},
    {"hdmi1", 0x20DF738C},     {"hdmi2", 0x20DF33CC},     {"hdmi3", 0x20DF9768},    {"0", 0x20DF08F7},
    {"1", 0x20DF8877},         {"2", 0x20DF48B7},         {"3", 0x20DFC837},        {"4", 0x20DF28D7},
    {"5", 0x20DFA857},         {"6", 0x20DF6897},         {"7", 0x20DFE817},        {"8", 0x20DF18E7},
    {"9", 0x20DF9867},
    // Not here on purpose (docs/protocol/ir-keys.md): factory, hotel-mode and service codes, and 0x20DF7B84,
    // which only shows the "press the wheel to register the remote" notice.
};

uint32_t LGIRRemote::code_for(const std::string &name) {
  for (auto &k : KEYS)
    if (name == k.name)
      return k.code;
  return 0;
}

void LGIRRemote::dump_config() {
  ESP_LOGCONFIG(TAG, "LG IR remote:\n  Hold style: %s",
                this->hold_style_ == HOLD_REPEAT_CODES ? "NEC repeat codes" : "full frames");
}

bool LGIRRemote::press(const std::string &name, uint32_t hold_ms) {
  uint32_t code = code_for(name);
  if (code == 0) {
    ESP_LOGW(TAG, "no IR code for key '%s'", name.c_str());
    return false;
  }
  this->send(code, hold_ms);
  return true;
}

void LGIRRemote::send(uint32_t code, uint32_t hold_ms) { this->send(code, hold_ms, this->hold_style_); }

void LGIRRemote::send(uint32_t code, uint32_t hold_ms, HoldStyle style) {
  // A held NEC key starts a new frame every ~108 ms. Repeated whole frames are separate presses to the TV (a held
  // Back steps back once per frame); NEC repeat codes (9 ms / 2.25 ms / 0.56 ms) are one held key, which the TV
  // auto-repeats only where it makes sense (arrows, volume).
  uint32_t times = hold_ms ? std::max<uint32_t>(1, hold_ms / 108) : 1;
  ESP_LOGI(TAG, "IR LG 0x%08" PRIX32 " x%" PRIu32 "%s", code, times,
           times > 1 ? (style == HOLD_REPEAT_CODES ? " (repeat codes)" : " (full frames)") : "");
  auto call = this->tx_->transmit();
  auto *d = call.get_data();
  // LG frame: 8 ms / 4 ms header, 32 bits MSB first (0.6 ms mark, then 1.6 ms space for a 1 or 0.55 ms for a 0),
  // a closing mark; 38 kHz. The same timings as ESPHome's LG protocol, which the TV decodes.
  d->set_carrier_frequency(38000);
  d->mark(8000);
  d->space(4000);
  for (int bit = 31; bit >= 0; bit--) {
    d->mark(600);
    d->space((code >> bit) & 1 ? 1600 : 550);
  }
  d->mark(600);
  this->last_carrier_ = 38000;
  if (times > 1 && style == HOLD_REPEAT_CODES) {
    uint32_t len = 0;
    for (int32_t v : d->get_data())
      len += (uint32_t) (v > 0 ? v : -v);
    for (uint32_t i = 1; i < times; i++) {
      d->space(len < 108000 ? 108000 - len : 40000);
      d->mark(9000);
      d->space(2250);
      d->mark(560);
      len = 9000 + 2250 + 560;
    }
  } else {
    call.set_send_times(times);
    call.set_send_wait(40000);
  }
  call.perform();
}

void LGIRRemote::transmit_raw(const std::vector<int32_t> &d, uint32_t carrier, uint8_t duty) {
  // remote_transmitter applies the carrier duty only when the carrier frequency changes, so the duty is set first
  // and the carrier nudged by 1 Hz whenever it would equal the previous transmission's (irrelevant to any
  // receiver). LG keys use exactly 38000 Hz, which this keeps off, so they get reconfigured back to 50%.
  // A baseband code set (carrier 0) is sent as is.
  if (carrier == 38000)
    carrier = 38001;
  if (carrier != 0 && carrier == this->last_carrier_)
    carrier++;
  this->last_carrier_ = carrier;
  this->tx_->set_carrier_duty_percent(duty);
  auto call = this->tx_->transmit();
  auto *t = call.get_data();
  t->set_carrier_frequency(carrier);
  for (int32_t v : d) {
    if (v > 0)
      t->mark((uint32_t) v);
    else
      t->space((uint32_t) -v);
  }
  call.perform();
  this->tx_->set_carrier_duty_percent(50);
}

bool LGIRRemote::transmit_zipir(const uint8_t *file, size_t len, int key, bool repeat) {
  static zipir_wave_t w;  // 6 kB: not on the stack
  int rc = zipir_waveform(file, len, 0, key, this->toggle_, &w);
  if (rc != 0) {
    ESP_LOGW(TAG, "ZipIR key %d not decodable (%d)", key, rc);
    return false;
  }
  int order[3];
  int n = zipir_press_order(&w, order);
  if (n == 0)
    return false;
  std::vector<int32_t> d;
  for (int f = 0; f < n; f++)
    d.insert(d.end(), w.frame[order[f]], w.frame[order[f]] + w.frame_len[order[f]]);
  uint8_t duty = (w.duty >= 10 && w.duty <= 90) ? w.duty : 50;
  ESP_LOGI(TAG, "ZipIR key %d, %u Hz, duty %u%%, frames %d%d%d, %u durations%s", key, (unsigned) w.carrier_hz, duty,
           order[0], order[1], order[2], (unsigned) d.size(), repeat ? ", repeating" : "");
  this->transmit_raw(d, w.carrier_hz, duty);
  if (w.sqc > 1)
    this->toggle_ ^= 1;  // the toggle bit flips on every new press
  this->rep_frame_.clear();
  if (repeat) {
    // The repeat frame (R, or the last frame sent) goes again at its own period, which includes its gap.
    int j = w.frame_len[1] ? 1 : order[n - 1];
    this->rep_frame_.assign(w.frame[j], w.frame[j] + w.frame_len[j]);
    uint32_t period = 0;
    for (int32_t v : this->rep_frame_)
      period += (uint32_t) (v > 0 ? v : -v);
    this->rep_period_ = std::max<uint32_t>(20, period / 1000);
    this->rep_next_ = millis() + this->rep_period_;
    this->rep_carrier_ = w.carrier_hz;
    this->rep_duty_ = duty;
    this->rep_count_ = 0;
  }
  return true;
}

bool LGIRRemote::zipir_usable(const uint8_t *file, size_t len) {
  static zipir_wave_t w;
  // 0 = key 0 decoded, 1 = no key 0 but the code set itself decodes; negative = malformed or unsupported.
  return zipir_waveform(file, len, 0, 0, 0, &w) >= 0;
}

void LGIRRemote::stop_repeat() {
  if (this->rep_frame_.empty())
    return;
  ESP_LOGI(TAG, "ZipIR repeat stopped after %u frames", (unsigned) this->rep_count_);
  this->rep_frame_.clear();
}

void LGIRRemote::loop() {
  if (this->rep_frame_.empty())
    return;
  uint32_t now = millis();
  if ((int32_t) (now - this->rep_next_) < 0)
    return;
  this->transmit_raw(this->rep_frame_, this->rep_carrier_, this->rep_duty_);
  this->rep_count_++;
  this->rep_next_ = now + this->rep_period_;
}

}  // namespace esphome::lg_ir_remote
