# Configuration

## `lg_ir_remote`

```yaml
remote_transmitter:
  id: ir_tx
  pin: GPIO7
  carrier_duty_percent: 50%

lg_ir_remote:
  id: lg_ir
  transmitter_id: ir_tx       # required
  hold_style: repeat_codes    # or full_frames
```

| Option | Default | Meaning |
|---|---|---|
| `transmitter_id` | (required) | the `remote_transmitter` driving the IR LED |
| `hold_style` | `repeat_codes` | how a held key is sent: `repeat_codes` sends the frame once, then NEC repeat codes, which the TV treats as one held key. `full_frames` repeats the whole frame every 108 ms, and the TV sees separate presses (a held Back steps back several times). |

Actions:

```yaml
- lg_ir_remote.press: { id: lg_ir, key: hdmi1 }             # by name; hold: 1s holds it
- lg_ir_remote.send: { id: lg_ir, code: 0x20DF22DD, hold: 0ms } # any 32-bit LG code
```

From a lambda: `id(lg_ir).press("ok")`, `id(lg_ir).send(0x20DF22DD, hold_ms)`, `id(lg_ir).send_pairing()`.

## `lg_magic_remote`

```yaml
lg_magic_remote:
  id: lg_bt
  ir_remote_id: lg_ir          # optional: IR fallback, IR pairing, universal control
  device_name: "LGE MR23"
  manufacturer_string: "SCD 21.2,BA 35,webOS"
  advertise_on_boot: true
  address: AA:BB:CC:DD:EE:FF   # optional
  debug:
    capture: false
    serial_commands: false
```

