`sbc_*.c` / `sbc_*.h`: the SBC encoder (mSBC mode used, for voice in FE reports), copied from ESP-IDF
`components/bt/host/bluedroid/external/sbc/encoder` (Broadcom, Apache-2.0). Kept flat because ESPHome only copies a
component's top-level sources. Changes from the original:
- `sbc_bt_types.h`, `sbc_bt_target.h`, `sbc_allocator.h` stand in for Bluedroid's `stack/bt_types.h`,
  `common/bt_target.h` and `osi/allocator.h` (we use NimBLE, so Bluedroid isn't built); the includes were renamed.
- `SINT32` is `int32_t` instead of `long` (identical on the ESP32; keeps host test builds the same).
- `APPL_TRACE_EVENT` is a no-op.
- `sbc_enc_func_declare.h` and `sbc_dct.h` include `sbc_encoder.h` themselves (ESPHome includes every header of a
  component on its own, in no particular order).
Host test: `tests/msbc/`.
