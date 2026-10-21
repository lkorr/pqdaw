"""Expected unkerned advance widths for the text-shaping test (tests/ui/test_text.cpp).

Reads cmap / hmtx / hhea / head straight from the TrueType file (no FreeType, no HarfBuzz), so the numbers are
an independent check on what the shaper reports. Prints, for each character of each string, the glyph's
advance in font units and scaled to 26.6 fixed point at the given pixel size: round(units * px * 64 / upem).

    python tools/fixtures/inter_advances.py assets/fonts/Inter-Regular.ttf 13 "Hamburgefonstiv"
"""
import struct
import sys


def tables(d):
    n = struct.unpack('>H', d[4:6])[0]
    out = {}
    for i in range(n):
        tag, _, off, ln = struct.unpack('>4sIII', d[12 + 16 * i:28 + 16 * i])
        out[tag.decode('latin-1')] = (off, ln)
    return out


def cmap(d, t):
    off = t['cmap'][0]
    nsub = struct.unpack('>H', d[off + 2:off + 4])[0]
    m = {}
    for i in range(nsub):
        _, _, so = struct.unpack('>HHI', d[off + 4 + 8 * i:off + 12 + 8 * i])
        st = off + so
        if struct.unpack('>H', d[st:st + 2])[0] == 12:
            ng = struct.unpack('>I', d[st + 12:st + 16])[0]
            for g in range(ng):
                a, b, gid = struct.unpack('>III', d[st + 16 + 12 * g:st + 28 + 12 * g])
                for c in range(a, b + 1):
                    m[c] = gid + (c - a)
    return m


def main():
    path, px = sys.argv[1], int(sys.argv[2])
    d = open(path, 'rb').read()
    t = tables(d)
    upem = struct.unpack('>H', d[t['head'][0] + 18:t['head'][0] + 20])[0]
    nh = struct.unpack('>H', d[t['hhea'][0] + 34:t['hhea'][0] + 36])[0]
    hm = t['hmtx'][0]
    cm = cmap(d, t)

    def adv(gid):
        i = min(gid, nh - 1)
        return struct.unpack('>H', d[hm + 4 * i:hm + 4 * i + 2])[0]

    print('upem', upem)
    for s in sys.argv[3:]:
        units = [adv(cm[ord(c)]) for c in s]
        scaled = [int(u * px * 64 / upem + 0.5) for u in units]
        print(s)
        print('  gids  ', [cm[ord(c)] for c in s])
        print('  units ', units)
        print('  26.6  ', scaled, 'sum', sum(scaled), 'px', (sum(scaled) + 32) >> 6)


if __name__ == '__main__':
    main()