| Option | Default | Meaning |
|---|---|---|
| `ir_remote_id` | none | an `lg_ir_remote`. Keys fall back to IR while the TV isn't connected, Pair sends the IR pairing code, and the TV's universal-control commands are transmitted as IR. Without it, the remote answers universal-control commands with "not supported". |
| `device_name` | `LGE MR23` | the Bluetooth name the TV sees. `LGE MR23` (LG's MR20GA replacement remote) is the tested one. As `LGE MR19BA`, the C9 ignored the Netflix and Prime Video keys, although its firmware classes both names as the same remote type. |
| `manufacturer_string` | `SCD 21.2,BA 35,webOS` | LG manufacturer data in the advertisement: the calibration (SCD) and battery versions, as an MR20GA advertises them |
| `advertise_on_boot` | `true` | advertise at boot, so the TV can reconnect |
| `address` | the chip's own | Bluetooth public address. The TV identifies the remote by this address, for the bond and for wake from standby (docs/protocol/wake.md), so changing it means pairing again. |
| `debug.capture` | `false` | build capture mode (docs/porting.md) |
| `debug.serial_commands` | `false` | line commands on the USB serial console, on chips with USB Serial/JTAG |

Actions:

```yaml
- lg_magic_remote.key: { id: lg_bt, key: home }              # by name, Bluetooth or IR fallback
- lg_magic_remote.press_code: { id: lg_bt, code: 0x8044, hold: 1500ms }  # any Magic Remote code
- lg_magic_remote.move_pointer: { id: lg_bt, dx: 300, dy: -100 } # pixels of the 1920x1080 UI
- lg_magic_remote.scroll: { id: lg_bt, notches: -3 }         # + = up
- lg_magic_remote.wake_tv: lg_bt                               # wake from standby over Bluetooth
- lg_magic_remote.pair: lg_bt
- lg_magic_remote.forget: lg_bt
```

From a lambda: `id(lg_bt).key("home")`, `id(lg_bt).press(0x8044, 80)`, `id(lg_bt).move_pointer(dx, dy)`,
`id(lg_bt).wheel(n)`, `id(lg_bt).is_connected()`, `id(lg_bt).pointer_active()`.

How `key` sends:

- **While the TV is connected:** over Bluetooth. Keys without a Bluetooth code (the HDMI inputs) go over IR.
- **While it isn't:** over IR when an `lg_ir_remote` is linked and the key has an IR code. Otherwise the key waits
  up to 8 s for the TV to reconnect and is then sent over Bluetooth, as a real Magic Remote does. Either way the
  remote asks the TV to reconnect.
- **Power keys:**
  - `power` and `poweron` wake a TV in standby over Bluetooth (docs/protocol/wake.md) and, with IR linked, also
    send the discrete IR power-on.
  - `poweron` does nothing while the TV is connected.
  - `poweroff` turns a connected TV off over Bluetooth, and otherwise sends the discrete IR off, which leaves a TV
    in standby as it is.

### Voice: `speaker` platform

```yaml
speaker:
  - platform: lg_magic_remote
    id: lg_voice_speaker
    lg_magic_remote_id: lg_bt
    buffer_duration: 250ms
```

Feed it from a `speaker` media player at 16 kHz mono WAV ([`packages/voice.yaml`](../packages/voice.yaml)).
Whatever plays reaches the TV's voice recognition as if spoken into the remote. Details:
[protocol/voice.md](protocol/voice.md).

## Key names

The same names work for `lg_magic_remote.key` and `lg_ir_remote.press`.

| Key name | Button | Bluetooth | IR |
|---|---|---|---|
| `power` | Power | yes | yes |
| `poweron` | Power on | yes | yes |
| `poweroff` | Power off | yes | yes |
| `up` | Up | yes | yes |
| `down` | Down | yes | yes |
| `left` | Left | yes | yes |
| `right` | Right | yes | yes |
| `ok` | OK | yes | yes |
| `back` | Back | yes | yes |
| `exit` | Exit | yes | yes |
| `home` | Home | yes | yes |
| `settings` | Settings | yes | yes |
| `more` | Quick menu | yes | yes |
| `input` | Input | yes | yes |
| `info` | Info | yes | yes |
| `guide` | Guide | yes | yes |
| `volup` | Volume up | yes | yes |
| `voldown` | Volume down | yes | yes |
| `mute` | Mute | yes | yes |
| `chup` | Channel up | yes | yes |
| `chdown` | Channel down | yes | yes |
| `netflix` | Netflix | yes | yes |
| `prime` | Prime Video | yes | yes |
| `list` | List | yes | yes |
| `play` | Play | yes | yes |
| `pause` | Pause | yes | yes |
| `stop` | Stop | yes | yes |
| `rewind` | Rewind | yes | yes |
| `ff` | Fast forward | yes | yes |
| `red` | Red | yes | yes |
| `green` | Green | yes | yes |
| `yellow` | Yellow | yes | yes |
| `blue` | Blue | yes | yes |
| `picture` | Picture mode | yes | yes |
| `sound` | Sound mode | yes | yes |
| `subtitles` | Subtitles | yes | yes |
| `launcher` | Launcher | yes | yes |
| `search` | Search | yes | yes |
| `recent` | Recent | yes | yes |
| `livetv` | Live TV | yes | yes |
| `livemenu` | Live menu | yes | yes |
| `record` | Record | yes | yes |
| `prevtrack` | Previous track | yes | yes |
| `nexttrack` | Next track | yes | yes |
| `aspect` | Aspect ratio | yes | yes |
| `advsettings` | Advanced settings | yes |  |
| `inputhub` | Input hub | yes |  |
| `screenremote` | Screen remote | yes | yes |
| `userguide` | User guide | yes | yes |
| `voice` | Voice | yes |  |
| `dash` | Dash | yes | yes |
| `hdmi1` | HDMI 1 |  | yes |
| `hdmi2` | HDMI 2 |  | yes |
| `hdmi3` | HDMI 3 |  | yes |
| `hdmi4` | HDMI 4 |  | yes |
| `simplink` | SimpLink | | yes |
| `0` | 0 | yes | yes |
| `1` | 1 | yes | yes |
| `2` | 2 | yes | yes |
| `3` | 3 | yes | yes |
| `4` | 4 | yes | yes |
| `5` | 5 | yes | yes |
| `6` | 6 | yes | yes |
| `7` | 7 | yes | yes |
| `8` | 8 | yes | yes |
| `9` | 9 | yes | yes |

A few keys are never sent, on purpose:

- **Set-top power `0x8099` and set-top menu `0x8098`:** they open a dialog that swallows every key.
- **Factory, hotel-mode and service codes:** see [protocol/ir-keys.md](protocol/ir-keys.md).
