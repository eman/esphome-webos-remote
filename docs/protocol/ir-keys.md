# LG TV infrared keys

The IR codes `lg_ir_remote` sends, how they relate to the Bluetooth Magic Remote's key codes, which codes must
never be sent, and how held keys work.

Tested on one TV only: an LG OLED C9 (2019, webOS 4.5). LG has used the same `0x20DF....` codes across many
years of TVs, but other models were not tested.

## Frame format

| Item | Value |
|---|---|
| Protocol | NEC, 32 bits |
| Carrier | 38 kHz |
| Header | 8 ms mark, 4 ms space (ESPHome's `lg` protocol timings, which the TV decodes) |
| Bit | 0.6 ms mark, then 0.55 ms space for a 0 or 1.6 ms space for a 1 |
| End | 0.6 ms closing mark |
| Code | `0x20DF<cc><~cc>` |

Codes are written the way ESPHome's `lg` protocol and most LG code lists write them: the 32 bits in the order they
are sent, first bit in the top bit. NEC sends every byte least significant bit first, so:

- `20 DF` is the NEC address `0x04` and its complement `0xFB`, each bit-reversed.
- `cc` bit-reversed is the NEC command, and that command **is the TV's key index**: the TV looks it up in its
  internal key table (on the C9, 349 entries, indices `0x000`-`0x15C`).
- Indices above `0xFF` (Japanese broadcast digit keys on the C9) don't fit in an NEC command, so no IR code reaches
  them.

Example: OK is `0x20DF22DD`. `cc` = `0x22` = `0010 0010`; reversed, `0100 0100` = `0x44`, the TV's Enter key.

## Bluetooth key codes

The Bluetooth Magic Remote sends the same index as `0x8000 | index`: OK is `0x8044`. Exceptions:

- **Power** is `0x0008`, without the `0x8000` prefix. `0x8008` does nothing.
- **Screen remote** (see [below](#screen-remote-caveat)).
- Some indices only work over one of the two: see the table.

## Keys

Every key in `KEYS` in `components/lg_ir_remote/lg_ir_remote.cpp`. "Key" is the name `lg_ir_remote.press` and
`lg_magic_remote` take.

| Key | Button | IR code | Index | Bluetooth code |
|---|---|---|---|---|
| `power` | Power | `0x20DF10EF` | `0x08` | `0x0008` (no `0x8000` prefix) |
| `poweron` | Power on | `0x20DF23DC` | `0xC4` | none (see below) |
| `poweroff` | Power off | `0x20DFA35C` | `0xC5` | none (see below) |
| `up` | Up | `0x20DF02FD` | `0x40` | `0x8040` |
| `down` | Down | `0x20DF827D` | `0x41` | `0x8041` |
| `left` | Left | `0x20DFE01F` | `0x07` | `0x8007` |
| `right` | Right | `0x20DF609F` | `0x06` | `0x8006` |
| `ok` | OK | `0x20DF22DD` | `0x44` | `0x8044` |
| `back` | Back | `0x20DF14EB` | `0x28` | `0x8028` |
| `exit` | Exit | `0x20DFDA25` | `0x5B` | `0x805B` |
| `home` | Home | `0x20DF3EC1` | `0x7C` | `0x807C` |
| `settings` | Settings | `0x20DFC23D` | `0x43` | `0x8043` |
| `more` | Quick menu | `0x20DFA25D` | `0x45` | `0x8045` |
| `input` | Input | `0x20DFD02F` | `0x0B` | `0x800B` |
| `info` | Info | `0x20DF55AA` | `0xAA` | `0x80AA` |
| `guide` | Guide | `0x20DFD52A` | `0xAB` | `0x80AB` |
| `volup` | Volume up | `0x20DF40BF` | `0x02` | `0x8002` |
| `voldown` | Volume down | `0x20DFC03F` | `0x03` | `0x8003` |
| `mute` | Mute | `0x20DF906F` | `0x09` | `0x8009` |
| `chup` | Channel up | `0x20DF00FF` | `0x00` | `0x8000` |
| `chdown` | Channel down | `0x20DF807F` | `0x01` | `0x8001` |
| `netflix` | Netflix | `0x20DF6A95` | `0x56` | `0x8056` |
| `prime` | Prime Video | `0x20DF3AC5` | `0x5C` | `0x805C` |
| `list` | List | `0x20DFCA35` | `0x53` | `0x8053` |
| `play` | Play | `0x20DF0DF2` | `0xB0` | `0x80B0` |
| `pause` | Pause | `0x20DF5DA2` | `0xBA` | `0x80BA` |
| `stop` | Stop | `0x20DF8D72` | `0xB1` | `0x80B1` |
| `rewind` | Rewind | `0x20DFF10E` | `0x8F` | `0x808F` |
| `ff` | Fast forward | `0x20DF718E` | `0x8E` | `0x808E` |
| `red` | Red | `0x20DF4EB1` | `0x72` | `0x8072` |
| `green` | Green | `0x20DF8E71` | `0x71` | `0x8071` |
| `yellow` | Yellow | `0x20DFC639` | `0x63` | `0x8063` |
| `blue` | Blue | `0x20DF8679` | `0x61` | `0x8061` |
| `picture` | Picture mode | `0x20DFB24D` | `0x4D` | `0x804D` |
| `sound` | Sound mode | `0x20DF4AB5` | `0x52` | `0x8052` |
| `subtitles` | Subtitles | `0x20DF9C63` | `0x39` | `0x8039` |
| `launcher` | Launcher | `0x20DF42BD` | `0x42` | `0x8042` |
| `search` | Search | `0x20DF1EE1` | `0x78` | `0x8078` |
| `recent` | Recent | `0x20DFAD52` | `0xB5` | `0x80B5` |
| `livetv` | Live TV | `0x20DFF00F` | `0x0F` | `0x800F` |
| `livemenu` | Live menu | `0x20DF7986` | `0x9E` | `0x809E` |
| `record` | Record | `0x20DFBD42` | `0xBD` | `0x80BD` |
| `prevtrack` | Previous track | `0x20DF4DB2` | `0xB2` | `0x80B2` |
| `nexttrack` | Next track | `0x20DFCD32` | `0xB3` | `0x80B3` |
| `aspect` | Aspect ratio | `0x20DF9E61` | `0x79` | `0x8079` |
| `screenremote` | Screen remote | `0x20DF6996` | `0x96` | `0x80DE` (see below) |
| `userguide` | User guide | `0x20DF5EA1` | `0x7A` | `0x807A` |
| `dash` | Dash | `0x20DF32CD` | `0x4C` | `0x804C` |
| `hdmi1` | HDMI 1 | `0x20DF738C` | `0xCE` | none |
| `hdmi2` | HDMI 2 | `0x20DF33CC` | `0xCC` | none |
| `hdmi3` | HDMI 3 | `0x20DF9768` | `0xE9` | none |
| `0` | 0 | `0x20DF08F7` | `0x10` | `0x8010` |
| `1` | 1 | `0x20DF8877` | `0x11` | `0x8011` |
| `2` | 2 | `0x20DF48B7` | `0x12` | `0x8012` |
| `3` | 3 | `0x20DFC837` | `0x13` | `0x8013` |
| `4` | 4 | `0x20DF28D7` | `0x14` | `0x8014` |
| `5` | 5 | `0x20DFA857` | `0x15` | `0x8015` |
| `6` | 6 | `0x20DF6897` | `0x16` | `0x8016` |
| `7` | 7 | `0x20DFE817` | `0x17` | `0x8017` |
| `8` | 8 | `0x20DF18E7` | `0x18` | `0x8018` |
| `9` | 9 | `0x20DF9867` | `0x19` | `0x8019` |

Notes:

- **Power** toggles. **Power on** and **Power off** are the TV's discrete codes and do the same thing whatever
  state the TV is in; a discrete Power off sent to a TV in standby leaves it in standby (tested). There is no
  discrete Bluetooth code (`0x80C5` does nothing). The TV is only connected over Bluetooth while it is on, so
  `lg_magic_remote` sends Power `0x0008` for "power off" and wakes the TV with an advertisement for "power on"
  (see [wake](wake.md)).
- **HDMI 1-3** are discrete input codes. They have no Bluetooth code: `0x80CE`, `0x80CC` and `0x80E9` do nothing on
  the C9. Over Bluetooth, use Input (the input picker). HDMI 4 would be `0x20DF5BA4` (index `0xDA`); it is not in
  `KEYS` and was not tested.
- **Advanced settings** (`0x80B8`), **Input hub** (`0x80B9`) and **Voice** (`0x808B`) are Bluetooth only: the TV's
  key table has no IR entry for them.

## Screen remote caveat

Don't derive an IR code from a Bluetooth code (or the reverse) without checking the TV's table. Screen remote is
`0x80DE` over Bluetooth, but index `0xDE` in the IR table is something else: `0x20DF7B84`, the code that only shows
the "press the wheel to register the remote" notice. The IR Screen remote is index `0x96`, `0x20DF6996` (tested:
it opens the same panel as the Bluetooth key).

## Registration code

`0x20DFC13E` (index `0x83`) asks the TV to register a Bluetooth Magic Remote. Sent once while the remote
advertises, it makes the TV connect and pair (see [pairing](../pairing.md)). It is `LG_IR_PAIRING` /
`send_pairing()` in the component and is not in `KEYS`.

Public LG code lists also name `0x20DFAE51` (index `0x75`) as a registration code. This project doesn't use it and
it was not tested.

## Codes never to send

The TV's key table also holds factory, store and service functions. Some put the TV into a mode that is hard to
leave or that changes settings. They are not in `KEYS` on purpose, and the rule is simple: **never sweep IR codes**
to see what they do. Indices to avoid (the IR code is `0x20DF`, the index bit-reversed, then its complement):

| Index | What it is |
|---|---|
| `0x4E`, `0x4F` | hotel mode, hotel mode "ready" |
| `0x33` | store (demo) power mode |
| `0x68` | self-diagnosis |
| `0x69` | white-balance adjustment |
| `0x84`-`0x87` | factory volume presets |
| `0x88` | another discrete factory function, next to the volume presets |
| `0x99` | auto-configure |
| `0xA6`, `0xEE`, `0xA8` | HDMI, USB and second-USB checks |
| `0xD1`, `0xD2` | QR and test-pattern checks |
| `0xF9` | tilt check |
| `0xFA`, `0xFB` | factory ("in-start") menu stop and start |
| `0xFC`, `0xFD` | service checks |
| `0xFE` | power-only (factory) mode |
| `0xFF` | service adjustment menu |
| `0xDE` | `0x20DF7B84`: harmless, but it only shows the "press the wheel" notice; the old Magic Remote sends it for keys it can't send over IR |

Many other entries in the table have no label in the firmware; an unlabelled index is no proof that it is safe.

Two Bluetooth codes to avoid as well: `0x8099` (set-top power) and `0x8098` (set-top menu). On a TV with no
universal-control device set up they open a "use the Magic Remote to control your set-top box?" dialog that takes
every key, including Back, Exit and Home, until No is chosen (No is highlighted, so OK closes it).

## Held keys

An LG IR remote sends a held key as one full frame followed by NEC **repeat codes** (9 ms mark, 2.25 ms space,
0.56 ms mark), one every ~108 ms. The TV treats that as one held key and auto-repeats it only where that makes sense
(arrows, volume). Repeating the **whole frame** instead looks like separate presses.

Measured on the C9 (2026-11-20), each key held 1.5 s over IR, keys counted in a test app:

| Hold style | Right | Back |
|---|---|---|
| whole frame every ~108 ms | 11 key presses | 11 Backs (stepped back 11 times) |
| first frame, then repeat codes | 23 key presses (the TV's own auto-repeat) | 1 Back |

`lg_ir_remote` sends repeat codes by default (`hold_style: repeat_codes`); `hold_style: full_frames` keeps the
other behaviour for testing.

## How the codes were checked

- Every code in `KEYS` has address `0x20DF` and a correct command complement, and its index lands on the TV key
  table entry the key is named after (from the C9 firmware). Apart from Power and Screen remote, every key that has
  both an IR and a Bluetooth code uses `0x8000 | index` for Bluetooth.
- The Bluetooth codes were each tested on the C9 (2026-11-20): in a test app that logs key events, by the
  foreground app they opened, or by the system panel they opened.
- The IR codes for Launcher, Search, Recent, Live TV, Live menu, Record, Previous/Next track, Aspect ratio, User
  guide, Dash and Screen remote were each sent to the C9 and did what their Bluetooth twin does. The discrete power
  codes and the hold styles were tested as described above. The other IR codes are the long-standing LG codes from
  public lists (see the README's credits).
