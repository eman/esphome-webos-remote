"""Fails if the repository contains things that must not be published.

Always checked: private IPv4 addresses, email addresses, and Bluetooth/MAC addresses other than the placeholders
listed in ALLOWED_MACS. Optional, from files outside the repository (so the patterns themselves stay private):
  --patterns FILE   one case-insensitive regular expression per line (# comments allowed)
  --secrets FILE    KEY=VALUE or `key: value` lines; each value is searched for literally and never printed
Only file names and line numbers are reported, never the matching text of a secret.

Run from the repository root: python3 scripts/scrub_check.py [--patterns F] [--secrets F ...]
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRIVATE_IP = re.compile(
    r"\b(10\.\d{1,3}|192\.168|172\.(1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b"
)
EMAIL = re.compile(r"\b[\w.+-]+@[\w-]+(\.[\w-]+)*\.[A-Za-z]{2,}\b")
MAC = re.compile(r"\b[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}\b")
ALLOWED_MACS = {"00:00:00:00:00:00", "AA:BB:CC:DD:EE:FF"}
ALLOWED_EMAIL = re.compile(r"noreply@|@users\.noreply\.github\.com$")
SKIP_SUFFIXES = (".pdf", ".png", ".jpg", ".zir", ".bin")


def tracked_files():
    try:
        out = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout
        return [f for f in out.splitlines() if f]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for d, _, names in os.walk(ROOT):
            if "/.git" in d or "/.esphome" in d:
                continue
            files += [os.path.relpath(os.path.join(d, n), ROOT) for n in names]
        return files


def secret_values(path):
    values = []
    for line in open(path, errors="replace"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^[\w.-]+\s*[:=]\s*(.*)$", line)
        if m:
            v = m.group(1).strip().strip("\"'")
            if len(v) >= 6:
                values.append(v)
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--patterns")
    ap.add_argument("--secrets", action="append", default=[])
    a = ap.parse_args()
    patterns = []
    if a.patterns:
        for line in open(a.patterns):
            line = line.strip()
            if line and not line.startswith("#"):
                patterns.append(re.compile(line, re.I))
    secrets = [v for p in a.secrets for v in secret_values(p)]

    problems = 0
    for rel in tracked_files():
        if rel.endswith(SKIP_SUFFIXES) or rel.startswith("tests/compile/secrets.yaml"):
            continue
        try:
            text = open(os.path.join(ROOT, rel), errors="replace").read()
        except (IsADirectoryError, FileNotFoundError):
            continue
        for n, line in enumerate(text.splitlines(), 1):
            found = []
            if PRIVATE_IP.search(line):
                found.append("private IP address")
            for m in EMAIL.finditer(line):
                if not ALLOWED_EMAIL.search(m.group(0)):
                    found.append("email address")
            for m in MAC.finditer(line):
                if m.group(0).upper() not in ALLOWED_MACS:
                    found.append(f"MAC address {m.group(0)}")
            for p in patterns:
                if p.search(line):
                    found.append(f"pattern /{p.pattern}/")
            if any(s in line for s in secrets):
                found.append("a secret value")
            for f in found:
                print(f"{rel}:{n}: {f}")
                problems += 1
    print(f"scrub check: {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
