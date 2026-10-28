#!/usr/bin/env python3
"""Lists the layout of a Call of Duty / United Offensive installation's executables.

Usage: python3 tools/inspect_game.py "/path/to/Call of Duty"

For every single-player executable and game DLL found (CoD: CoDSP.exe, gamex86.dll,
cgamex86.dll, uix86.dll; United Offensive: CoDUOSP.exe and the uo_*.dll files) it
prints what porting needs to know: timestamp, image base and size, entry point,
sections, whether the code is protected, and which DLLs it imports. It only reads
the files' headers and prints numbers and names: no game code."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'recomp'))
from pe import PE  # noqa: E402

GAMES = {
    'Call of Duty': ['CoDSP.exe', 'gamex86.dll', 'cgamex86.dll', 'uix86.dll'],
    'United Offensive': ['CoDUOSP.exe', 'uo_gamex86.dll', 'uo_cgamex86.dll', 'uo_uix86.dll'],
}
# Sections left in executables wrapped by SafeDisc (their code is encrypted on disk).
SAFEDISC_SECTIONS = {'.stxt774', '.stxt371', 'stxt774', 'stxt371'}


def find(game_dir, name):
    for entry in os.listdir(game_dir):
        if entry.lower() == name.lower():
            return os.path.join(game_dir, entry)
    return None


def entropy(data):
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = float(len(data))
    return max(0.0, -sum(c / n * math.log2(c / n) for c in counts if c))


def describe(path):
    pe = PE(path)
    d = pe.data
    pe_off = struct.unpack_from('<I', d, 0x3C)[0]
    timestamp = struct.unpack_from('<I', d, pe_off + 8)[0]
    names = [s.name for s in pe.sections]
    safedisc = sorted(SAFEDISC_SECTIONS & set(names))
    text = next((s for s in pe.sections if s.name in ('.text', 'text')), None)
    bits = entropy(d[text.raw_off:text.raw_off + text.raw_size]) if text and text.raw_size else 0.0
    # Encrypted code looks random (close to 8 bits per byte); SafeDisc's sections can
    # stay behind in an executable whose code is plain.
    protected = bits > 7.5
    print('  %-16s %9d B  timestamp %08X  base %08X  size %08X  end %08X  entry %08X' % (
        os.path.basename(path), len(d), timestamp, pe.image_base, pe.size_of_image,
        pe.image_base + pe.size_of_image, pe.entry))
    print('      sections: %s' % ', '.join('%s@%08X+%X' % (s.name, s.va, max(s.vsize, s.raw_size))
                                         for s in pe.sections))
    print('      relocations: %s' % ('yes' if pe.relocs else 'no'))
    if text and text.raw_size:
        print('      .text entropy: %.2f bits/byte (plain x86 code about 6-6.7, encrypted close to 8)' % bits)
    dlls = sorted({dll.lower() for dll, _ in pe.imports.values()}) if pe.imports else []
    print('      imports from: %s' % (', '.join(dlls) or '(none)'))
    if protected:
        print('      PROTECTED: the code is encrypted and cannot be recompiled')
    elif safedisc:
        print('      SafeDisc sections %s present, code not encrypted: usable' % ', '.join(safedisc))
    return not protected


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    game_dir = sys.argv[1]
    for game, files in GAMES.items():
        print('%s:' % game)
        for name in files:
            path = find(game_dir, name)
            if not path:
                print('  %-16s MISSING' % name)
                continue
            try:
                describe(path)
            except Exception as e:   # a damaged or unexpected file: report and go on
                print('  %-16s cannot read: %s' % (name, e))
        print()
    uo_dir = find(game_dir, 'uo')
    if uo_dir and os.path.isdir(uo_dir):
        paks = sorted(p for p in os.listdir(uo_dir) if p.lower().endswith('.pk3'))
        print('uo/: %d .pk3 files' % len(paks))
    return 0


if __name__ == '__main__':
    sys.exit(main())
