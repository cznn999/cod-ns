#!/usr/bin/env python3
"""Checks a Call of Duty (2003) installation before building the port.

Usage: python3 tools/check_game_files.py /path/to/the/game

The recompiler needs the single-player executable of version 1.3 without copy
protection (timestamp 0x40443FDA, the one of the Steam release)
and the three game DLLs; the data (main/*.pk3) is read at run time. This script
only reads the files and reports what it finds."""
import os
import struct
import sys

EXPECTED_TIMESTAMP = 0x40443FDA
REQUIRED = ["CoDSP.exe", "gamex86.dll", "cgamex86.dll", "uix86.dll"]
# Section names left in executables wrapped by SafeDisc (their code is encrypted on disk).
SAFEDISC_SECTIONS = {".stxt774", ".stxt371"}


def pe_info(path):
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
    names = []
    for i in range(nsec):
        raw = data[sec + 40 * i:sec + 40 * i + 8]
        names.append(raw.rstrip(b"\0").decode("latin-1"))
    return timestamp, names, len(data)


def find(game_dir, name):
    """Case-insensitive lookup (the SD card does not care about case; Linux does)."""
    for entry in os.listdir(game_dir):
        if entry.lower() == name.lower():
            return os.path.join(game_dir, entry)
    return None


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    game_dir = sys.argv[1]
    ok = True
    for name in REQUIRED:
        path = find(game_dir, name)
        if not path:
            print("MISSING   %s" % name)
            ok = False
            continue
        info = pe_info(path)
        if not info:
            print("BAD FILE  %s (not a PE file)" % name)
            ok = False
            continue
        timestamp, sections, size = info
        line = "OK        %-13s %8d B, timestamp %08X" % (name, size, timestamp)
        if name == "CoDSP.exe":
            protected = SAFEDISC_SECTIONS & set(sections)
            if protected:
                line = "PROTECTED %-13s SafeDisc sections: %s" % (name, ", ".join(sorted(protected)))
                ok = False
            elif timestamp != EXPECTED_TIMESTAMP:
                line = "WRONG VERSION %s: timestamp %08X, needed %08X (1.3 of the Steam release)" % (
                    name, timestamp, EXPECTED_TIMESTAMP)
                ok = False
        print(line)
    main_dir = find(game_dir, "main")
    paks = sorted(p for p in os.listdir(main_dir) if p.lower().endswith(".pk3")) if main_dir else []
    if paks:
        print("OK        main/: %d .pk3 files (%s ... %s)" % (len(paks), paks[0], paks[-1]))
    else:
        print("MISSING   main/*.pk3")
        ok = False
    print()
    print("Everything matches: ready to build." if ok else
          "Something does not match: the port needs the unprotected CoDSP.exe 1.3 of the Steam release.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
