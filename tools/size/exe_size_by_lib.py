"""Attribute an MSVC executable's code and data to the libraries it came from, from a linker map (link /MAP).

T1b.6 (LLVM size check). Usage:
    python tools/size/exe_size_by_lib.py pqdaw.map [--top N]

How to get the map without touching the build: take the link command of the target from
`ninja -t compdb -x` (in the build directory), replace /out: with a scratch path, add /MAP:<file>, run it in the
Visual Studio environment (scripts/vsenv.ps1). Each public and static symbol's size is the distance to the next
symbol in its section (the last one runs to the section end), so padding between functions counts to the symbol
before it. .pdata (unwind tables, no symbols) and .bss (not in the file) are reported separately.
"""
import argparse
import collections
import re
import sys

SECTION_RE = re.compile(r'^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})H\s+(\S+)\s+(CODE|DATA)')
SYMBOL_RE = re.compile(r'^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$')


def group_of(lib):
    name = lib.split(':')[0]
    low = name.lower()
    if name.startswith('LLVM'):
        return 'LLVM'
    if low == 'pq_harfbuzz':
        return 'harfbuzz'
    if low.startswith('pq_') or low.endswith('.cpp.obj') or low.endswith('.c.obj'):
        return 'pqdaw'
    if low.startswith(('libcmt', 'libucrt', 'libvcruntime', 'libcpmt', 'msvcrt', 'libconcrt', 'oldnames')):
        return 'CRT/STL'
    if low.startswith(('svml', 'libmmt', 'libirc', 'libdecimal')):
        return 'SVML/math'
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('map')
    ap.add_argument('--top', type=int, default=25)
    args = ap.parse_args()
    sections = {}  # section number -> (end offset, contains .bss start or None)
    bss = {}
    symbols = collections.defaultdict(list)  # section -> [(offset, lib)]
    with open(args.map, encoding='latin-1') as f:
        for line in f:
            m = SECTION_RE.match(line)
            if m and not symbols:
                sec, start, length, name = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
                end = start + length
                sections[sec] = max(sections.get(sec, 0), end)
                if name == '.bss':
                    bss[sec] = start
                continue
            m = SYMBOL_RE.match(line)
            if m:
                sec = int(m.group(1), 16)
                if sec == 0 or sec not in sections:
                    continue
                symbols[sec].append((int(m.group(2), 16), m.group(5)))
    by_lib = collections.Counter()
    by_group = collections.Counter()
    by_group_code = collections.Counter()
    for sec, syms in symbols.items():
        syms.sort()
        end = sections[sec]
        limit = bss.get(sec, end)  # .bss is not in the file
        for i, (off, lib) in enumerate(syms):
            if off >= limit:
                continue
            nxt = syms[i + 1][0] if i + 1 < len(syms) else limit
            size = max(0, min(nxt, limit) - off)
            by_lib[lib.split(':')[0]] += size
            by_group[group_of(lib)] += size
            if sec == 1:
                by_group_code[group_of(lib)] += size
    total = sum(by_group.values())
    print(f'attributed (code + read-only + initialised data): {total / 2**20:.2f} MiB')
    for sec, end in sorted(sections.items()):
        print(f'  section {sec}: {end / 2**20:.2f} MiB' + (f' (.bss from {bss[sec] / 2**20:.2f} MiB, not in the file)' if sec in bss else ''))
    print('by group (all sections / .text):')
    for g, n in by_group.most_common():
        print(f'  {g:24s} {n / 2**20:8.2f} MiB  {100.0 * n / total:5.1f} %   code {by_group_code[g] / 2**20:7.2f} MiB')
    print(f'top {args.top} libraries:')
    for lib, n in by_lib.most_common(args.top):
        print(f'  {lib:40s} {n / 2**20:8.2f} MiB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
