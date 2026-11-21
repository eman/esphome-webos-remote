# Pointer: how the TV turns motion reports into movement

A Magic Remote does not send screen positions. It sends raw gyroscope and accelerometer readings, and the TV's
motion library (Hillcrest MotionEngine, inside the TV's input service `lginput2`) turns them into an absolute
pointer position. To move the pointer by a chosen number of pixels, the remote has to model what the TV does.

Everything here was worked out on **one TV: an LG OLED C9 (2019, webOS 4.5, firmware 05.50.00)**. Labels as in
[bluetooth.md](bluetooth.md#scope-and-evidence): **Tested** (measured on the C9 with this component, 2026-11-20),
**Firmware** (read in the C9's firmware), **Inferred**, **Copied** (from captures of real remotes published by
other projects). The model described here was also checked by running the
TV's own motion code in an emulator against random and recorded input.

## Prerequisites

- The TV must have accepted a motion calibration block (`F9 17`, see
  [bluetooth.md](bluetooth.md#f9-17-motion-calibration-scd)). Without it the pointer appears but never moves.
- Pointer reports are `FD` reports in mode 1 (pointing); see the
  [FD layout](bluetooth.md#fd-report-layout-19-bytes).

## Session

| Step | What the remote sends | Status |
|---|---|---|
| Start | One mode-2 `FD` report with key `0x803E` and sequence 0 | Copied from a real remote, Tested |
| Motion | Mode-1 `FD` reports, one per 10 ms (100 Hz), sequence +1 each, gyro and accelerometer filled in | Tested |
| Key during the session | Put the key code in the motion reports. OK clicks at the pointer | Tested |
| End | One `FD` report with key `0x803F`, keeping the last sequence number | Tested |

A navigation key (an arrow, Back and so on) hides the pointer; a real remote ends the session then, and so does
this project (Tested).

## Units and axes

| Item | Value | Status |
|---|---|---|
| Gyro scale | 0.0010653 rad/s per count (about 0.061 °/s; 2000 °/s full scale) | Firmware, Tested |
| Gyro axis 2 | Horizontal (yaw); positive moves right | Tested |
| Gyro axis 0 | Vertical (pitch); positive moves down | Tested |
| Gyro axis 1 | Roll; no pointer movement | Tested |
| Accelerometer | About 4096 counts per g; used for tilt. This project sends gravity on -z | Inferred, Tested |
| Report period | Nominal 10 ms, from the calibration block. The TV does not measure the time between reports: each report with the next sequence number counts as one period | Firmware |
| Lost reports | A skipped sequence number loses that report's motion | Firmware |
| Output | Absolute pixels in a 1920×1080 space, clamped to 0-1914 by 0-1074 | Firmware |

## The TV's processing chain

For each report the TV runs these stages in order (Firmware):

1. **Sensor fusion.** Estimates the remote's orientation and the gyro bias (drift), and subtracts the bias.
2. **Tilt compensation.** Converts body rates into screen-horizontal and screen-vertical rates using the estimated
   tilt. Above about 75° of pitch the data is treated as invalid.
3. **Tremor filter.** Removes hand tremor (below).
4. **Ballistic gain.** Turns the rate into a pixel step with an accelerating gain (below).
5. **Alignment.** Nudges each step towards where the pointer "should" be for the angle turned (below).

### Ballistic gain

- Below 0.02 rad/s (about 19 counts/s) the gain is zero: a dead zone (Firmware).
- Above it, the gain in pixels per radian comes from an accelerating table indexed by the rate, times a speed scale
  from the TV's pointer-speed setting, times 1.5 (1080 / 720). The low end of the table (0.02-0.14 rad/s) gives
  roughly 200-1400 px/rad; faster turns get more pixels per radian (Firmware).
- The pointer-speed setting selects a scale of 1.18 or 0.885 (and, for one setting, a different table). The C9
  tested was on the setting with scale 0.885; measurements matched the model with that value (Tested).
- Sub-pixel remainders are carried over between reports (Firmware).

Measured gain without alignment effects, in pixels per (count × second) (Tested):

| Rate (counts/s) | Measured | Model, scale 0.885 |
|---:|---:|---:|
| 75 | 0.4 | 0.98 |
| 150 | 2.1 | 2.02 |
| 300 | 3.2 | 2.71 |
| 1200 | 4.6 | 5.64 (4.7 after the alignment clamp) |

The low-speed shortfall is explained by bias learning and the tremor filter (below).

### Alignment

On top of the ballistic step the TV keeps a **target**: an anchor position plus K × the angle turned since the
anchor, measured from the fusion's orientation estimate (Firmware).

- Each step is rotated by at most 2° towards the target and scaled by a factor r, clamped to 1/1.2 … 1.2, that
  would bring it onto the target (Firmware).
- **K is 1994 px/rad for the first 100 moving reports** (reports with a gain above zero), **then the running mean
  of the gains actually used** (Firmware, Tested). K changes the whole offset since the anchor, so a move that
  spans the switch at report 100 gets re-mapped part-way.
- **The ±20% rule.** While every report's own gain stays within 20% of K, the clamp never limits r, and the pointer
  ends up at K × angle **whatever the speed**. On the tested speed setting that holds for about 130-290 counts/s
  (Inferred from the model, consistent with measurements).
- K persists across pointer sessions on one Bluetooth connection and is rebuilt after a reconnect (Tested).
- Reports sent while the pointer is hidden (such as a wake shake) do not count towards K (Tested: the model only
  fits the measurements without them).
- Anchors are reset at the start of a session, when the tilt validity changes (pitch above 36°, with hysteresis up
  to 75°), and at the screen edges (Firmware). Alignment is off while the pointer is on an edge.
- The 2° rotation towards the target puts 10-25 px of vertical movement on long horizontal moves (Firmware,
  Tested).

### Tremor filter

A Kalman filter tuned to hand tremor around 9 Hz (Firmware). Its effect, from the emulated filter at a 10 ms
period:

| Input | Output |
|---|---|
| Sine up to 2 Hz | Gain 1.00, 6-7 ms delay |
| 5 Hz | 0.97 |
| 9 Hz | 0.41 |
| Step of 210 counts/s | 64% on the first report, about 80% for the next three, overshoot to 110% around report 9, settled after about 20 reports |
| Stop | 33%, 22%, 22%, 20%, 14% over the next reports, then a small undershoot |

The tails of a move run at low speed, where the gain is small, so **short moves come out short**: a 30 px move gives
about 17-21 px on the TV (Tested). Long moves are not affected. The filter is bypassed above about 5.2 rad/s
(Firmware).

## Gyro-bias learning, and why jitter is added

The fusion stage learns the gyro bias: when the remote looks "stable", it takes the mean gyro reading as drift and
subtracts it from then on (Firmware). A synthetic remote runs into this in three ways (all Tested):

| Input | What the TV does |
|---|---|
| A steady turn with little noise | Learned as drift and cancelled: 300 counts/s for 1 s with ±6 counts of noise moved the pointer 276-427 px instead of about 680 |
| Gyro exactly `0 0 0` at rest | The TV decides the remote was put down and hides the pointer within about 1 s |
| Gyro noise above about 20 counts at rest | The TV cannot re-learn zero drift; a bias learned earlier keeps moving the pointer by itself |

Adding ±40 counts of gyro noise to a steady turn stopped the cancellation; accelerometer noise did not help.

What this project does:

- Every motion report carries small random jitter: gyro ±6 counts, accelerometer ±40 counts. That keeps the pointer
  visible at rest and lets the TV keep its bias at zero.
- Moves are never steady: the turn rate varies sinusoidally (below), so the TV does not take it for drift.
- If the pointer drifts by itself, a few seconds of still reports (gyro 0, jitter of 6 or less) let the TV
  re-learn zero.

## Hiding and waking the pointer

- After about 5.5 s without motion, or after a navigation key, the TV hides the pointer and sends `F9 02` (Tested).
- Motion alone does not bring it back. A **wake shake** does: the TV's shake detector reacts to quick alternating
  yaw strokes (Firmware, Tested). This project sends gyro axis 2 at ±1600 counts, 4 strokes of 80 ms, and waits
  600 ms before moving. The pointer reappears at the screen centre (Tested).
- A wheel notch also shows the pointer at the screen centre (Tested).

## How `move_pointer` plans a move

From the model above:

1. If no session is running, send the wake shake; the pointer is then at the screen centre.
2. On a fresh session, first turn back and forth: 8 strokes with zero net angle (about 1 s, about ±55 px on
   screen). This gets the TV past the K switch at report 100 without moving the pointer.
3. Turn through **angle = distance / 2156 px/rad**, along the direction of the move, and stop on the exact angle.
4. Turn at **210 ± 35 counts/s**, varied as a 5 Hz sine. That speed keeps each report inside the ±20% band, and
   2156 px/rad is this glide's mean gain on the tested speed setting, so the TV's K settles at the same value the
   planner uses.

## Measured accuracy

On the C9, 2026-11-20, using chains of 12 moves (Tested):

| Result | Value |
|---|---|
| Repeatability between runs | A few pixels |
| Typical error | Most moves within about 10% |
| Example: request 800, 300 | 762-773, 303-308 |
| Example: request 600, 0 | 522-617 |
| Request 30 px | 17-21 px (tremor filter) |
| First chain after an ESP32 restart | Up to 10% different (the TV rebuilds its state on reconnect) |

A full model of the TV's cursor path (fusion bias aside) reproduces the measured chains to about 20 px rms. The
remaining differences follow the state of the TV's bias estimate, which the remote cannot observe.
