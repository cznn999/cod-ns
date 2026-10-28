#!/usr/bin/env python3
"""Checks a Call of Duty (2003) installation before building the port.

Usage: python3 tools/check_game_files.py /path/to/the/game

Call of Duty: the recompiler needs CoDSP.exe of version 1.3 (timestamp 0x40443FDA,
the Steam release) with its code not encrypted, the three game DLLs and main/*.pk3.
United Offensive (optional): CoDUOSP.exe of the Steam release (timestamp
0x412653F0), the three uo_*.dll files and uo/*.pk3.

An executable counts as protected when its code section is encrypted (SafeDisc on
the retail CDs): encrypted code looks random, close to 8 bits of entropy per byte,
while plain x86 code is about 6 to 6.7. This script only reads the files and
reports what it finds."""
import math
import os
import struct
import sys

GAMES = [
    # name, executable, expected timestamp, DLLs, data folder, optional
    ("Call of Duty", "CoDSP.exe", 0x40443FDA, ["gamex86.dll", "cgamex86.dll", "uix86.dll"], "main", False),
    ("United Offensive", "CoDUOSP.exe", 0x412653F0,
     ["uo_gamex86.dll", "uo_cgamex86.dll", "uo_uix86.dll"], "uo", True),
]
ENCRYPTED_BITS = 7.5


def entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = float(len(data))
    return max(0.0, -sum(c / n * math.log2(c / n) for c in counts if c))


def pe_info(path):
    """(timestamp, entropy of the code section, file size), or None for a non-PE file."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    timestamp = struct.unpack_from("<I", data, pe + 8)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sec = pe + 24 + opt_size
    bits = 0.0
    for i in range(nsec):
        h = sec + 40 * i
        name = data[h:h + 8].rstrip(b"\0").decode("latin-1")
        raw_size, raw_off = struct.unpack_from("<II", data, h + 16)
        if name in (".text", "text"):
            bits = entropy(data[raw_off:raw_off + raw_size])
    return timestamp, bits, len(data)


def find(game_dir, name):
    """Case-insensitive lookup (the SD card does not care about case; Linux does)."""
    for entry in os.listdir(game_dir):
        if entry.lower() == name.lower():
            return os.path.join(game_dir, entry)
    return None


def check_game(game_dir, game):
    """Prints the checks of one game; returns True (usable), False (not usable) or
    None (an optional game that is not installed)."""
    title, exe, expected, dlls, data_dir, optional = game
    print("%s:" % title)
    if optional and not find(game_dir, exe):
        print("  not installed (optional)")
        return None
    ok = True
    for name in [exe] + dlls:
        path = find(game_dir, name)
        if not path:
            print("  MISSING       %s" % name)
            ok = False
            continue
        info = pe_info(path)
        if not info:
            print("  BAD FILE      %s (not a PE file)" % name)
            ok = False
            continue
        timestamp, bits, size = info
        line = "  OK            %-16s %8d B, timestamp %08X" % (name, size, timestamp)
        if bits > ENCRYPTED_BITS:
            line = "  PROTECTED     %-16s code encrypted (%.2f bits/byte)" % (name, bits)
            ok = False
        elif name == exe and timestamp != expected:
            line = "  WRONG VERSION %-16s timestamp %08X, needed %08X (the Steam release)" % (name, timestamp, expected)
            ok = False
        print(line)
    folder = find(game_dir, data_dir)
    paks = sorted(p for p in os.listdir(folder) if p.lower().endswith(".pk3")) if folder and os.path.isdir(folder) else []
    if paks:
        print("  OK            %s/: %d .pk3 files" % (data_dir, len(paks)))
    else:
        print("  MISSING       %s/*.pk3" % data_dir)
        ok = False
    return ok


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    game_dir = sys.argv[1]
    if not os.path.isdir(game_dir):
        print("%s is not a folder" % game_dir)
        return 2
    results = []
    for game in GAMES:
        results.append((game[0], check_game(game_dir, game)))
        print()
    for title, ok in results:
        if ok is None:
            continue
        print("%s: %s" % (title, "ready to build." if ok else
                          "something does not match: the port needs the files of the Steam release."))
    return 0 if all(ok is not False for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
