#!/usr/bin/env python3
"""Finds United Offensive's counterparts of the CoDSP.exe addresses the port uses.

Usage: python3 tools/find_uo_addresses.py "/path/to/the/game"

The game folder must hold both CoDSP.exe (1.3) and CoDUOSP.exe. For every address
the port knows in CoDSP.exe (the game state it reads, the functions it hooks) the
script takes the code around it, blanks out what differs between two builds of the
same source (absolute addresses, call and jump targets), looks for the same code in
CoDUOSP.exe and reads the address there. It prints only addresses, match counts and
SHA-1 sums of UO's code at the matches (what the recompiler's hook tables need): no
game code."""
import hashlib
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'recomp'))
from pe import PE  # noqa: E402

try:
    import capstone
    from capstone import x86 as X
except ImportError:
    print('capstone is missing: pip install capstone')
    sys.exit(2)

# Data the port reads or writes in CoDSP.exe: address -> what it is.
DATA = {
    0x01439264: 'cls.keyCatchers (pad: play or menus)',
    0x01439260: 'cls.state (frame statistics)',
    0x0056EE44: 'current cinematic handle (RoQ fix)',
    0x0086A5F4: 'cinematics array (RoQ fix)',
    0x014BD648: 'UI VM (render thread font cache)',
    0x01549698: 'backEndData (render thread)',
    0x01534504: 'max polys (render thread)',
    0x015344F8: 'max poly verts (render thread)',
}
# Code addresses outside the recompiler's tables: address -> what it is.
CODE = {
    0x004D4AC1: 'RE_EndFrame, after its call (render thread)',
}
WINDOW_INSNS = 10          # instructions per pattern


def text_section(pe):
    for s in pe.sections:
        if s.name in ('.text', 'text'):
            return s
    raise SystemExit('%s: no .text section' % pe.path)


class Code:
    def __init__(self, path):
        self.pe = PE(path)
        self.text = text_section(self.pe)
        self.base = self.text.va
        self.data = self.pe.data[self.text.raw_off:self.text.raw_off + self.text.raw_size]
        self.lo = self.pe.image_base
        self.hi = self.pe.image_base + self.pe.size_of_image
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    def insns(self, addr, count):
        off = addr - self.base
        if not 0 <= off < len(self.data):
            return []
        out = []
        for ins in self.md.disasm(self.data[off:off + 16 * count], addr):
            out.append(ins)
            if len(out) == count:
                break
        return out

    def masked(self, addr, count, mark=None):
        """Bytes of `count` instructions from addr with absolute addresses and branch
        targets blanked (None). `mark`: the address of a dword to report the
        position of (it is blanked too). Returns (pattern, position of mark)."""
        pattern, mark_pos = [], None
        for ins in self.insns(addr, count):
            b = [int(x) for x in ins.bytes]
            blank = set()
            if ins.disp_size == 4:
                disp = ins.disp & 0xFFFFFFFF
                if self.lo <= disp < self.hi:
                    blank.update(range(ins.disp_offset, ins.disp_offset + 4))
            if ins.imm_size == 4 and ins.imm_offset:
                imm_val = int.from_bytes(bytes(b[ins.imm_offset:ins.imm_offset + 4]), 'little')
                is_branch = ins.group(X.X86_GRP_JUMP) or ins.group(X.X86_GRP_CALL)
                if is_branch or self.lo <= imm_val < self.hi:
                    blank.update(range(ins.imm_offset, ins.imm_offset + 4))
            if mark is not None and ins.address <= mark < ins.address + ins.size:
                mark_pos = len(pattern) + (mark - ins.address)
                blank.update(range(mark - ins.address, mark - ins.address + 4))
            pattern.extend(None if i in blank else v for i, v in enumerate(b))
        return pattern, mark_pos

    def find(self, pattern):
        rx = re.compile(b''.join(b'.' if v is None else re.escape(bytes([v])) for v in pattern), re.DOTALL)
        return [self.base + m.start() for m in rx.finditer(self.data)]

    def dword(self, addr):
        off = addr - self.base
        return int.from_bytes(self.data[off:off + 4], 'little')

    def sha1(self, addr, size):
        off = addr - self.base
        return hashlib.sha1(self.data[off:off + size]).hexdigest()

    def refs(self, value):
        """Instruction starts in .text whose displacement or immediate is `value`."""
        needle = value.to_bytes(4, 'little')
        out = []
        start = 0
        while True:
            i = self.data.find(needle, start)
            if i < 0:
                break
            start = i + 1
            for back in range(1, 8):
                a = self.base + i - back
                ins = self.insns(a, 1)
                if not ins or ins[0].size < back + 4:
                    continue
                ins = ins[0]
                if (ins.disp_size == 4 and ins.disp_offset == back) or \
                        (ins.imm_size == 4 and ins.imm_offset == back):
                    out.append((a, self.base + i))
                    break
        return out


def match_data(cod, uo, value):
    votes = Counter()
    sites = cod.refs(value)
    for start, at in sites:
        pattern, pos = cod.masked(start, WINDOW_INSNS, mark=at)
        if pos is None:
            continue
        hits = uo.find(pattern)
        if len(hits) == 1:
            votes[uo.dword(hits[0] + pos)] += 1
    return votes, len(sites)


def match_code(cod, uo, addr):
    pattern, _ = cod.masked(addr, WINDOW_INSNS)
    if not pattern:
        return []
    return uo.find(pattern)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    game_dir = sys.argv[1]

    def path(name):
        for e in os.listdir(game_dir):
            if e.lower() == name.lower():
                return os.path.join(game_dir, e)
        raise SystemExit('%s not found in %s' % (name, game_dir))

    cod = Code(path('CoDSP.exe'))
    uo = Code(path('CoDUOSP.exe'))

    print('Data:')
    for value, what in DATA.items():
        votes, n = match_data(cod, uo, value)
        best = ', '.join('%08X x%d' % kv for kv in votes.most_common(3)) or 'not found'
        print('  %08X -> %s   (%d references in CoD; %s)' % (value, best, n, what))

    print()
    print('Code:')
    import gen  # noqa: E402  (the recompiler's hook tables)
    tables = [('PATCHES', a, gen.PATCHES[a][1], gen.PATCHES[a][0]) for a in sorted(gen.PATCHES)]
    tables += [('RENDER_WAIT', a, 16, '') for a in sorted(gen.RENDER_WAIT)]
    tables += [('CODE', a, 16, what) for a, what in CODE.items()]
    for table, addr, size, what in tables:
        hits = match_code(cod, uo, addr)
        if len(hits) == 1:
            print('  %-11s %08X -> %08X  sha1(%d)=%s  %s' % (table, addr, hits[0], size, uo.sha1(hits[0], size), what))
        else:
            print('  %-11s %08X -> %s  %s' % (table, addr, 'not found' if not hits else
                                            'ambiguous (%d matches)' % len(hits), what))
    return 0


if __name__ == '__main__':
    sys.exit(main())
