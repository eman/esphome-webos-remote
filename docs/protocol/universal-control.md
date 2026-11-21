# Universal control (UEI ZipIR)

LG TVs can use the Magic Remote as an IR blaster for a set-top box or another device ("Universal Control", set up
in the TV's Device Connector). The TV never sends raw timings. It takes code sets from its built-in UEI QuickSet
database and sends them to the remote in UEI's compressed **ZipIR** format over the remote's Bluetooth `F9` report;
the remote decodes them and transmits the IR itself.

In this project, `lg_magic_remote` handles the commands (`uei_command_` in `lg_magic_remote.cpp`) and
`lg_ir_remote` decodes and transmits (`lgir_zipir.c`, `transmit_zipir`).

> **Status.** Worked out by static analysis of an LG OLED C9 (2019, webOS 4.5) firmware and by running the TV's
> QuickSet library in an emulator. This project's firmware was tested only by **injecting** the TV's `F9` writes
> into the ESP32, never driven by the TV itself: the TV sends these commands only once a Universal Control device
> is set up, and that was not done. See [Tested and untested](#tested-and-untested).

## Commands

Bytes as the remote sees them in a write to its `F9` report (without the report ID).

| Op | Name | TV to remote | Remote reply |
|---|---|---|---|
| `61` | transferZipIr | `61 more` + `repeat` (1) + one-key ZipIR file | `61 st` |
| `62` | saveCodeSet | `62 more` + code set id (3) + ZipIR file | `62 st` |
| `64` | eraseCodeSet | `64` + id (3), or `ff ff ff` = all | `64 st` |
| `65` | sendIrCode | `65` + id (3) + UEI key index (1) + `repeat` (1) | `65 st id(3) key(1)` |
| `66` | getCodeSet | `66` | `66 n id1(3) id2(3)` |
| `67` | setStbPwrIr | `67` + id (3) | `67 st` |
| `68` | getStbPwrIr | `68` | `68 id(3)` |

- **Code set id**: 3 bytes, the device-type letter (ASCII) and the code set number as a big-endian u16. For
  example `53 0f 01` is `S3841` (a made-up id). `ff ff ff` = none.
- **Status** `st`: `00` = success, `01` = failure. Other values were not seen.

## Transport

### Fragments

The TV writes at most 90 bytes per fragment, counting the report ID.

| Ops | Fragment | Data per fragment |
|---|---|---|
| `61`, `62` | `op more data...`; `more` = 1 while another fragment follows, 0 on the last | up to 87 bytes |
| `64`-`68` | `op data...` | always one fragment |

The TV may write to the output report or to the feature report, depending on the remote's firmware version;
accept both. A `62` payload (id + ZipIR) is at most 1600 bytes, about 19 fragments.

### Timing

- Before **each** fragment the TV waits up to 150 ms for a reply to the previous one, then sends anyway.
- A new command waits up to 1 s for the previous command's reply.

So the remote should answer every fragment. This project replies `op 00` to each fragment with `more` = 1 and the
real status after the last one.

### How the TV reacts to replies

| Reply | TV |
|---|---|
| `62 01` | asks `66` for the list of stored code sets (the remote may be full or hold another set) |
| `65 01 ...` | waits 250 ms before the next command |
| `66 n ...` | reads at most two ids and counts the non-`ff ff ff` ones itself |

## Firmware-version gate

The TV asks for the remote's firmware version with `F9 11`; the remote answers `12 ...`. The TV refuses all
universal-control commands when the major version (byte 1) is below `0x12`. The same reply tells the TV which code
sets the remote holds:

| Byte | Content |
|---|---|
| 0 | `12` (reply op) |
| 1 | firmware major (must be at least `0x12`) |
| 2 | zone |
| 3-5 | not used for universal control |
| 6, 7 | battery thresholds (mid, low) |
| 8-10 | 1st stored code set id |
| 11-13 | 2nd stored code set id |
| 14-16 | primary (set-top power) code set id |

This project answers `12 15 61 02 02 28 34 23` (major 21) followed by the stored ids, `ff ff ff` for empty slots,
held sets first.

## Payloads

### 0x61: transmit one key now

`repeat` + a ZipIR file with one code set holding only this key, as **function 0** whatever its real index. The TV
re-encodes the protocol part to just what that key needs.

`repeat` is 1 for OK, Back, Page up/down, the four arrows and Channel up/down, and 0 for other keys (also 0 when
the TV types a channel number as digits).

### 0x62: store a code set

Code set id + a ZipIR file with that code set and the keys of the cacheable list (below) it has, at their **real**
UEI key indices. If the device is the primary one, `67` follows.

### 0x65: transmit a stored key

Code set id + UEI key index + `repeat`. The TV uses it only for the cacheable keys and only when it believes the
remote holds the code set; everything else goes as `61`.

### 0x64, 0x66, 0x67, 0x68

`64` erases one stored set, or all with `ff ff ff`. `66` asks which sets are stored. `67` names the primary code
set, the one that supplies the set-top power key; what a real remote does with it on its own is not known. `68`
asks for it.

## When the TV sends them

Nothing is sent unless a Universal Control device is configured in Device Connector (device type, brand, HDMI
input).

- **On connect**: when the TV learns which sets the remote holds (from the `12` reply or a `66` reply), it erases
  sets it no longer uses (`64`), sets the primary (`67`) and stores the device's set (`62`, then `67` if primary).
- **On a key press**: the remote sends the key over Bluetooth as usual. If the current input is assigned to a
  Universal Control device, the TV answers with `65` (cacheable key, set stored) or `61`. One command per press;
  the TV sends nothing on release.
- **During setup** (inferred): the key test sends a test key as `61` and asks the user whether the device reacted.

To try it without a set-top box, assign the device to an **unused** HDMI input. Assigning Universal Control to an
input in use changes how remote keys are handled on that input until the device is removed.

## UEI key indices

The cacheable keys, the ones `62` stores and `65` sends (29):

| Index | Key | Index | Key |
|---|---|---|---|
| 1 | Power | 36 | Exit |
| 4, 5 | Channel up, down | 37 | Back |
| 9-17 | digits 1-9 | 38-41 | Up, Down, Left, Right |
| 18 | digit 0 | 42 | OK |
| 19 | Channel enter | 77-80 | Red, Green, Yellow, Blue |
| 24 | Play | 121 | Home |
| 26 | Pause | | |
| 33 | Menu | | |

Other common indices (sent as `61`): 6, 7, 8 volume up, down, mute; 21 last channel; 22 input; 25 stop; 27 rewind;
28 fast forward; 29 record; 35 guide; 45 favourite; 46 info; 105 DVR. For a set-top box the TV maps Page up/down to
Channel up/down first.

## ZipIR format

### Container

Big-endian.

| Field | Size | Content |
|---|---|---|
| VER | 1 | 3 |
| FSZ | 4 | file size |
| FCD | 4 | database tag, copied into extracts |
| TC | 1 | number of type entries, including a terminator |
| type entry | 5 x TC | type letter (1, `00` for the terminator), code set count (2), index of its first code set (2) |
| CC | 2 | number of code set records, including a terminator |
| record | 8 x CC | code set number (2), data offset (4), protocol part length (2) |
| data | | from byte `12 + 5 x TC + 8 x CC`: per code set, its protocol part, then its key part |

Data offsets count from the start of the data. A code set's key part runs from the end of its protocol part to the
next record's offset; the terminator record (number `ffff` or 0) carries the end offset.

### Protocol part

A bit stream, read most significant bit first from the first byte.

| Field | Bits | Content |
|---|---|---|
| VER | 4 | 0 |
| EM | 4 | encoding mode; only 0 is known and supported |
| CP | 12 | carrier period in 10 ns units: carrier = 100,000,000 / CP Hz; 0 = no carrier |
| DC | 4 | duty cycle in tens of percent (10 with CP 0: baseband, plain on/off) |
| NCT | 6 | number of characteristic times (CT) - 1 |
| NLT | 4 | how many of the CTs are "long" |
| CT[i] | 16 or 12 | the first NLT: 16 bits x 8 us; the rest: 12 bits x 4 us |
| NRS | 6 | number of repetition sequences (RS) - 1 |
| LRS | 2 | width of the RS lengths: LRS + 3 bits |
| SRS[i] | LRS + 3 | length of RS i, - 1; all lengths first |
| RS[i][j] | bitlen(NCT - 1) | CT index; all RS in order |
| NWS | 10 | number of waveform substitutions (WS) - 1 |
| LWS | 3 | width of the WS lengths: LWS + 3 bits |
| SWS[i] | LWS + 3 | length of WS i, - 1 |
| WS[i][j] | bitlen(NRS - 1) | RS index |

`bitlen(n)` is the number of bits of `n`, at least 1 (`bitlen(7)` = 3, `bitlen(4)` = 3, `bitlen(0)` = 1). The rest
of the last byte is padding.

### Key part

Starts on a byte boundary, right after the protocol part.

| Field | Bits | Content |
|---|---|---|
| BWS | 4 | width of the WS indices below |
| SQC | 4 | functions per key - 1 (toggle states: 2 for RC5/RC6-style protocols) |
| functions | | until fewer than 3 bits are left, or a field runs out or is out of range |

Each function:

| Field | Bits | Content |
|---|---|---|
| WSP | 3 | bit 2 = M (main frame), bit 1 = R (repeat frame), bit 0 = B (third frame); 0 = no function |
| WS index | BWS each | one for each of M, R, B that is set, in that order; must be below NWS |

Function `n` is key `n / SQC`, toggle state `n % SQC`. WSP values seen in practice: 0, 2 (R), 3 (R B), 4 (M),
6 (M R), 7 (M R B).

### From ZipIR to a waveform

1. Pick function `key x SQC + toggle % SQC`. If that toggle state is empty, use toggle 0; if that is empty too, the
   key is missing.
2. Each frame (M, R, B) is a WS: expand it to its RS, and each RS to its CT durations.
3. The durations alternate **mark** (carrier on) and **space**, starting with a mark, across the whole frame.
4. The last space of a frame is the gap before the next frame.
5. Carrier = 100,000,000 / CP Hz (0 = baseband), duty = DC x 10 %.

### Press order

What the TV's QuickSet library's own software blaster sends for one press:

| Frames present | Sent |
|---|---|
| M, R and B | M R B |
| M and R | M R R |
| one frame only | that frame three times |

This project follows the same rule for the remaining combinations: M and B sends M M M; R and B sends R R R (B is
not sent). Which frames the TV's library sends for those two combinations was not confirmed.

### Toggle and held keys (inferred)

- For SQC 2 code sets the toggle state should flip on every new press; the library takes the toggle as an argument
  and doesn't keep it. This project flips it after each transmission of such a code set.
- What a real Magic Remote sends while a key is held, and exactly what the `repeat` byte changes, is not known. This
  project: when `repeat` is 1 and the command arrives while one of its own Bluetooth keys is held, the press is sent
  as above and then the R frame (or the last frame sent, if there is no R) repeats at its own length until the key
  is released.

## What this project implements

| Part | Implementation |
|---|---|
| Fragments | reassembled per op (`61`, `62`); each fragment with `more` = 1 answered `op 00` |
| `61` | decoded and transmitted at once; `61 00`, or `61 01` if it can't be decoded |
| `62` | container checked, stored in one of 2 slots in flash (1600 bytes each, kept across restarts); replaces a stored set with the same id; when both slots are full, replaces the one that isn't the primary (the TV's expectation here is not known) |
| `64` | erases one set or all |
| `65` | transmits the key from the stored set; `65 01 ...` if the set or the key is missing |
| `66`, `12` | stored ids, held sets first |
| `67`, `68` | primary id stored and reported; not used otherwise |
| Carrier, duty | as decoded; duties outside 10-90 % fall back to 50 % |
| EM other than 0 | rejected (`01`) |
| No IR transmitter linked | `61`, `62` and `65` refused with `01` |

## Tested and untested

| What | How | Result |
|---|---|---|
| Format and decoder | a reference decoder compared with the TV's own QuickSet decoder (emulated) on every code set in the C9's local database, all keys, toggles 0-2 | about a million waveforms, no differences |
| `lgir_zipir.c` | compared with that reference decoder on a sample of the database | 13,184 waveforms identical |
| `lgir_zipir.c` | [`tests/zipir/`](../../tests/zipir/): synthetic code sets only | passes |
| Command handling | the TV's `F9` writes injected into the ESP32 (2026-11-20) | fragments reassembled, replies as specified, 38 kHz frames handed to the transmitter |
| `repeat` | `65` with repeat 1 injected during a 1.5 s held Right | 9 repeat frames, all seen by an IR receiver |
| Transmitted waveform | one capture of an NEC-type key with an IR receiver | main frame identical bit for bit, followed by its two repeat frames |
| Driven by the TV | needs a Universal Control device set up | **not done** |
| Toggle codes, baseband code sets, actual duty | | **not tested** |

The code-set database and these comparisons are not part of this repository.

## Unknown

- The real Magic Remote's IR engine: its repeat timing, what it does with the primary code set by itself, and its
  error replies.
- Status values other than `00` and `01`.
- Encoding modes other than 0. None are in the C9's local database; code sets from LG's online service might use
  them.
