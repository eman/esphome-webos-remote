# Pairing the Bluetooth remote

The TV accepts one Bluetooth Magic Remote at a time. Registering this one replaces any other.

## First time

1. Flash the firmware. With `advertise_on_boot: true` (the default), the ESP32 starts advertising as
   `LGE MR23`.
2. Turn the TV on.
3. Press **Pair with TV** in Home Assistant, or run `lg_magic_remote.pair`.
   - **With `bluetooth_ir.yaml` (IR linked):** the ESP32 sends the IR pairing code `0x20DFC13E` itself. The IR LED
     must reach the TV.
   - **Bluetooth only:** have something send `0x20DFC13E` once while the ESP32 advertises: any LG-capable IR blaster
     (another ESPHome node with `lg_ir_remote`, a Broadlink, …). On the tested TV, the code is the only trigger.
4. The TV connects, pairs (LE Secure Connections, no PIN), reads the HID layout and subscribes. This takes a few
   seconds. **Bluetooth connected** turns on.
5. The TV asks for the motion calibration and creates its pointer device. After that, `move_pointer` works.

The bond lives in the ESP32's NVS and survives firmware updates. After a TV power cycle or an ESP32 reboot, the
TV reconnects by itself within seconds. The first attempt after the TV boots sometimes drops, and the next one
succeeds.

## If pairing fails

- **The TV rate-limits failed pairings.** After a few failures it refuses (SMP error 0x09, Repeated Attempts) and
  ignores the pairing code until it is power-cycled. To recover:
  1. Press **Forget TV**.
  2. Turn the TV off and on.
  3. Wait 20 s.
  4. Press **Pair with TV**.
- **Forget TV** tells the TV to drop this remote first (F9 `40`), then deletes the ESP32's bond. Both sides then
  start clean.
- **Changing the `address`** (or flashing a new chip) means pairing again. The TV identifies the remote by its
  address.
- **Don't use `0x20DF7B84`.** Some IR lists call it "register Magic Remote", but on the tested TV it only shows a
  "press the wheel to register" notice.

## Wake from standby

Waking the TV over Bluetooth needs a separate, one-time registration on the TV. See
[protocol/wake.md](protocol/wake.md).
