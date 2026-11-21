#pragma once
// Infrared remote for LG webOS TVs: LG (NEC) key codes by name, held keys with NEC repeat codes, and raw / ZipIR
// (universal control) transmission. Works on its own, or as the infrared half of lg_magic_remote.
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/remote_transmitter/remote_transmitter.h"

#include <string>
#include <vector>

namespace esphome::lg_ir_remote {

/// IR code the TV treats as "register a Magic Remote": sent once while a Bluetooth remote
/// advertises, it makes the TV connect and pair.
static const uint32_t LG_IR_PAIRING = 0x20DFC13E;

enum HoldStyle : uint8_t {
  HOLD_FULL_FRAMES = 0,   // the whole frame again every ~108 ms (the TV sees separate presses)
  HOLD_REPEAT_CODES = 1,  // first frame, then NEC repeat codes (what an LG IR remote sends)
};

class LGIRRemote : public Component {
 public:
  void loop() override;
  void dump_config() override;

  void set_transmitter(remote_transmitter::RemoteTransmitterComponent *tx) { this->tx_ = tx; }
  void set_hold_style(HoldStyle s) { this->hold_style_ = s; }

  /// Send a 32-bit LG code, e.g. 0x20DF22DD (OK). hold_ms > 0 holds the key.
  void send(uint32_t code, uint32_t hold_ms = 0);
  void send(uint32_t code, uint32_t hold_ms, HoldStyle style);
  /// Send a key by name ("ok", "power", "hdmi1", "0".."9", ...). Returns false for an unknown name.
  bool press(const std::string &name, uint32_t hold_ms = 0);
  /// The LG code for a key name, 0 if unknown.
  static uint32_t code_for(const std::string &name);
  /// Ask the TV to register a Bluetooth Magic Remote that is advertising right now.
  void send_pairing() { this->send(LG_IR_PAIRING); }

  /// Raw durations in microseconds (+ = mark, - = space) on `carrier` Hz at `duty` percent.
  void transmit_raw(const std::vector<int32_t> &durations, uint32_t carrier, uint8_t duty);
  /// Transmit key `key` of the first code set in a UEI ZipIR file (universal control). With `repeat`, the key's
  /// repeat frame keeps going until stop_repeat(). Returns false if the key can't be decoded.
  bool transmit_zipir(const uint8_t *file, size_t len, int key, bool repeat);
  void stop_repeat();
  /// True if the first code set of a ZipIR file decodes (protocol and key parts), so it's worth storing.
  static bool zipir_usable(const uint8_t *file, size_t len);
  bool repeating() const { return !this->rep_frame_.empty(); }

 protected:
  remote_transmitter::RemoteTransmitterComponent *tx_{nullptr};
  HoldStyle hold_style_{HOLD_REPEAT_CODES};
  uint32_t last_carrier_{0};  // carrier of the last transmission (see transmit_raw)
  uint8_t toggle_{0};         // RC5/RC6-style toggle state for ZipIR code sets that have one

  std::vector<int32_t> rep_frame_;
  uint32_t rep_period_{0}, rep_next_{0}, rep_carrier_{0}, rep_count_{0};
  uint8_t rep_duty_{50};
};

template<typename... Ts> class SendAction : public Action<Ts...>, public Parented<LGIRRemote> {
 public:
  TEMPLATABLE_VALUE(uint32_t, code)
  TEMPLATABLE_VALUE(uint32_t, hold)
  void play(const Ts &...x) override { this->parent_->send(this->code_.value(x...), this->hold_.value(x...)); }
};

template<typename... Ts> class PressAction : public Action<Ts...>, public Parented<LGIRRemote> {
 public:
  TEMPLATABLE_VALUE(std::string, key)
  TEMPLATABLE_VALUE(uint32_t, hold)
  void play(const Ts &...x) override { this->parent_->press(this->key_.value(x...), this->hold_.value(x...)); }
};

}  // namespace esphome::lg_ir_remote
