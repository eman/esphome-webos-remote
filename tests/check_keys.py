"""Checks the key tables against each other and against the LG code rule.

- Every IR code in lg_ir_remote is a well-formed LG code: 0x20DF, then a command byte and its complement.
- Every Bluetooth code in lg_magic_remote is 0x8000 | the IR command byte bit-reversed, for keys that have both
  (Power is 0x0008 on Bluetooth; digits are 0x8010 + n, generated in code).
- Every key in scripts/keys.py exists where it says it does.

Run from the repository root: python3 tests/check_keys.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from keys import KEYS  # noqa: E402

# Handled in LGMagicRemote::key() rather than in its table.
BT_SPECIAL = {"poweron", "poweroff"} | {str(n) for n in range(10)}
# Power has no 0x8000 prefix; Screen remote's Bluetooth code (0x80DE) and IR code (index 0x96) differ on the TV
# (docs/protocol/ir-keys.md).
BT_EXCEPTIONS = {"power": 0x0008, "screenremote": 0x80DE}


def table(path, width):
    text = open(os.path.join(ROOT, path)).read()
    body = text[text.index("KEYS[] = {") :]
    body = body[: body.index("};")]
    pat = r'\{"(\w+)",\s*0x([0-9A-Fa-f]{' + str(width) + r'})\}'
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(pat, body)}


def rev8(b):
    return int(f"{b:08b}"[::-1], 2)


def main():
    ir = table("components/lg_ir_remote/lg_ir_remote.cpp", 8)
    bt = table("components/lg_magic_remote/lg_magic_remote.cpp", 4)
    errors = []
    for name, code in ir.items():
        cmd, inv = (code >> 8) & 0xFF, code & 0xFF
        if code >> 16 != 0x20DF or cmd ^ inv != 0xFF:
            errors.append(f"IR {name}: 0x{code:08X} is not an LG code")
    for name, code in bt.items():
        if name in BT_EXCEPTIONS:
            if code != BT_EXCEPTIONS[name]:
                errors.append(f"Bluetooth {name}: 0x{code:04X}, expected 0x{BT_EXCEPTIONS[name]:04X}")
            continue
        if name in ir:
            want = 0x8000 | rev8((ir[name] >> 8) & 0xFF)
            if code != want:
                errors.append(f"{name}: Bluetooth 0x{code:04X} but IR 0x{ir[name]:08X} gives 0x{want:04X}")
    for label, name, _, where in KEYS:
        if "ir" in where and name not in ir:
            errors.append(f"keys.py {label}: '{name}' has no IR code")
        if "bt" in where and name not in bt and name not in BT_SPECIAL:
            errors.append(f"keys.py {label}: '{name}' has no Bluetooth code")
        if "ir" not in where and name in ir:
            errors.append(f"keys.py {label}: '{name}' has an IR code but is marked Bluetooth only")
    print(f"{len(ir)} IR codes, {len(bt)} Bluetooth codes, {len(KEYS)} buttons")
    for e in errors:
        print("ERROR", e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
