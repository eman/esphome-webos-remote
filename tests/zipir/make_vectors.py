#!/usr/bin/env python3
"""Generate synthetic ZipIR test vectors for test_zipir.c.

Every code set here is made up: timings, frames and ids are invented for the test, and nothing comes from a real
code-set database. The encoder follows docs/protocol/universal-control.md. Expected waveforms are computed from the
same high-level description the encoder starts from (lists of durations), not by decoding the bytes, so the C
decoder is checked against the format description rather than against itself.

    python3 make_vectors.py            # rewrite vectors.h
    python3 make_vectors.py --check    # fail if vectors.h is out of date
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "vectors.h")


def bitlen(n):
    """Bits needed for the values 0..n, at least 1."""
    return max(1, n.bit_length())


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, width):
        if value < 0 or value >= (1 << width):
            raise ValueError(f"{value} does not fit in {width} bits")
        self.bits += [(value >> (width - 1 - i)) & 1 for i in range(width)]

    def bytes(self):
        b = self.bits + [0] * (-len(self.bits) % 8)  # pad the last byte with zeros
        return bytes(int("".join(map(str, b[i : i + 8])), 2) for i in range(0, len(b), 8))


class CodeSet:
    """A code set described the way the format stores it.

    ct: characteristic times in microseconds; the first `nlt` are "long" (stored / 8), the rest "short" (/ 4).
    rs: repetition sequences, lists of CT indices.
    ws: waveform substitutions (frames), lists of RS indices.
    functions: per function number (key * sqc + toggle), None or a dict with any of "M", "R", "B" -> WS index.
    """

    def __init__(self, type_letter, number, cp, dc, ct, nlt, rs, ws, functions, sqc=1, em=0, lrs=None, lws=None):
        self.type_letter, self.number = type_letter, number
        self.cp, self.dc, self.ct, self.nlt = cp, dc, ct, nlt
        self.rs, self.ws, self.functions, self.sqc, self.em = rs, ws, functions, sqc, em
        self.lrs = lrs if lrs is not None else max(0, bitlen(max(len(r) for r in rs) - 1) - 3)
        self.lws = lws if lws is not None else max(0, bitlen(max(len(w) for w in ws) - 1) - 3)

    def protocol(self):
        w = BitWriter()
        w.put(0, 4)  # VER
        w.put(self.em, 4)
        w.put(self.cp, 12)
        w.put(self.dc, 4)
        w.put(len(self.ct) - 1, 6)
        w.put(self.nlt, 4)
        for i, t in enumerate(self.ct):
            unit, width = (8, 16) if i < self.nlt else (4, 12)
            if t % unit:
                raise ValueError(f"CT {t} us is not a multiple of {unit} us")
            w.put(t // unit, width)
        w.put(len(self.rs) - 1, 6)
        w.put(self.lrs, 2)
        for r in self.rs:
            w.put(len(r) - 1, self.lrs + 3)
        for r in self.rs:
            for c in r:
                w.put(c, bitlen(len(self.ct) - 1))
        w.put(len(self.ws) - 1, 10)
        w.put(self.lws, 3)
        for s in self.ws:
            w.put(len(s) - 1, self.lws + 3)
        for s in self.ws:
            for r in s:
                w.put(r, bitlen(len(self.rs) - 1))
        return w.bytes()

    def keys(self):
        bws = bitlen(len(self.ws) - 1)
        w = BitWriter()
        w.put(bws, 4)
        w.put(self.sqc - 1, 4)
        for f in self.functions:
            f = f or {}
            wsp = (4 if "M" in f else 0) | (2 if "R" in f else 0) | (1 if "B" in f else 0)
            w.put(wsp, 3)
            for name in "MRB":
                if name in f:
                    w.put(f[name], bws)
        return w.bytes()

    def frame(self, ws_index):
        """Expected durations of one frame: + mark, - space, alternating from a mark."""
        times = [self.ct[c] for r in self.ws[ws_index] for c in self.rs[r]]
        return [t if i % 2 == 0 else -t for i, t in enumerate(times)]

    def waveform(self, key, toggle):
        """Expected (carrier, duty, sqc, [M, R, B]) or None if the key is missing."""
        n = key * self.sqc + toggle % self.sqc
        f = self.functions[n] if n < len(self.functions) else None
        if not f:  # an empty toggle state falls back to toggle 0
            n0 = key * self.sqc
            f = self.functions[n0] if n0 < len(self.functions) else None
        if not f:
            return None
        frames = [self.frame(f[name]) if name in f else [] for name in "MRB"]
        carrier = 100_000_000 // self.cp if self.cp else 0
        return carrier, self.dc * 10, self.sqc, frames


def container(codesets, fcd=0x5A1E7E57):
    """ZipIR file: VER, FSZ, FCD, type table, code set records, data. Code sets must be grouped by type."""
    types = []
    for i, cs in enumerate(codesets):
        if types and types[-1][0] == cs.type_letter:
            types[-1][1] += 1
        else:
            types.append([cs.type_letter, 1, i])
    types.append(["\0", 0, len(codesets)])  # terminator entry
    data, records = b"", []
    for cs in codesets:
        p, k = cs.protocol(), cs.keys()
        records.append((cs.number, len(data), len(p)))
        data += p + k
    records.append((0xFFFF, len(data), 0))  # terminator record: end of the last code set's data
    head = bytes([len(types)])
    for letter, count, first in types:
        head += bytes([ord(letter)]) + count.to_bytes(2, "big") + first.to_bytes(2, "big")
    head += len(records).to_bytes(2, "big")
    for number, offset, proto_len in records:
        head += number.to_bytes(2, "big") + offset.to_bytes(4, "big") + proto_len.to_bytes(2, "big")
    size = 1 + 4 + 4 + len(head) + len(data)
    return bytes([3]) + size.to_bytes(4, "big") + fcd.to_bytes(4, "big") + head + data


# ---- Synthetic code sets ------------------------------------------------------------------------------------------


def nec_like():
    """A made-up pulse-distance protocol in the style of NEC: header, 32 bits, stop mark, gap; a short repeat frame."""
    ct = [40_000, 96_000, 9000, 4500, 2248, 560, 564, 1688]
    # RS: 0 header, 1 bit 0, 2 bit 1, 3 stop + gap, 4 repeat frame, 5 a third ("B") frame
    rs = [[2, 3], [5, 6], [5, 7], [5, 0], [2, 4, 5, 1], [5, 7, 5, 0]]

    def data_frame(value):
        return [0] + [2 if (value >> (31 - i)) & 1 else 1 for i in range(32)] + [3]

    ws = [data_frame(0xA55A0FF0), [4], data_frame(0x12ED00FF), data_frame(0x0000FFFF), [5]]
    functions = [
        {"M": 0, "R": 1},  # key 0: M + R
        {"M": 2, "R": 1},  # key 1: M + R, another command
        None,  # key 2: missing
        {"M": 3},  # key 3: M only
        {"M": 0, "R": 1, "B": 4},  # key 4: M + R + B
        {"R": 1},  # key 5: R only
        {"R": 1, "B": 4},  # key 6: R + B
        {"M": 3, "B": 4},  # key 7: M + B
        {"B": 4},  # key 8: B only
    ]
    return CodeSet("S", 0x0F01, cp=2632, dc=3, ct=ct, nlt=2, rs=rs, ws=ws, functions=functions)


def toggle_set():
    """A made-up biphase protocol with a toggle bit: two functions (toggle states) per key, 36 kHz, 30 % duty."""
    ct = [88_896, 888, 1776]
    # Durations are stored as single-entry RS so each frame is a plain list of CT indices.
    rs = [[0], [1], [2]]

    def frame(pattern):  # pattern of 1 (one half-bit) and 2 (two half-bits), alternating mark/space, then the gap
        p = list(pattern)
        if len(p) % 2 == 0:
            p.append(1)
        return p + [0]

    ws = [frame([1, 1, 2, 2, 1, 1, 2, 1, 1]), frame([1, 1, 1, 1, 2, 2, 1, 1, 2]), frame([2, 1, 1, 2, 2, 2, 1])]
    functions = [
        {"M": 0},  # key 0, toggle 0
        {"M": 1},  # key 0, toggle 1
        {"M": 2},  # key 1, toggle 0
        None,  # key 1, toggle 1: empty, falls back to toggle 0
        None,  # key 2: missing in both states
        None,
    ]
    return CodeSet("C", 0x0A2B, cp=2778, dc=3, ct=ct, nlt=1, rs=rs, ws=ws, functions=functions, sqc=2)


def baseband_set():
    """No carrier (CP 0, DC 10): the output is plain on/off."""
    ct = [20_000, 400, 800]
    rs = [[1, 2], [2, 1], [1, 0]]
    ws = [[0, 1, 0, 0, 1, 2]]
    return CodeSet("C", 0x0A2C, cp=0, dc=10, ct=ct, nlt=1, rs=rs, ws=ws, functions=[{"M": 0}])


def em1_set():
    cs = nec_like()
    cs.em = 1
    return cs


# ---- Output -------------------------------------------------------------------------------------------------------


def c_bytes(name, data):
    rows = [", ".join(f"0x{b:02x}" for b in data[i : i + 16]) for i in range(0, len(data), 16)]
    return f"static const uint8_t {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def c_ints(name, values):
    if not values:
        return ""
    rows = [", ".join(str(v) for v in values[i : i + 12]) for i in range(0, len(values), 12)]
    return f"static const int32_t {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def press_order(frames):
    m, r, b = (len(f) > 0 for f in frames)
    if m and r and b:
        return [0, 1, 2]
    if m and r:
        return [0, 1, 1]
    for j, present in enumerate((m, r, b)):
        if present:
            return [j] * 3
    return []


def patched(data, offset, new):
    return data[:offset] + bytes(new) + data[offset + len(new) :]


def generate():
    nec, tog, base = nec_like(), toggle_set(), baseband_set()
    files = {
        "zf_nec": container([nec]),
        "zf_multi": container([nec, tog, base]),
        "zf_em1": container([em1_set()]),
    }
    multi = [nec, tog, base]

    # Offsets inside zf_nec (one type entry + terminator, two records): header 10, types 2 x 5, CC at 20,
    # records at 22 (number, CSS, FTO), data at 38.
    nec_file = files["zf_nec"]
    rec0 = 10 + 5 * nec_file[9] + 2
    data0 = rec0 + 8 * int.from_bytes(nec_file[rec0 - 2 : rec0], "big")
    proto_len = len(nec.protocol())
    bad = {
        "zf_short": nec_file[:11],  # shorter than the fixed header
        "zf_cut_records": nec_file[: rec0 + 4],  # record table cut off
        "zf_cut_data": nec_file[: data0 + proto_len // 2],  # data cut in the middle of the protocol part
        # protocol length (FTO) larger than the code set's data
        "zf_bad_fto": patched(nec_file, rec0 + 6, (0x7FFF).to_bytes(2, "big")),
        # protocol part claims 64 CTs (NCT field, the top 6 bits of its 4th byte, all ones) and runs out of bits
        "zf_bad_nct": patched(nec_file, data0 + 3, [nec_file[data0 + 3] | 0xFC]),
        # first code set record points past the end of the file
        "zf_bad_css": patched(nec_file, rec0 + 2, (0x00100000).to_bytes(4, "big")),
        # offset + protocol length wraps around 32 bits (a decoder bug, fixed 2026-11-20)
        "zf_wrap_css": patched(nec_file, rec0 + 2, (0xFFFFFFF0).to_bytes(4, "big")),
    }
    # A WS that refers to an RS that doesn't exist: rebuild the NEC set with RS index 7 in its repeat frame
    # (6 RS -> 3-bit indices, so 7 is encodable but out of range).
    broken = nec_like()
    broken.ws[1] = [7]
    bad["zf_bad_rs"] = container([broken])
    files.update(bad)

    # Wave cases: (name, file, code set index, key, toggle, expected rc, expectation source or None, known_bug)
    cases = []

    def ok(name, fname, cs, index, key, toggle):
        cases.append((name, fname, index, key, toggle, 0, cs.waveform(key, toggle), 0))

    for key in (0, 1, 3, 4, 5, 6, 7, 8):
        ok(f"nec key {key}", "zf_nec", nec, 0, key, 0)
    ok("nec key 0 toggle 1 (sqc 1 ignores the toggle)", "zf_nec", nec, 0, 0, 1)
    ok("multi: nec as code set 0", "zf_multi", nec, 0, 1, 0)
    for key, toggle in ((0, 0), (0, 1), (0, 2), (0, 3), (1, 0), (1, 1)):
        ok(f"toggle key {key} toggle {toggle}", "zf_multi", tog, 1, key, toggle)
    ok("baseband key 0", "zf_multi", base, 2, 0, 0)
    for name, fname, index, key, toggle in (
        ("nec key 2 missing", "zf_nec", 0, 2, 0),
        ("nec key 9 past the last function", "zf_nec", 0, 9, 0),
        ("nec key 200 far past the end", "zf_nec", 0, 200, 0),
        ("toggle key 2 missing in both states", "zf_multi", 1, 2, 1),
        ("baseband key 1 missing", "zf_multi", 2, 1, 0),
    ):
        cases.append((name, fname, index, key, toggle, 1, None, 0))
    cases.append(("code set index 3 out of range", "zf_multi", 3, 0, 0, -1, None, 0))
    cases.append(("code set index -1", "zf_multi", -1, 0, 0, -1, None, 0))
    cases.append(("EM 1 is not supported", "zf_em1", 0, 0, 0, -2, None, 0))
    for fname in bad:
        cases.append((f"malformed {fname[3:]}", fname, 0, 0, 0, -1, None, 0))

    # Code set cases: (file, index, rc, type, number, protocol length, key part length)
    cs_cases = [("zf_nec", 0, 0, "S", nec.number, len(nec.protocol()), len(nec.keys()))]
    for i, cs in enumerate(multi):
        cs_cases.append(("zf_multi", i, 0, cs.type_letter, cs.number, len(cs.protocol()), len(cs.keys())))
    cs_cases += [("zf_multi", 3, -1, "?", 0, 0, 0), ("zf_short", 0, -1, "?", 0, 0, 0)]
    cs_cases += [("zf_cut_records", 0, -1, "?", 0, 0, 0), ("zf_bad_css", 0, -1, "?", 0, 0, 0)]

    out = [
        "// Generated by make_vectors.py from synthetic code sets; do not edit. Regenerate: python3 make_vectors.py\n",
        "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n",
    ]
    for name, data in files.items():
        out.append(c_bytes(name, data))
    out.append("\n")
    wave_rows = []
    for n, (name, fname, index, key, toggle, rc, exp, known) in enumerate(cases):
        if exp:
            carrier, duty, sqc, frames = exp
            ptrs = []
            for j, f in enumerate(frames):
                out.append(c_ints(f"w{n}_{'MRB'[j]}", f))
                ptrs.append(f"w{n}_{'MRB'[j]}" if f else "NULL")
            order = press_order(frames)
            lens = ", ".join(str(len(f)) for f in frames)
            ords = ", ".join(str(o) for o in order + [0] * (3 - len(order)))
            wave_rows.append(
                f'    {{"{name}", {fname}, sizeof({fname}), {index}, {key}, {toggle}, {rc}, {known}, {carrier}u, '
                f"{duty}, {sqc}, {{{', '.join(ptrs)}}}, {{{lens}}}, {len(order)}, {{{ords}}}}},"
            )
        else:
            wave_rows.append(
                f'    {{"{name}", {fname}, sizeof({fname}), {index}, {key}, {toggle}, {rc}, {known}, 0u, 0, 0, '
                f"{{NULL, NULL, NULL}}, {{0, 0, 0}}, 0, {{0, 0, 0}}}},"
            )
    out.append(
        "\ntypedef struct {\n  const char *name;\n  const uint8_t *file;\n  size_t len;\n"
        "  int index, key, toggle;\n  int rc;      // expected return value of zipir_waveform\n"
        "  int known_bug;  // 1 = skipped unless built with -DTEST_KNOWN_BUGS (none at present)\n"
        "  uint32_t carrier_hz;\n  uint8_t duty, sqc;\n  const int32_t *frame[3];  // M, R, B\n"
        "  uint16_t frame_len[3];\n  int order_n;\n  int order[3];\n} wave_case_t;\n\n"
        "static const wave_case_t WAVE_CASES[] = {\n" + "\n".join(wave_rows) + "\n};\n\n"
    )
    out.append(
        "typedef struct {\n  const char *file_name;\n  const uint8_t *file;\n  size_t len;\n  int index, rc;\n"
        "  char type;\n  uint16_t id;\n  size_t proto_len, keys_len;\n} codeset_case_t;\n\n"
        "static const codeset_case_t CODESET_CASES[] = {\n"
    )
    for fname, index, rc, t, number, pl, kl in cs_cases:
        out.append(f"    {{\"{fname}\", {fname}, sizeof({fname}), {index}, {rc}, '{t}', 0x{number:04X}, {pl}, {kl}}},\n")
    out.append("};\n")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if vectors.h differs from the generated output")
    args = ap.parse_args()
    text = generate()
    if args.check:
        try:
            with open(OUT) as f:
                current = f.read()
        except FileNotFoundError:
            current = None
        if current != text:
            print("tests/zipir/vectors.h is out of date: run python3 tests/zipir/make_vectors.py", file=sys.stderr)
            return 1
        return 0
    with open(OUT, "w") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
