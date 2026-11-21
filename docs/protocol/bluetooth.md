# Magic Remote Bluetooth protocol

This page describes how an LG webOS TV talks to its Bluetooth Magic Remote, as far as this project needs it:
advertising, registration, pairing, the HID reports, key codes and the remote-management commands (report `F9`).

Related pages: [pointer](pointer.md), [voice](voice.md), [wake from standby](wake.md),
[universal control](universal-control.md).

## Scope and evidence

Everything here was worked out on **one TV: an LG OLED C9 (2019, webOS 4.5, firmware 05.50.00)**. Other models and
firmware versions may differ. Each statement carries one of these labels:

| Label | Meaning |
|---|---|
| **Tested** | Observed on the C9 with this component (dates are 2026-11) |
| **Firmware** | Read in the C9's firmware image (static analysis), not separately tested |
| **Inferred** | Follows from the above but was not checked end to end |
| **Copied** | Taken from captures of real remotes published by other projects; works on the C9, meaning unknown |

Byte offsets on this page count from the start of the GATT characteristic value, which does **not** include the
HID report ID. The TV's HID stack adds the report ID in front, so the TV sees each report one byte longer.

## Advertising

| Item | Value | Status |
|---|---|---|
| Flags | `0x05` (LE Limited Discoverable, BR/EDR not supported), as a real remote | Tested |
| Manufacturer data | Company `0x00C4` (LG Electronics), then ASCII `SCD 21.2,BA 35,webOS` | Tested |
| Name | Complete local name in the scan response, default `LGE MR23` | Tested |
| PDU | Legacy connectable, scannable, undirected (`ADV_IND`), every 20-30 ms | Tested |

- **Name.** The TV classifies a remote by a substring match on its name; anything starting `LGE MR` gets the
  current Magic Remote handling (motion report `FD`, voice report `FE`) (Firmware). Both `LGE MR19BA` and
  `LGE MR23` registered and sent keys. Netflix and Prime Video keys only worked after registering as `LGE MR23`
  (Tested), which is why that is the default.
