#!/bin/bash
# Type check the Air2 RX toggle code that lives in hotspot/src/os/bsd/vm/os_bsd.cpp
# without an iOS SDK: a verbatim slice of that file is compiled against hotspot
# shaped stub declarations.
#
# usage: ./run_check.sh /path/to/openjdk   (a tree with jdk8u_ios_rx_toggle.diff applied)
set -e
OPENJDK=${1:?usage: run_check.sh /path/to/openjdk}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$OPENJDK/hotspot/src/os/bsd/vm/os_bsd.cpp
[ -f "$SRC" ] || { echo "no $SRC"; exit 1; }

python3 "$HERE/slice_block.py" "$SRC" "$HERE/rx_block.inc"

for CXX in clang++ g++; do
  command -v $CXX >/dev/null || { echo "$CXX: not installed, skipped"; continue; }
  echo "== $CXX"
  $CXX -std=gnu++98 -fsyntax-only -Wall -Wno-unused-parameter -Wno-unused-function \
       "$HERE/rx_harness.cpp"
  echo "   ok"
done
