# Waking the TV from standby over Bluetooth

In standby the TV drops the Magic Remote's Bluetooth link, so no key can be sent over Bluetooth. A real Magic
Remote turns the TV on with IR. This project can also wake the TV over Bluetooth, using LG's "Turn on via
Bluetooth" feature that is meant for phones.

Everything here was worked out on **one TV: an LG OLED C9 (2019, webOS 4.5, firmware 05.50.00)**. Labels as in
[bluetooth.md](bluetooth.md#scope-and-evidence): **Tested** (on the C9, 2026-11-20), **Firmware** (read in the
C9's firmware), **Inferred**.

## How it works

Before entering standby, the TV programs its Bluetooth controller with a filter for LG manufacturer data that
contains a registered 6-byte ID. The controller keeps scanning while the rest of the TV sleeps, and powers the TV
on when an advertisement matches (Firmware). The remote only has to broadcast that advertisement for a few
seconds. No connection is made.

## What a real Magic Remote does

Nothing of the kind. An MR19's firmware sends Power as IR only, in every state. It never advertises a wake
pattern, and the directed advertising it uses to reconnect carries no data the filter could match (Firmware, from
the remote's firmware). So Bluetooth wake is something a real remote cannot do; it borrows the phone feature.

## Requirements on the TV

### 1. Turn on via Bluetooth

Settings > General > Mobile TV On > **Turn on via Bluetooth** must be on (Firmware). The underlying setting is
`bleAdvertisingOnOff` = `"on"` in the `network` category; the default is `"off"` (Firmware). On the tested TV it
was already on.

### 2. Register the remote's ID, once, as type ThinQ

The ID is registered through the TV's local service `luna://com.webos.service.asc/registerDevice`:

```json
{"address": "AA:BB:CC:DD:EE:FF", "type": "ThinQ"}
```

- `address` is exactly 17 characters, six hex bytes separated by colons. Nothing checks that a device with that
  address exists (Firmware).
- The ID does not have to be the advertiser's Bluetooth address: the controller filter ignores the advertiser
  address (Firmware). A test ID different from the remote's address woke the TV (Tested). This project uses its
  own Bluetooth address (or the `address:` option) as the ID, which keeps it recognisable.
- `"type": "ThinQ"` matters; see [the short form](#the-short-form-and-why-not-to-use-it).
- The TV keeps up to 10 registrations and drops the oldest when full (Firmware).

#### Through Home Assistant

The TV's second-screen interface (SSAP, the network API that Home Assistant's `webostv` integration uses) exposes
`registerDevice` to clients with the `WRITE_SETTINGS` permission, which Home Assistant requests when it pairs
(Firmware). With Home Assistant's existing pairing the call returned `returnValue: true` (Tested). Run it once,
for example from Developer tools > Actions:

```yaml
action: webostv.command
data:
  entity_id: media_player.your_lg_tv
  command: com.webos.service.asc/registerDevice
  payload:
    address: "AA:BB:CC:DD:EE:FF"
    type: ThinQ
```

Any other SSAP client paired with that permission can send the same request (`ssap://com.webos.service.asc/registerDevice`
with the same payload) (Inferred).

#### Removing a registration

`unRegisterDevice` (payload `{"address": "…"}`) is **not** reachable over SSAP: the TV answers 404 "no such service
or method" (Tested). It can be reached as a local `luna://` call, for example through a webOS notification alert
whose close action is that call (the "alert trick" some webOS tools use), or from a Developer Mode app
(Tested). The settings item "Delete Pairing History" probably clears registrations too (Inferred).

## The wake advertisement

| Item | Value |
|---|---|
| PDU | Legacy, non-connectable, undirected (`ADV_NONCONN_IND`) |
| Interval | 20 ms, all three advertising channels |
| Duration | About 4 s (tested with 4-5 s) |
| When | Only while the TV is not connected (a connected TV is on) |

Advertising data, 21 bytes:

| Bytes | Content |
|---|---|
| `02 01 06` | Flags |
| `11 FF` | Manufacturer-specific data, length `0x11` (17 bytes follow) |
| `C4 00` | Company ID `0x00C4`, LG Electronics |
| 6 bytes | The registered ID, in the order it is written in the registration string |
| `07 00` | Fixed marker |
| 6 bytes | The TV's own Bluetooth address, most significant byte first |

For the example registration above and a TV with Bluetooth address `<TV address>`:

```
02 01 06 11 FF C4 00 AA BB CC DD EE FF 07 00 <TV address, 6 bytes, MSB first>
```

This project takes the TV's address from its bond with the TV, so the remote must have been registered with the
TV first. Do not put a second manufacturer-data field in the same packet (Firmware).

## The short form, and why not to use it

A registration without `type` gives a shorter pattern without the TV address:
`02 01 06 0B FF C4 00 <ID> 07 00` (Firmware).

On the C9 it **never woke the TV from deep standby**: not as `ADV_NONCONN_IND`, `ADV_IND` or `ADV_SCAN_IND`, not
with the ID bytes reversed, not in a 10 s burst (Tested). It did wake the TV within about 1 s when sent in the
first minute after switching off, while the TV's own software is still scanning (Tested). Whether the controller
filter is only armed for ThinQ registrations or the short pattern is not what it matches was not established.

This project sends the short form only when it has no bond with a TV, and logs a warning.

In that first minute the TV's software also checks the long form, by comparing the TV address in the packet with
its own as text; whether the case matches was not established, and the long form was only tested from deep standby.
So with `lg_ir_remote` linked, **Power** and **Power on** in standby send the discrete IR power-on as well, which a
TV that is already on ignores.

## Results on the C9 (2026-11-20)

Each deep-standby trial waited 5 minutes after the TV was switched off.

| Test | Result |
|---|---|
| Long form, registered ID (twice with a test ID, once with the remote's own ID) | Woke the TV 3 times out of 3, about 16 s after the burst (a full boot) |
| Long form, unregistered ID | TV stayed off |
| Long form, ID that had been unregistered | TV stayed off |
| Short form (several variants) | Never woke the TV from deep standby |
| Short form, within about 60 s of switching off | Woke the TV within about 1 s |

## In this component

- The `wake_tv` action sends the long form for 4 s (configurable).
- The Power key sends `0x0008` over Bluetooth while connected (the TV turns off). While disconnected it sends the
  wake burst, and IR only if the burst cannot be sent.
- Once the TV is up it reconnects to the remote by itself.
