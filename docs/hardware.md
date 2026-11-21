# Hardware

## Boards

| | Bluetooth (`lg_magic_remote`) | Infrared (`lg_ir_remote`) |
|---|---|---|
| ESP32-C6 | tested (ESP32-C6 SuperMini) | tested |
| ESP32-C3, C5, H2, S3 | should work; S3 compiles in CI | should work |
| ESP32 (original), S2 | no: Bluetooth 5 extended advertising is required (S2 has no Bluetooth) | compiles in CI |

The Bluetooth component uses NimBLE directly, so it can't share the chip with `esp32_ble`,
`esp32_ble_tracker` or `bluetooth_proxy`.

With voice from HTTPS media (Home Assistant TTS), Bluetooth, Wi-Fi and TLS together need mbedTLS dynamic buffers
in the `esp32: framework: sdkconfig_options:` block (see [`packages/voice.yaml`](../packages/voice.yaml)).

## IR LED

- **Wiring:** use an IR LED module with a driver transistor, or an IR LED with an NPN transistor (or a logic-level
  MOSFET) and a series resistor sized for the LED's current. A GPIO can't drive an IR LED usefully on its own.
- **Pin:** the packages use GPIO7, which you can change with the `ir_tx_pin` substitution. On the original ESP32,
  GPIO6-11 are flash pins.
- **Range:** like any IR remote, it needs line of sight to the TV's receiver or a good reflection.

The transmitter's `carrier_duty_percent: 50%` is the default. `lg_ir_remote` switches to each universal-control
code set's own duty cycle while sending it.

## Optional: IR receiver for loopback

An IR receiver module (e.g. VS1838B) next to the LED shows in the log what was actually sent:

```yaml
remote_receiver:
  pin:
    number: GPIO6
    inverted: true
    mode:
      input: true
      pullup: true
  dump: [lg, raw]
  tolerance: 25%
  filter: 400us   # the VS1838B breaks some marks with 100-400 us dropouts; without this LG frames don't decode
  idle: 25ms
```

## ESP32-C6 SuperMini notes

- The blue LED (GPIO15) and the RGB LED (WS2812, GPIO8) are free for your own use. If you use the RGB LED with
  `esp32_rmt_led_strip`, set `rmt_symbols: 48`. The default of 96 takes all of the C6's transmit RMT memory, and the
  IR transmitter then fails with "out of RMT symbol memory".
- The green LED that blinks with no battery attached is the charger's status LED. No GPIO controls it.
