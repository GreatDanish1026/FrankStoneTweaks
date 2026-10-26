"""What a function in the game's exe refers to: UTF-16 strings it loads (cvar names, mostly),
calls it makes and immediates passed in r8d (SetBy priorities), in order.

  python -I fnrefs.py <symbol regex | hex address> [<more>...]

The function ends at the next symbol in the map (mapsyms.py). Needs llvm-objdump on PATH.
"""
import bisect
import re
import struct
import subprocess
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder out
import mapsyms

LEA = re.compile(r'^\s*([0-9a-f]+):\s+(lea[a-z]*|call[a-z]*|mov[a-z]*)\s+(.*)$')


def wide_at(img, addr):
    rva = addr - mapsyms.BASE
    for va, size, ro in img.secs:
        if va <= rva < va + size:
            o = ro + rva - va
            s = []
            while len(s) < 120:
                c = struct.unpack_from('<H', img.d, o + 2 * len(s))[0]
                if c == 0:
                    break
                if not 32 <= c < 127:
                    return None
                s.append(chr(c))
            return ''.join(s) if len(s) >= 3 else None
    return None


def main():
    syms = mapsyms.load()
    addrs = [a for a, _ in syms]
    img = mapsyms.Image(mapsyms.EXE)
    for arg in sys.argv[1:]:
        if re.fullmatch(r'(0x)?[0-9a-fA-F]{9,}', arg):
            start = int(arg, 16)
            name = mapsyms_name(syms, addrs, start)
        else:
            rx = re.compile(arg)
            hit = next(((a, n) for a, n in syms if rx.search(n) and not n.startswith('$')), None)
            if not hit:
                print('no match:', arg)
                continue
            start, name = hit
        i = bisect.bisect_right(addrs, start)
        while i < len(addrs) and addrs[i] == start:
            i += 1
        end = addrs[i] if i < len(addrs) else start + 0x1000
        print('==', hex(start), name, '(%d bytes)' % (end - start))
        out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '--start-address=0x%x' % start,
                              '--stop-address=0x%x' % end, mapsyms.EXE], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = LEA.match(line)
            if not m:
                continue
            at, op, args = m.groups()
            t = re.search(r'# 0x([0-9a-f]+)', args)
            if op.startswith('lea') and t:
                s = wide_at(img, int(t.group(1), 16))
                if s:
                    print('  %s  L"%s"' % (at, s))
            elif op.startswith('call'):
                c = re.match(r'(?:callq\s+)?0x([0-9a-f]+)', args)
                if c:
                    print('  %s  call %s' % (at, mapsyms_name(syms, addrs, int(c.group(1), 16))[:110]))
                else:
                    print('  %s  call %s' % (at, args.split('#')[0].strip()))
            elif op.startswith('mov') and re.match(r'\$0x[0-9a-f]+000000, %r8d', args):
                print('  %s  r8d = %s' % (at, args.split(',')[0]))


def mapsyms_name(syms, addrs, a):
    i = bisect.bisect_right(addrs, a) - 1
    off = a - syms[i][0]
    return syms[i][1] + ('+0x%x' % off if off else '')


if __name__ == '__main__':
    main()
