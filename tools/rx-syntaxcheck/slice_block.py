#!/usr/bin/env python3
"""Extract the Air2 RX toggle code from a patched os_bsd.cpp for syntax checking.

The two regions are located structurally (not by line number) so this keeps
working as the file is edited.  The output is a verbatim slice - it is compiled
together with rx_harness.cpp, which supplies hotspot-shaped stub declarations, so
what gets type checked is exactly the code that ships.

usage: slice_block.py <path/to/os_bsd.cpp> <out.inc>
"""
import pathlib
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)

SRC = pathlib.Path(sys.argv[1])
OUT = pathlib.Path(sys.argv[2])
with open(str(SRC), "r", encoding="utf-8", newline="") as fh:
    lines = fh.read().splitlines()


def find(pred, start=0):
    for i in range(start, len(lines)):
        if pred(lines[i]):
            return i
    raise SystemExit("marker not found near line %d" % start)


def end_of_block(start):
    depth, seen = 0, False
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if "{" in lines[i]:
            seen = True
        if seen and depth == 0:
            return i
    raise SystemExit("unbalanced block from %d" % start)


a0 = find(lambda l: l.startswith("address os::Bsd::debug_jit_rx_base"))
a1 = end_of_block(find(lambda l: l.startswith("bool os::Bsd::a2_rx_handle_fault"), a0))
b0 = find(lambda l: l.startswith("static address a2_mirror_rx_lo"))
b1 = end_of_block(find(lambda l: l.startswith("address os::Bsd::mirrored_find_rx"), b0))

chunk = ["// ===== BEGIN verbatim copy from os_bsd.cpp (do not edit here) ====="]
chunk += lines[a0:a1 + 1] + [""] + lines[b0:b1 + 1]
chunk += ["// ===== END verbatim copy ====="]
OUT.write_text("\n".join(chunk) + "\n", encoding="utf-8", newline="\n")
print("region A: lines %d-%d, region B: lines %d-%d, wrote %s (%d lines)"
      % (a0 + 1, a1 + 1, b0 + 1, b1 + 1, OUT, len(chunk)))
