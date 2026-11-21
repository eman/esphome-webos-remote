#pragma once
#include "esphome/core/automation.h"
#include "lg_magic_remote.h"

namespace esphome::lg_magic_remote {

template<typename... Ts> class KeyAction : public Action<Ts...>, public Parented<LGMagicRemote> {
 public:
  TEMPLATABLE_VALUE(std::string, key)
  void play(const Ts &...x) override { this->parent_->key(this->key_.value(x...)); }
};

template<typename... Ts> class PressCodeAction : public Action<Ts...>, public Parented<LGMagicRemote> {
 public:
  TEMPLATABLE_VALUE(uint16_t, code)
  TEMPLATABLE_VALUE(uint32_t, hold)
  void play(const Ts &...x) override {
    uint32_t hold = this->hold_.value(x...);
    this->parent_->press(this->code_.value(x...), hold ? hold : 80);
  }
};

template<typename... Ts> class MovePointerAction : public Action<Ts...>, public Parented<LGMagicRemote> {
 public:
  TEMPLATABLE_VALUE(int, dx)
  TEMPLATABLE_VALUE(int, dy)
  void play(const Ts &...x) override { this->parent_->move_pointer(this->dx_.value(x...), this->dy_.value(x...)); }
};

template<typename... Ts> class ScrollAction : public Action<Ts...>, public Parented<LGMagicRemote> {
 public:
  TEMPLATABLE_VALUE(int, notches)
  void play(const Ts &...x) override { this->parent_->wheel((int8_t) this->notches_.value(x...)); }
};

template<typename... Ts> class WakeAction : public Action<Ts...>, public Parented<LGMagicRemote> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->wake_tv(this->duration_.value(x...)); }
};

template<typename... Ts> class PairAction : public Action<Ts...>, public Parented<LGMagicRemote> {
 public:
  void play(const Ts &...x) override { this->parent_->pair(); }
};

template<typename... Ts> class ForgetAction : public Action<Ts...>, public Parented<LGMagicRemote> {
 public:
  void play(const Ts &...x) override { this->parent_->forget(); }
};

}  // namespace esphome::lg_magic_remote
