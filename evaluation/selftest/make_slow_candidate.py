#!/usr/bin/env python3
# nanoalloc evaluation package - self-test helper.
# SPDX-License-Identifier: MIT
"""Create a deliberately slower but valid copy of an implementation tree.

Copies SOURCE to DESTINATION and inserts a fixed busy-wait at the start of
nanoalloc::Pool::allocate() and nanoalloc::Pool::deallocate(). Allocation
results are unchanged, so the copy passes the correctness gate exactly as the
original does, and the evaluator must report it as slower. Used to check that
the performance gate detects a genuine slowdown:

    python3 selftest/make_slow_candidate.py ORIGINAL SLOW --spin 200
    python3 evaluate.py --baseline BUDDY --candidate SLOW --threshold 90 ...

Works on any tree that defines both functions (out of line, with the
baseline's signatures) in a .cpp file under src/.
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

DEFINITION = re.compile(
    r"(\b(?:void\s*\*\s*|void\s+)Pool::(?:allocate|deallocate)\s*\([^)]*\)\s*(?:noexcept\s*)?\{)")
HELPER = """
// Inserted by the nanoalloc evaluation self-test (make_slow_candidate.py).
namespace {{
void nanoeval_selftest_delay() noexcept {{
    static volatile unsigned counter = 0;
    for (unsigned i = 0; i < {spin}u; ++i) {{
        counter = counter + 1u;
    }}
}}
}}  // namespace
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--spin", type=int, default=200,
                        help="busy-wait loop iterations per allocate/deallocate call")
    args = parser.parse_args()
    if args.destination.exists():
        sys.exit(f"{args.destination} exists; remove it first")
    shutil.copytree(args.source, args.destination,
                    ignore=shutil.ignore_patterns(".git", "build", "build-*"))
    patched = 0
    for path in sorted((args.destination / "src").rglob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        if not DEFINITION.search(text):
            continue
        text, count = DEFINITION.subn(r"\1\n    nanoeval_selftest_delay();", text)
        # Define the helper after the last #include, before any use.
        includes = list(re.finditer(r"^#include[^\n]*\n", text, re.MULTILINE))
        position = includes[-1].end() if includes else 0
        text = text[:position] + HELPER.format(spin=args.spin) + text[position:]
        path.write_text(text, encoding="utf-8")
        patched += count
        print(f"patched {count} definition(s) in {path.relative_to(args.destination)}")
    if patched < 2:
        sys.exit("expected definitions of Pool::allocate and Pool::deallocate in src/*.cpp")
    print(f"slow candidate written to {args.destination} (spin {args.spin})")


if __name__ == "__main__":
    main()
