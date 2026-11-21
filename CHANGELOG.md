# Changelog

## 0.1.0 (2026-11-20)

First release, tested on an LG OLED C9 (2019, webOS 4.5).

- `lg_magic_remote`: Bluetooth Magic Remote. Keys, pointer, scroll wheel, voice from Home Assistant TTS
  (speaker platform), wake from standby, universal control with `lg_ir_remote`. Debug tools and capture mode
  behind `debug:`.
- `lg_ir_remote`: LG IR keys by name and code, held keys as NEC repeat codes, ZipIR code-set transmission with
  each code set's carrier and duty cycle.
- `lg_ir_remote` refuses hotel, factory, service and set-top codes unless `allow_unsafe_codes: true`.
- A key pressed while the TV is reconnecting is sent once the link is back (up to 8 s), as a real Magic Remote does.
- Power and Power on in standby also send the discrete IR power-on when IR is linked.
- Packages for IR only, Bluetooth only, or both, with generated Home Assistant buttons.
