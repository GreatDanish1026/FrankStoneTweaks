"""Rough call stacks of every thread in a game minidump (UEMinidump.dmp), named from the map.

  python -I dumpstacks.py <UEMinidump.dmp> [--all]

No debugger needed: for each thread, its instruction pointer and every stack qword that points
into the game exe (most are return addresses, some are stale) are named with mapsyms.py. Only
threads with game frames are shown unless --all. Thread names come from the ThreadNames
stream when the dump has one.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script's folder out
import bisect

import mapsyms

THREAD_LIST, MODULE_LIST, THREAD_NAMES = 3, 4, 24


def main():
    data = open(sys.argv[1], 'rb').read()
    show_all = '--all' in sys.argv
    sig, ver, nstreams, dir_rva = struct.unpack_from('<4sIII', data, 0)
    assert sig == b'MDMP'
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from('<III', data, dir_rva + 12 * i)
        streams[t] = (size, rva)

    def mdstring(rva):
        n = struct.unpack_from('<I', data, rva)[0]
        return data[rva + 4:rva + 4 + n].decode('utf-16le')

    exe_base = exe_end = None
    _, rva = streams[MODULE_LIST]
    for i in range(struct.unpack_from('<I', data, rva)[0]):
        m = rva + 4 + 108 * i
        base, size = struct.unpack_from('<QI', data, m)
        name = mdstring(struct.unpack_from('<I', data, m + 20)[0])
        if name.lower().endswith('-shipping.exe'):
            exe_base, exe_end = base, base + size

    names = {}
    if THREAD_NAMES in streams:
        _, rva = streams[THREAD_NAMES]
        for i in range(struct.unpack_from('<I', data, rva)[0]):
            tid, name_rva = struct.unpack_from('<IQ', data, rva + 4 + 12 * i)
            names[tid] = mdstring(name_rva)

    syms = mapsyms.load()
    addrs = [a for a, _ in syms]

    def name(addr):
        a = addr - exe_base + mapsyms.BASE
        i = bisect.bisect_right(addrs, a) - 1
        return '%s+0x%x' % (syms[i][1][:110], a - syms[i][0])

    _, rva = streams[THREAD_LIST]
    for i in range(struct.unpack_from('<I', data, rva)[0]):
        t = rva + 4 + 48 * i
        tid, _, _, _, _, stack_start, stack_size, stack_rva, ctx_size, ctx_rva = struct.unpack_from('<IIIIQQIIII', data, t)
        rip = struct.unpack_from('<Q', data, ctx_rva + 0xf8)[0] if ctx_size >= 0x100 else 0
        stack = data[stack_rva:stack_rva + stack_size]
        frames = []
        for o in range(0, len(stack) - 7, 8):
            q = struct.unpack_from('<Q', stack, o)[0]
            if exe_base <= q < exe_end:
                frames.append(q)
        if not frames and not show_all and not exe_base <= rip < exe_end:
            continue
        print('\n== thread %d %s  rip %s' % (tid, names.get(tid, ''),
                                            name(rip) if exe_base <= rip < exe_end else hex(rip)))
        seen = 0
        for q in frames:
            print('   ', name(q))
            seen += 1
            if seen >= 40:
                print('    ...')
                break


if __name__ == '__main__':
    main()
