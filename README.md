# esphome-webos-remote

ESPHome components that turn an ESP32 into a remote for LG webOS TVs, controlled from Home Assistant:

- **`lg_magic_remote`: Bluetooth.** The ESP32 registers with the TV as its Bluetooth **Magic Remote**. Keys work
  without line of sight. It also drives the pointer and the scroll wheel, sends voice from Home Assistant TTS, and
  wakes the TV from standby. With the IR component linked, it also carries out the TV's universal control of other
  devices.
- **`lg_ir_remote`: infrared.** Sends every LG IR key by name or code, including discrete power and HDMI inputs.
  Held keys are sent the way an LG remote sends them, as NEC repeat codes. It can also transmit UEI ZipIR code sets.

Each component works alone; use one or both.

> **Tested on one TV so far:** an LG OLED C9 (2019, webOS 4.5, firmware 05.50.00). Other webOS years probably work
> for IR. For Bluetooth, the pairing, pointer and wake details may differ. Reports welcome: see
> [docs/porting.md](docs/porting.md).

## What you need

| | Bluetooth | Infrared |
|---|---|---|
| Chip | ESP32-C3, C5, C6, H2 or S3 (needs Bluetooth 5 extended advertising; not the original ESP32) | any ESP32 |
| Extra hardware | none | an IR LED module on a GPIO (default GPIO7) |
| Line of sight to the TV | no | yes |
| Combines with `esp32_ble`, `bluetooth_proxy` | no (it uses NimBLE directly) | yes |

ESPHome 2026.11.0 or later (the tested version), ESP-IDF framework. Wiring and boards:
[docs/hardware.md](docs/hardware.md).

## Quick start

Copy one of [`examples/`](examples) next to a `secrets.yaml` (see
[`examples/secrets.example.yaml`](examples/secrets.example.yaml)), then flash it:

| Example | Packages | Gives you |
|---|---|---|
| [`ir-only.yaml`](examples/ir-only.yaml) | `ir` | an IR remote: a button per key, plus actions for any code |
| [`bluetooth-only.yaml`](examples/bluetooth-only.yaml) | `bluetooth` | a Bluetooth Magic Remote: a button per key, pointer, scroll, wake |
| [`capture.yaml`](examples/capture.yaml) | `bluetooth_base`, `debug` | capture a real Magic Remote's reports (see [docs/porting.md](docs/porting.md)) |

`ir.yaml` and `bluetooth.yaml` each bundle the engine (`ir_base.yaml` / `bluetooth_base.yaml`) and one button per
key (`ir_buttons.yaml` / `bluetooth_buttons.yaml`); add `voice.yaml` for voice. Include `ir.yaml` for an IR
remote, `bluetooth.yaml` for a Bluetooth one.

The packages import the components from this repository:

```yaml
packages:
  webos_remote:
    url: https://github.com/eman/esphome-webos-remote
    ref: v0.1.0
    files:
      - packages/bluetooth.yaml
```

For the Bluetooth remote, pair it with the TV once; see [docs/pairing.md](docs/pairing.md).

You can also use the components directly, without the packages:

```yaml
external_components:
  - source: github://eman/esphome-webos-remote@v0.1.0
    components: [lg_ir_remote, lg_magic_remote]

remote_transmitter:
  id: ir_tx
  pin: GPIO7
  carrier_duty_percent: 50%

lg_ir_remote:
  id: lg_ir
  transmitter_id: ir_tx

lg_magic_remote:
  id: lg_bt
  ir_remote_id: lg_ir   # optional

button:
  - platform: template
    name: Home
    on_press:
      - lg_magic_remote.key: { id: lg_bt, key: home }
```

## Documentation

- [Configuration](docs/configuration.md): options, actions, key names
- [Pairing](docs/pairing.md): registering the Bluetooth remote with the TV, and recovering from failures
- [Hardware](docs/hardware.md): boards, IR LED, optional IR receiver for loopback
- [Home Assistant](docs/home-assistant.md): entities, actions, voice from TTS, waking the TV
- [Porting](docs/porting.md): debug tools and capture mode, for other TVs
- Protocol notes: [Bluetooth](docs/protocol/bluetooth.md), [pointer](docs/protocol/pointer.md),
  [wake](docs/protocol/wake.md), [voice](docs/protocol/voice.md), [IR keys](docs/protocol/ir-keys.md),
  [universal control](docs/protocol/universal-control.md)

## Credits

These projects documented parts of the Magic Remote protocol and were used as references:
[wittenbe/magic-remote-bridge](https://github.com/wittenbe/magic-remote-bridge) (MR25GB GATT layout and replies),
[Haplois/uc3-airmouse](https://github.com/Haplois/uc3-airmouse) (FD report layout),
[brainrom/lg-magic](https://github.com/brainrom/lg-magic) (the MR20GA calibration block and version reply,
published in its issue #9 and PR #8).

The IR codes come from [ledoge's LG IR list](https://gist.github.com/ledoge/ab78723efdaa00e6815e30d1792790fd) and
Tasmota's LG code list. The mSBC encoder is ESP-IDF's Bluedroid SBC encoder (Apache-2.0, see [NOTICE](NOTICE)).

Not affiliated with LG Electronics. "LG", "webOS" and "Magic Remote" are trademarks of LG Electronics.

## License

MIT, see [LICENSE](LICENSE). The SBC encoder files (`components/lg_magic_remote/sbc_*`) are Apache-2.0.
