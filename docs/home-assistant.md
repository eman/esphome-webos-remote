# Home Assistant

The ESP32 shows up through the ESPHome integration. What the packages add:

| Package | Entities | Actions (`esphome.<device>_<action>`) |
|---|---|---|
| `ir_base.yaml` | none | `ir_code` (code, hold_ms), `ir_key` (name, hold_ms) |
| `ir.yaml` | `ir_base` + a button per IR key | (as `ir_base`) |
| `bluetooth_base.yaml` | **Bluetooth connected**, **Pair with TV**, **Forget TV**, **Wake TV (Bluetooth)**, **Dump Bluetooth log** | `key` (name), `key_code` (code, hold_ms), `move_pointer` (dx, dy), `scroll` (notches), `wake_tv` |
| `bluetooth.yaml` | `bluetooth_base` + a button per Magic Remote key | (as `bluetooth_base`) |
| `voice.yaml` | **TV voice** media player | |
| `debug.yaml` | | test actions, see [porting.md](porting.md) |

Codes are passed as decimal integers, e.g. `551494365` for `0x20DF22DD`.

## Voice

Anything played on **TV voice** reaches the TV's voice recognition. For example:

```yaml
action: tts.speak
target:
  entity_id: tts.google_translate_en_com
data:
  media_player_entity_id: media_player.lg_tv_remote_tv_voice
  message: Netflix
```

The TV opens the voice UI, listens, and acts on what it hears. It must be connected over Bluetooth.

## Turning the TV on and off

- **Power off** and **Power on** are idempotent: each only ever turns the TV off or on.
- **Power on**, **Power** and **Wake TV (Bluetooth)** wake a TV in standby over Bluetooth. This needs the one-time
  registration in [protocol/wake.md](protocol/wake.md). Waking takes about 16-20 s.
- **Power off** turns the TV off over Bluetooth. In standby it sends the discrete IR off, which does nothing to a TV
  that is already off.