- **`SCD <year>.<version>`.** The TV parses this field to pick the motion calibration (SCD, see
  [F9 17](#f9-17-motion-calibration-scd)). It has built-in presets for `16.1`, `17.1` and `18.1`. For any other
  value it looks for a cached calibration under that version and, if there is none, asks the remote for one with
  `F9 17` (Firmware). With `21.2` the C9 asked once, accepted the reply, and stopped asking on later connections
  (Tested).
- **`BA 35`.** Meaning not established. It matches the low-battery threshold (35) that the TV reads from the
  version reply (Inferred).
- A real MR19 advertises the same structure with flags `0x05`, `SCD 18.1,BA 35,webOS` and the name `LGE MR19`
  in the scan response (Firmware, from the remote's own firmware).

## Registration (pairing a new remote)

The TV does not pair with arbitrary Bluetooth devices from its menu. A Magic Remote is registered like this
(Tested):

1. The remote advertises as above.
2. The TV receives the LG IR code **`0x20DFC13E`** (NEC address `0x04`, command `0x83`), once. A real remote sends
   it when you press the wheel to pair; this project sends it from an IR LED, or you can send it with any IR
   blaster.
3. The TV scans, connects to the advertising remote, and pairs.
4. The TV reads the HID report map and the report references, subscribes to the input reports, and starts sending
   `F9` commands.

Registering a new remote replaces the previous one. Do not sweep other IR codes looking for this one: LG's IR table
also contains factory, hotel-mode and reset codes. `0x20DF7B84` only shows a "press OK on the wheel" notice.

## Pairing and bonding

| Item | Value | Status |
|---|---|---|
| Method | LE Secure Connections, Just Works (no MITM, IO capability NoInputNoOutput) | Tested |
| Bonding | Yes; encryption and identity keys distributed both ways | Tested |
| ATT MTU | The TV does not start an MTU exchange. The remote must (this project asks for 247), or long `F9` replies are truncated | Tested |

### Security Request timing

- **New registration:** the TV waits for the remote's Security Request after connecting and gives up after about
  3 s. Send it as soon as the link is established (Tested).
- **Reconnect with a bond:** the TV encrypts the link itself within milliseconds. If the remote sends a Security
  Request as well, that request never completes and the link drops after about 30 s. This project waits 0.5 s
  and sends a Security Request only if the link is still unencrypted (Tested).
- Do not send notifications before the link is encrypted. A notification that raced the TV's encryption start
  ended the link with error `0x3D` (MIC failure) (Tested).

### Reconnecting

- After a TV power cycle or a remote restart, the TV reconnects by itself within seconds and encrypts with the
  saved bond. The first attempt after the TV boots sometimes drops with `0x3D`; the next succeeds (Tested).
- In standby the TV disconnects the remote (Firmware), so there is no link to send keys over. See
  [wake.md](wake.md).
- After a disconnect this project sends a 1.28 s burst of high-duty directed advertising to the bonded TV before
  returning to undirected advertising. After one failed reconnect the TV only connected to directed advertising
  (Tested). A real MR19 also reconnects with directed advertising (Firmware).

### Pairing rate limit (SMP `0x09`) and recovery

After repeated failed pairings the TV answers with SMP error `0x09` ("Repeated Attempts") and then ignores the IR
pairing code until it is power-cycled (Tested). A typical trigger: the remote still holds an old bond, reconnects,
and fails encryption.

Recovery (Tested):

1. Make the remote forget the TV (this project's `forget` action, below).
2. Switch the TV off and on, and wait about 20 s.
3. Pair again (advertise and send the IR pairing code).

To avoid it:

- When encryption fails because the TV no longer has the key, delete the remote's bond so the next registration
  starts clean. This project does that.
- To unpair, send `F9 40` first (see [unplug](#remote-to-tv-commands)) and delete the remote's own bond **before**
  the TV drops the link. With the bond still present, the disconnect starts directed advertising, the TV connects,
  the encryption fails, and the rate limit trips (Tested).

## GATT layout

The TV reads the HID service and uses three input reports and two writable reports. The layout below comes from a
real MR25GB remote, as documented by other open projects, and works on the C9 (Copied, Tested).

| Service | Characteristic | Notes |
|---|---|---|
| HID `0x1812` | HID Information `0x2A4A` | `00 00 00 01` |
| | HID Control Point `0x2A4C` | Write without response |
| | Protocol Mode `0x2A4E` | Read, write without response; report mode |
| | Report Map `0x2A4B` | Declares reports `FD`, `F9` (input and feature) and `FE` |
| | Report `0x2A4D`, reference `F9 01` | Input: replies to the TV, by notification |
| | Report `0x2A4D`, reference `F9 03` | Feature: the TV writes commands here |
| | Report `0x2A4D`, reference `FD 01` | Input: keys, wheel and motion |
| | Report `0x2A4D`, reference `FE 01` | Input: voice audio |
| | Report `0x2A4D`, reference `F9 02` | Output: the TV writes commands here too |
| Battery `0x180F` | Battery Level `0x2A19` | |
| Device Information `0x180A` | PnP ID and strings | PnP ID `000f:3412` (Broadcom), as the MR20 family reports it; an MR19's own firmware reports Realtek `005d:0000`. The TV doesn't check it |
| Scan Parameters `0x1813` | | |
| LG vendor service (128-bit UUID) | Two characteristics | Never used by the C9 (Tested) |

The C9 sends `F9` commands both as feature-report writes and as output-report writes (Tested); handle both.

## Reports

| Report ID | Direction | Value length | Use |
|---|---|---|---|
| `FD` | Remote to TV | 19 bytes | Key code, wheel, motion sensors |
| `FE` | Remote to TV | 123 bytes | Voice audio (see [voice.md](voice.md)) |
| `F9` | Both | Varies | Remote-management commands and replies |

The TV also has parsers for older report formats (`FA`, `F7`, `FC`) used by older remotes (Firmware); this
project does not use them.

### `FD` report layout (19 bytes)

| Offset | Size | Content | Status |
|---:|---:|---|---|
| 0 | 1 | Header, `0xC4`. A header copied from a different remote model made the TV ignore every report, keys included | Tested |
| 1 | 1 | Sequence number, 8-bit, wraps. Increment by one per motion report | Firmware, Tested |
| 2 | 1 | Bits 0-1: mode. `2` = navigation, `1` = pointing. Bits 2-7: battery (this project sends 39) | Firmware, Tested |
| 3 | 1 | Metadata, meaning not established; this project sends `0xFF` | Copied |
| 4-9 | 6 | Gyroscope axes 0, 1, 2: signed 16-bit, big-endian | Firmware |
| 10-15 | 6 | Accelerometer axes 0, 1, 2: signed 16-bit, big-endian | Firmware |
| 16-17 | 2 | Key code, unsigned 16-bit, big-endian; `0x0000` = no key | Firmware, Tested |
| 18 | 1 | Wheel, signed: notches in this report. The TV accepts -9 to +9, but an app gets one wheel event (`deltaY` ±53) per report whatever the count, so `scroll` sends one notch per report | Firmware, Tested |

- The TV accepts only modes 1 and 2 (Firmware).
- **Mode 2 (navigation)** is for keys outside a pointer session: arrows move the focus and OK selects. With
  mode 1 outside a motion stream, OK clicked at the hidden pointer and arrows did nothing (Tested).
- **Mode 1 (pointing)** reports carry gyro and accelerometer values at 100 Hz during a pointer session; see
  [pointer.md](pointer.md). A key pressed during a session is sent inside those reports.
- **Wheel:** the TV accepts -9 to +9 per report (Firmware). This project sends one notch per report; positive
  scrolls up. A notch also shows the pointer at the screen centre (Tested).
- With no motion, this project sends gyro `0 0 0` and accelerometer `0 0 -4087` (gravity along -z, about
  4096 counts per g).

### Special key codes

| Code | Meaning | Status |
|---|---|---|
| `0x803E` | "Active" / motion start. Sent when the TV subscribes to `FD`, and at the start of a pointer session | Firmware, Tested |
| `0x803F` | Idle / motion stop. Sent at the end of a pointer session and in reply to `F9 02` | Firmware, Tested |
| `0x800A` | Voice start acknowledgement (reply to `F9 03`) | Firmware, Tested |
| `0x800D` | Voice stop acknowledgement (reply to `F9 04`) | Firmware, Tested |

The TV excludes these four from ordinary key matching, so they do not release a held key (Firmware).

## Key codes

### The rule

**Bluetooth key code = `0x8000` | the NEC command byte of the key's LG IR code.**

LG IR codes are usually written as 32-bit numbers, most significant bit first, for example OK = `0x20DF22DD`.
The NEC command byte is the third byte of that number with its bit order reversed: `0x22` = `00100010` reversed
is `01000100` = `0x44`, so OK is `0x8044` over Bluetooth. Every code in the table below follows this rule
(Firmware, Tested).

Exceptions:

- **Power is `0x0008`**, without the `0x8000` bit. It turns the TV off. `0x8008`, `0x80C5` and a held `0x8008`
  do nothing (Tested).
- **HDMI 1-3 have no Bluetooth codes.** The TV's key table has none (Firmware); tried codes did nothing (Tested).
  Use IR, or Input (`0x800B`) and the input picker.
- Voice is `0x808B`; see [voice.md](voice.md).

### Keys verified on the C9

All of these were sent over Bluetooth and checked on 2026-11-20 by the key code a test app received, by the
foreground app changing, or by a system panel taking over the arrow keys (Tested).

| Key | Code | Key | Code | Key | Code |
|---|---|---|---|---|---|
| OK | `0x8044` | Up | `0x8040` | Down | `0x8041` |
| Left | `0x8007` | Right | `0x8006` | Back | `0x8028` |
| Home | `0x807C` | Settings | `0x8043` | More (`…`) | `0x8045` |
| Volume up | `0x8002` | Volume down | `0x8003` | Mute | `0x8009` |
| Channel up | `0x8000` | Channel down | `0x8001` | Input | `0x800B` |
| Red | `0x8072` | Green | `0x8071` | Yellow | `0x8063` |
| Blue | `0x8061` | Guide | `0x80AB` | Info | `0x80AA` |
| Play | `0x80B0` | Pause | `0x80BA` | Stop | `0x80B1` |
| Rewind | `0x808F` | Fast forward | `0x808E` | Exit | `0x805B` |
| Picture mode | `0x804D` | Sound mode | `0x8052` | Subtitles | `0x8039` |
| List | `0x8053` | Netflix | `0x8056` | Prime Video | `0x805C` |
| Digits 0-9 | `0x8010`-`0x8019` | Power | `0x0008` | Launcher | `0x8042` |
| Search | `0x8078` | Recent | `0x80B5` | Live TV | `0x800F` |
| Record | `0x80BD` | User guide | `0x807A` | Screen remote | `0x80DE` |
| Advanced settings | `0x80B8` | Input hub | `0x80B9` | Aspect ratio | `0x8079` |
| Voice | `0x808B` | Live menu | `0x809E` | Dash | `0x804C` |
| Previous track | `0x80B2` | Next track | `0x80B3` | | |

Notes:

- The TV's own name for Exit (`0x805B`) is "Live TV" and for Back (`0x8028`) is "Previous"; the TV turns them
  into Exit and Back (Firmware, Tested).
- Settings, Guide, More, Picture mode and List open system panels that apps do not see. In the Picture mode panel,
  Right changes the picture mode at once (Tested).
- The TV's key table holds about 40 further codes. Many are regional (Japanese broadcast keys and the like) and
  only reach apps as key codes, or do nothing visible on a US C9 (Tested).

### Keys never to send

| Code | Key | What happens |
|---|---|---|
| `0x8099` | Set-top power | Opens "use the Magic Remote to control your set-top box?" |
| `0x8098` | Set-top menu | Same dialog |

The dialog takes every key: apps see nothing, and Back, Exit and Home do not close it. Only choosing **No** closes
it; No is highlighted, so OK does it (Tested). This project leaves both codes out.

## Holding keys and long presses

The key field is a single code, not a bitmask: one key at a time (Firmware).

| Event | Behaviour | Status |
|---|---|---|
| Press | A report with the key's code starts a held key | Firmware |
| Hold | Re-send the same code. A real remote repeats it at about 100 Hz; this project every 15 ms | Copied, Tested |
| Release | A report with a different code (normally `0x0000`) releases the held key | Firmware, Tested |
| Stale release | The TV releases a key that has not been re-reported for more than 400 ms | Firmware |
| Auto-repeat | Starts after about 449 ms; repeats no faster than every ~44 ms | Firmware |
| Long press | Held for more than 1000 ms. Keys with a long-press action run their short action on release instead | Firmware |

Long-press actions on the C9 (keymap for `LGE MR…` remotes), checked by holding for 1.5 s (Tested):

| Key held | Result |
|---|---|
| Back | Live TV |
| Input | Input hub |
| Red | Record |
| Settings | A system panel (the keymap says Advanced settings) |
| Screen remote | A system panel (the keymap says Input) |

The keymap also lists long presses that weren't tried: Dash (BML), Live zoom (magnifier), Up/Down (screen
down) and `…` (audio description; holding `…` for 1.5 s did nothing) (Firmware).

Each target also has its own code (table above), so a long press is never needed to reach it.

## F9 commands

`F9` carries remote-management commands. The first value byte is the opcode. The TV writes commands to the
feature or output report; the remote replies with notifications on the `F9` input report.

### TV to remote commands

| Opcode | Purpose | Reply from this project | Status |
|---|---|---|---|
| `17` | Request the motion calibration (SCD) | Two fragments, see below | Firmware, Tested |
| `11` | Firmware version query | `12 …`, see below | Firmware, Tested |
| `13` | Unknown | `14 18 52 05 38 0C 00 01 08 03 E0 01` | Copied |
| `19` | Unknown | `19 34 23` followed by nine `FF` | Copied |
| `90` | Unknown | `90 00 00` | Copied |
| `01` | Pointer active ("active motion") | None | Firmware |
| `02` | Idle: the TV has hidden the pointer or wants the remote to sleep | FD `0x803F`, then `0x0000` | Tested |
| `03` | Voice: open the microphone | FD `0x800A` | Tested |
| `04` | Voice: close the microphone | FD `0x800D` | Tested |
| `05` | Voice keep-alive, about every 3 s | None | Firmware, Tested |
| `15` | Start a remote firmware update | None; ignore it | Firmware |
| `61`-`68` | Universal control (IR code sets) | See [universal-control.md](universal-control.md) | Firmware |

On the C9, `17`, `11` and `02` were seen (Tested), and `11` arrives again at the start of pointer sessions. The
replies to `13`, `19` and `90` come from captures of real remotes talking to other TVs; they are answered in case
a TV asks, and what they mean is unknown. This project
also answers `6C` and `6D` with `6C 00` and `6D 00`, as a real remote did for a newer TV (Copied). Unknown opcodes
are logged and not answered.

A real remote answers `F9 02` with `0x803F` and drops the link about 15 s later (Copied). This project answers
the same way but stays connected.

For long commands the TV splits its writes into chunks of about 90 bytes (Firmware). Only the universal-control
commands are long enough to need this.

### Remote to TV commands

| Opcode | Purpose | Status |
|---|---|---|
| `40` | Unplug: the TV drops the link at once, deletes the pairing and shows a notice | Firmware, Tested |
| `41` | Unplug for a different remote type | Firmware |

### F9 17: motion calibration (SCD)

The TV only creates its pointer device after it has a valid calibration block. Without one, keys work and the
pointer appears but never moves (Tested; this was the cause of a long failure).

**Reply framing.** Each fragment is a notification on the `F9` input report:

| Offset | Content |
|---:|---|
| 0 | `17` |
| 1 | `01` if another fragment follows, `00` for the last one |
| 2 | Number of calibration bytes in this fragment; `0` makes the TV ignore the fragment |
| 3 | `00` (skipped by the TV) |
| 4… | Calibration bytes, appended to what the TV has collected so far |

The TV collects up to 272 bytes and creates the pointer device after a non-empty last fragment (Firmware). This
project sends 260 bytes as 150 + 110, each in a 154-byte value (Tested).

**Validity rule.** The TV rejects the block unless all of these hold (Firmware; a block that failed them was
rejected on the TV, Tested):

| Check | Requirement |
|---|---|
| Size | First little-endian 16-bit field = `260` |
| Version | Second little-endian 16-bit field = `5` |
| Checksum | The sum of the first 65 little-endian 32-bit words (all 260 bytes) is zero, modulo 2^32 |

The block also carries the sensor scales and the nominal report period (a 32-bit float of `0.01` s at offset 8,
after the marker bytes `FB FA E1 E7` at offset 4) (Firmware). This project's block comes from a public dump of
an MR20GA remote's calibration. That dump started one byte early and lacked the last byte; dropping the leading
byte and appending `0xAB` (the only last byte that makes the checksum zero) gives a block the C9 accepts. The C9
then cached it and stopped asking on reconnects (Tested).

### F9 11: version reply

The reply has opcode `12`:

| Offset | Content | This project |
|---:|---|---|
| 0 | `12` | |
| 1 | Major version. The TV reads the fields below only when it is above `0x11` (Firmware) | `0x15` |
| 2 | Minor version: the TV logs bytes 1-3 as `major.minor.patch` (Firmware) | `0x61` |
| 3-7 | Firmware version bytes. The TV reads offsets 6 and 7 as the battery thresholds "mid" and "low" (Firmware) | `02 02 28 34 23` (mid 52, low 35) |
| 8-13 | Two stored universal-control code-set IDs, 3 bytes each, `FF FF FF` = none | |
| 14-16 | The primary (set-top power) code-set ID | |

Offsets 0-7 are a real MR20GA's reply (Copied). Offsets 8-16 are described in
[universal-control.md](universal-control.md).
