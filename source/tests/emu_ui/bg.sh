#!/bin/sh
# D153: daemon + screen CSMs in elfloader3-linux (ui_demo over the fake PAN, bg.scn); checks 4:\uipolls.txt.
set -e
cd "$(dirname "$0")"
B=build
cmake -S . -B $B -DUI_PLATFORM=ELKA >/dev/null && cmake --build $B >/dev/null
O=$PWD/$B/bg
rm -rf "$O"; mkdir -p "$O/root"
timeout 120 python3 ../../../duke3d/tests/emu/run_elf.py "$PWD/$B/ui_demo.elf" "$O/root" "$O" "$PWD/bg.scn" > "$O/run.out" 2>&1 || true
LOG=$(find "$O/root" -iname uipolls.txt | head -1)
python3 - "$LOG" <<'P'
import sys
log = open(sys.argv[1], encoding='latin1').read().splitlines()
def at(s, start=0):
    return next(i for i in range(start, len(log)) if s in log[i])
idle = at('emu: DoIDLE closes')
assert 'closes 1 CSM' in log[idle], log[idle]          # only the screen: the daemon is below idle
again = at('emu: started again', idle)
mid = [l for l in log[idle + 1:again] if l.startswith('t=')]
assert mid and all('dmn=1 scr=1' in l and 'state=Connected' in l for l in mid), mid  # D154: the screen is made again (hidden on the phone; the emulator runs no MMI move)
print('DoIDLE: screen closed, daemon + link kept for %d s' % len(mid))
idle2 = at('emu: DoIDLE closes', again)
assert 'closes 1 CSM' in log[idle2], log[idle2]
back = [l for l in log[again + 1:idle2] if l.startswith('t=')]
assert back and all('dmn=1 scr=1 ' in l + ' ' and 'top=1' in l for l in back), back
print('started again: screen back on top')
again2 = at('emu: started again', idle2)
mid = [l for l in log[idle2 + 1:again2] if l.startswith('t=')]
assert mid and all('dmn=1 scr=1' in l and 'state=Connected' in l for l in mid), mid  # D154: the screen is made again (hidden on the phone; the emulator runs no MMI move)
top = [l for l in log[again2 + 1:] if l.startswith('t=') and 'top=1' in l]
assert top, 'screen not back after the second start'
print('second DoIDLE + start: same')
assert log[-1] == 'all closed', log[-3:]
print('Exit: disconnected, screen + daemon closed')
P
