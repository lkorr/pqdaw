"""M4 review (document): compares renders of gen_projects.py projects (WAV, float or 16/24-bit PCM, stdlib only).

Usage: python analyse.py <render dir>   (expects <name>.wav per project)
Prints per-channel RMS (dBFS) and the max abs difference / level ratio of the pairs that must agree.
"""
import math
import os
import struct
import sys
import wave


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    # minimal RIFF parser (WAVE_FORMAT_IEEE_FLOAT = 3 or PCM = 1, possibly in an EXTENSIBLE header)
    pos, fmt, chans, bits, frames = 12, None, 0, 0, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt, chans = struct.unpack("<HH", body[:4])
            bits = struct.unpack("<H", body[14:16])[0]
            if fmt == 0xFFFE:
                fmt = struct.unpack("<H", body[24:26])[0]
        elif cid == b"data":
            frames = body
        pos += 8 + size + (size & 1)
    n = len(frames) // (bits // 8)
    if fmt == 3 and bits == 32:
        s = struct.unpack("<%df" % n, frames[:n * 4])
    elif fmt == 1 and bits == 16:
        s = [x / 32768.0 for x in struct.unpack("<%dh" % n, frames[:n * 2])]
    elif fmt == 1 and bits == 24:
        s = [int.from_bytes(frames[i:i + 3], "little", signed=True) / 8388608.0 for i in range(0, n * 3, 3)]
    else:
        raise SystemExit("unsupported format %r/%r" % (fmt, bits))
    return [s[c::chans] for c in range(chans)]


def rms_db(x):
    m = sum(v * v for v in x) / max(1, len(x))
    return 10 * math.log10(m) if m > 0 else -999.0


def main():
    d = sys.argv[1]
    w = {}
    for f in sorted(os.listdir(d)):
        if f.endswith(".wav"):
            w[f[:-4]] = read(os.path.join(d, f))
            print("%-16s " % f[:-4] + "  ".join("ch%d %8.3f dB" % (c, rms_db(ch)) for c, ch in enumerate(w[f[:-4]])))
    pairs = [("map_half", "direct_m17p5"), ("map_quarter", "direct_m43p75"), ("map_inverted", "direct_p8p75"),
             ("rackid_rack", "rackid_bare"), ("rackid_muted2", "rackid_bare")]
    for a, b in pairs:
        if a not in w or b not in w:
            continue
        diff = max(abs(x - y) for ca, cb in zip(w[a], w[b]) for x, y in zip(ca, cb))
        print("%-14s vs %-14s  level diff %+.4f dB, max |diff| %.3g" % (a, b, rms_db(w[a][0]) - rms_db(w[b][0]), diff))
    if "map_curve" in w and "direct_m17p5" in w:
        print("map_curve level vs direct -17.5 dB: %+.3f dB" % (rms_db(w["map_curve"][0]) - rms_db(w["direct_m17p5"][0])))


if __name__ == "__main__":
    main()
