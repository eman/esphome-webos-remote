"""The compile matrix in .github/workflows/ci.yml must list exactly the tests/compile/*.yaml configs.

Run from the repository root: python3 scripts/check_ci_matrix.py (CI fails if the two drift). No PyYAML
dependency — the matrix block is a plain YAML list, scanned with a regex.
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ci = open(os.path.join(ROOT, ".github", "workflows", "ci.yml")).read()
m = re.search(r"matrix:\s*\n\s*config:\s*\n((?:\s*-\s*\S+\s*\n)+)", ci)
if not m:
    sys.exit("could not find the compile matrix config list in ci.yml")
matrix = set(re.findall(r"-\s*(\S+)", m.group(1)))

files = {os.path.basename(f)[:-5] for f in glob.glob(os.path.join(ROOT, "tests", "compile", "*.yaml"))}
files.discard("secrets")

if matrix != files:
    print("ci.yml compile matrix does not match tests/compile/*.yaml:")
    print("  only in ci.yml      :", sorted(matrix - files) or "(none)")
    print("  only in tests/compile:", sorted(files - matrix) or "(none)")
    sys.exit(1)
print(f"ci.yml compile matrix matches {len(files)} configs in tests/compile")
