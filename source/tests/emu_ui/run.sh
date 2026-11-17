#!/bin/sh
# Build the UI demo (src/app_ui.h over a fake PAN) and play ui.scn in elfloader3-linux;
# screenshots -> $1 (default build/shots). PLATFORM=NSG: the non-ELKA NewSGOLD build (S75, 132x176) in
# elfloader3-nsg, build dir build-nsg.
set -e
cd "$(dirname "$0")"
B=build
if [ "$PLATFORM" = NSG ]; then B=build-nsg; export EMU_BIN=elfloader3-nsg; fi
cmake -S . -B $B -DUI_PLATFORM=${PLATFORM:-ELKA} >/dev/null && cmake --build $B
OUT=${1:-$PWD/$B/shots}
mkdir -p $B/root
python3 ../../../duke3d/tests/emu/run_elf.py "$PWD/$B/ui_demo.elf" "$PWD/$B/root" "$OUT" "$PWD/ui.scn"
