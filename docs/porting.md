# Porting to other TVs and remotes

Everything here was worked out on one TV, a 2019 LG OLED C9 (webOS 4.5). Other webOS years may want a different
remote name, other F9 replies, or another wake packet. These tools help you find out. Please open an issue with
what you learn: TV model, webOS version, and what worked.

## What to check first on another TV

| Feature | How to tell it works |
|---|---|
| Pairing | **Bluetooth connected** turns on after **Pair with TV**; the log shows "TV subscribed to FD" |
| Keys | arrows move the focus; `key_code` with any code from [protocol/bluetooth.md](protocol/bluetooth.md) |
| Pointer | the log shows the TV requesting the calibration (F9 `17`), then `move_pointer` moves the pointer |
| Voice | **TV voice** playing a word opens the voice UI; the log shows F9 `03` then F9 `04` |
| Wake | after the registration in [protocol/wake.md](protocol/wake.md), **Wake TV (Bluetooth)** from standby |

If keys work but the pointer never moves, the TV most likely rejected the calibration block. Check the log around
F9 `17`, and try `device_name` / `manufacturer_string` values matching your TV's own remote.

## Debug actions (`packages/debug.yaml`)

| Action | Use |
|---|---|
| `inject_f9` (hex) | handle bytes as if the TV had written them to F9, e.g. to test universal-control replies |
| `adv_burst` (hex, seconds, pdu) | advertise arbitrary AD structures, e.g. to try wake packets |
| `wake_tv_pdu` (seconds, pdu) | the wake burst with another PDU type |
| `gyro` (g0, g1, g2, ms), `shake` | raw motion: pointer calibration |
| `glide`, `jitter`, `gravity`, `motion_period` | `move_pointer` tuning ([protocol/pointer.md](protocol/pointer.md)) |
| `scd_mode` (mode) | 1 = don't answer the calibration request, 2 = don't answer the version request |
| `bt_reconnect` | drop the link; the TV reconnects |
| `capture`, `capture_cmd` | capture mode, below |

**Dump Bluetooth log** prints the last 60 Bluetooth events with their uptime. That includes events from before the
Wi-Fi log stream attached, such as a reconnect at boot.

## Serial commands (`debug: serial_commands: true`)

On chips with USB Serial/JTAG, lines typed on the USB console run commands:

- **Status and pairing:** `status`, `pair`, `forget`, `disconnect`, `reconnect`.
- **Keys and input:** `key <hex> [ms]`, a key name with an optional hold (`home 1500`), `wheel <n>`,
  `gyro <g0> <g1> <g2> <ms>`, `move <dx> <dy>`.
- **Raw reports:** `f9 <hex>`, `fd <hex>`.
- **Advertising:** `adv on|off`, `name <s>`, `mfg <s>`, `flags <hex>`, `hid on|off`, `nameadv on|off`.
- **IR:** `ir <hex> [ms]` (IR linked).
- **Capture:** `cap start|stop|dump|forget`, `capcmd <hex>`.

## Capture mode (`debug: capture: true`, [`examples/capture.yaml`](../examples/capture.yaml))

The ESP32 plays the TV for a genuine LG Magic Remote and logs what it sends:

- its advertisement;
- its whole GATT table and every readable value;
- its replies to the TV's F9 requests (`13`, `11`, `19`, `90`, `17`, then `01`);
- its key and motion reports, kept in RAM for a paced dump (`FDLOG` lines).

It only connects to devices named `LGE MR…`, or with `SCD` in their LG manufacturer data, so it leaves the TV
alone. It also logs every advertiser stronger than −75 dBm.

1. Turn the TV off, so it can't grab the remote.
2. Wake the remote. Many remotes need Home + Back held to reset them into pairing.
3. Run the HA action `capture` with `what: start`, then press keys and move the remote.
4. Run `capture` with `what: dump` and save the log.

Capture mode builds the NimBLE observer role and a second connection in, so it costs some RAM; leave it off
otherwise.
