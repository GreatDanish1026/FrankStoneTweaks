"""Symbol lookups in the linker map Frank Stone ships (CastingFrankStone-Win64-Shipping.map).

  python -I mapsyms.py "<regex>"                symbols matching, with their addresses
  python -I mapsyms.py "<regex>" <n> [<slots>]  also list the vtable slots of the first n matches
  python -I mapsyms.py @<hex address>           the symbol at or just before an address

FS_BIN points at the game's Win64 folder (exe + map). A pickle cache is written next to this
script. The linker folds identical functions, so a slot can show an unrelated name: disassemble
to be sure (llvm-objdump -d --start-address=0x... --stop-address=0x... <exe>).
"""
import bisect
import os
import pickle
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get('FS_BIN', r'E:\SteamLibrary\steamapps\common\The Casting of Frank Stone\SMG037UE5\Binaries\Win64')
EXE = os.path.join(BIN, 'CastingFrankStone-Win64-Shipping.exe')
MAP = os.path.join(BIN, 'CastingFrankStone-Win64-Shipping.map')
CACHE = os.path.join(HERE, 'mapsyms.pickle')
BASE = 0x140000000
LINE = re.compile(r'^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', re.I)


def load():
    stamp = os.path.getmtime(MAP)
    if os.path.exists(CACHE):
        with open(CACHE, 'rb') as f:
            c = pickle.load(f)
        if c['stamp'] == stamp:
            return c['syms']
    syms = []
    with open(MAP, encoding='latin-1') as f:
        for line in f:
            m = LINE.match(line)
            if m and int(m.group(2), 16) >= BASE:
                syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    with open(CACHE, 'wb') as f:
        pickle.dump({'stamp': stamp, 'syms': syms}, f)
    return syms


class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', self.d, 0x3c)[0]
        n = struct.unpack_from('<H', self.d, pe + 6)[0]
        opt = struct.unpack_from('<H', self.d, pe + 20)[0]
        self.secs = []
        for i in range(n):
            _, vs, va, rs, ro = struct.unpack_from('<8sIIII', self.d, pe + 24 + opt + 40 * i)
            self.secs.append((va, max(vs, rs), ro))

    def q(self, addr):
        rva = addr - BASE
        for va, size, ro in self.secs:
            if va <= rva < va + size:
                return struct.unpack_from('<Q', self.d, ro + rva - va)[0]
        return None


def main():
    syms = load()
    addrs = [a for a, _ in syms]
    names = {}
    for a, n in syms:
        names.setdefault(a, []).append(n)
    if sys.argv[1].startswith('@'):
        a = int(sys.argv[1][1:], 16)
        i = bisect.bisect_right(addrs, a) - 1
        print(hex(syms[i][0]), '+', hex(a - syms[i][0]), syms[i][1])
        return
    rx = re.compile(sys.argv[1])
    hits = [(a, n) for a, n in syms if rx.search(n)]
    for a, n in hits[:200]:
        print(hex(a), n)
    if len(sys.argv) > 2:
        img = Image(EXE)
        slots = int(sys.argv[3]) if len(sys.argv) > 3 else 40
        for a, n in hits[:int(sys.argv[2])]:
            print('\nvtable', n)
            for s in range(slots):
                fn = img.q(a + 8 * s)
                if fn is None:
                    break
                print('  +0x%03x %x %s' % (8 * s, fn, ' | '.join(names.get(fn, ['?'])[:2])))


if __name__ == '__main__':
    main()
