# Changelog

## 0.1.0 (2026-11-20)

First release, tested on an LG OLED C9 (2019, webOS 4.5).

- `lg_magic_remote`: Bluetooth Magic Remote. Keys, pointer, scroll wheel, voice from Home Assistant TTS
  (speaker platform), wake from standby, universal control with `lg_ir_remote`. Debug tools and capture mode
  behind `debug:`.
- `lg_ir_remote`: LG IR keys by name and code, held keys as NEC repeat codes, ZipIR code-set transmission with
  each code set's carrier and duty cycle.
- Packages for IR only, Bluetooth only, or both, with generated Home Assistant buttons.
