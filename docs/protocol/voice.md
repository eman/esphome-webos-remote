# Voice

A Magic Remote streams its microphone to the TV as mSBC audio in `FE` reports, while the voice key is held. The
ESP32 has no microphone, so this project streams synthesised audio instead, for example Home Assistant text to
speech.

Everything here was worked out on **one TV: an LG OLED C9 (2019, webOS 4.5, firmware 05.50.00)**, and from an
MR19 remote's firmware. Labels as in [bluetooth.md](bluetooth.md#scope-and-evidence): **Tested** (on the C9,
2026-11-20), **Firmware** (read in the TV's or the MR19's firmware), **Inferred**.

## Who does what

- The remote decides nothing by itself. The TV opens and closes the microphone with `F9` commands (Firmware).
- On the TV, the voice key goes through the input service `lginput2` to the system UI, which asks the voice
  framework (`voiceconductor`) to start. That starts the audio reader (`voiceinput`), which sends `F9 03` to the
  remote and decodes the `FE` reports (Firmware; the middle steps are Inferred).
- The session lasts while the voice key is held (hold to talk). Releasing it ends the session (Firmware, Tested).

## Session sequence

| Step | Direction | Message | Timing |
|---|---|---|---|
| 1 | Remote → TV | `FD` key `0x808B` (Voice), mode 2. Re-send it at least every 400 ms (this project: every 50 ms) | |
| 2 | TV → remote | `F9 03`: open the microphone | 835 ms after step 1 in the test |
| 3 | Remote → TV | `FD` key `0x800A`: microphone open | Within 500 ms of `F9 03` |
| 4 | Remote → TV | `FE` reports with audio, key field `80 8B` | One every 15 ms; never a gap of 1 s or more |
| 5 | TV → remote | `F9 05`: keep-alive. No reply | About every 3 s |
| 6 | Remote → TV | Release the key: `FD` key `0x0000` (and/or `FE` reports with key `00 00`) | After the speech and some trailing silence |
| 7 | TV → remote | `F9 04`: close the microphone | |
| 8 | Remote → TV | Stop `FE` at once; `FD` key `0x800D`: microphone closed | Within 500 ms of `F9 04` |

Nothing sent before `F9 03` is read: the TV opens its audio reader only then (Firmware). `FE` reports on their
own never start a session (Firmware).

### TV timers

| Timer | Length | Started / cleared | On expiry |
|---|---|---|---|
| Start | 500 ms | Started at `F9 03`; cleared by `0x800A` | `F9 04`, error "start message missing" |
| Stop | 500 ms | Started at `F9 04`; cleared by `0x800D` | `F9 04` again |
| Stream gap | 1 s | Restarted by every `FE` report; cleared by `0x800D` | `F9 04`, error "streaming stop" |
| Capture length | 10 s | Started at `0x800A`, restarted by each keep-alive | `F9 04` |

All Firmware. The key itself is subject to the usual rule: the TV releases a key not refreshed for 400 ms
(Firmware). `FE` reports with `80 8B` and valid audio also refresh it (Firmware). `0x800A` and `0x800D` do not
release the voice key (Firmware).

If the remote has to end a session itself, it sends `FD 0x800D` and then releases the key. Stopping `FE` without
`0x800D` makes the TV stop after 1 s with an error (Firmware).

## `FE` report layout (123 bytes)

The report map declares 124 bytes for `FE`, but real remotes send 123-byte values. The TV requires exactly 124
bytes including the report ID (that is, 123 in the GATT value) and rejects anything else for voice (Firmware).
Keep the descriptor as it is and send 123 bytes.

| Offset | Size | Content |
|---:|---:|---|
| 0 | 1 | Sequence number: +1 per report, wraps at `0xFF`. Only used for the TV's packet statistics |
| 1-2 | 2 | Key held, big-endian: `80 8B` while the voice key is held, `00 00` after release |
| 3-62 | 60 | Audio unit 1 |
| 63-122 | 60 | Audio unit 2 |

Each audio unit (Firmware):

| Offset in unit | Size | Content |
|---:|---:|---|
| 0 | 1 | `01` (checked by the TV) |
| 1 | 1 | Sequence byte, not checked. This project cycles the HFP mSBC values `08 38 C8 F8` |
| 2-58 | 57 | One mSBC frame, starting with the sync byte `AD` (checked by the TV) |
| 59 | 1 | Pad byte, value ignored (this project sends `00`) |

## Audio format

| Item | Value |
|---|---|
| Codec | mSBC: SBC with 16 kHz sample rate, mono, 8 subbands, 15 blocks, loudness allocation, bitpool 26 |
| Frame | 57 bytes, header `AD 00 00 <CRC-8>`; the TV's decoder rejects frames with a bad CRC |
| Per frame | 120 samples = 7.5 ms |
| Per `FE` report | 2 frames = 240 samples = **15 ms** |
| Rate | 66.7 reports per second, about 8.2 kB/s of payload |
| Input PCM | Ordinary signed 16-bit samples; the TV decodes to 16 kHz mono 16-bit |

All Firmware. A real remote drops its first four buffers (about 60 ms) after `F9 03`, and does not resend a
dropped buffer (Firmware). The TV has no hard timing requirement beyond the 1 s gap; real-time pacing with small
bursts matches the real remote (Inferred).

### Link requirements

- The TV must be subscribed to the `FE` input report.
- ATT MTU of at least 126 (this project negotiates 247).
- The connection must carry one 123-byte notification every 15 ms.

## How this project feeds it from Home Assistant

The component provides a `speaker` platform, `lg_magic_remote`. Anything played on it is encoded as mSBC and
streamed to the TV as if spoken into the remote. A `speaker` media player in front of it lets Home Assistant play
TTS or sound files to it:

```yaml
speaker:
  - platform: lg_magic_remote
    id: lg_voice_speaker
    lg_magic_remote_id: lg_bt
    buffer_duration: 250ms

media_player:
  - platform: speaker
    id: lg_voice_player
    name: TV voice
    buffer_size: 16000
    announcement_pipeline:
      speaker: lg_voice_speaker
      format: WAV
      sample_rate: 16000
      num_channels: 1
```

The media pipeline must deliver **16 kHz, mono, 16-bit WAV**; the speaker logs an error otherwise. Home Assistant
transcodes to that format. Then, for example:

```yaml
action: tts.speak
target:
  entity_id: tts.your_tts_engine
data:
  media_player_entity_id: media_player.your_tv_voice_player
  message: Netflix
```

What the speaker does:

1. Buffers some audio first, then holds the voice key (`0x808B`, refreshed every 50 ms).
2. Gives up if the TV does not send `F9 03` within 4 s.
3. On `F9 03` sends `0x800A` and starts streaming: 150 ms of silence first, then the audio, one `FE` report every
   15 ms (catching up at most 4 reports if the loop was late). If the buffer runs dry mid-stream it sends silence,
   so the 1 s gap timer never fires.
4. After the audio ends, sends about 0.4 s of silence and releases the key.
5. On `F9 04` stops and sends `0x800D`.

Tested on 2026-11-20: TTS saying "Netflix" opened Netflix (`F9 03` after 835 ms, 145 audio reports, then `F9 04`).

If HTTPS media downloads fail with mbedTLS error `-0x7F00` (out of memory next to Bluetooth and Wi-Fi), enable
mbedTLS dynamic buffers in the ESP-IDF configuration.
