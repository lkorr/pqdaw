"""Peak / RMS of a float or 16/24-bit WAV, overall and per window (stdlib only).

usage: python -I wavpeak.py file.wav [window_seconds]
"""
import math, struct, sys


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    pos, fmt, ch, sr, bits, frames = 12, 1, 2, 48000, 32, b""
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt, ch, sr = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if fmt == 0xFFFE:
                fmt = struct.unpack("<H", body[24:26])[0]
        elif cid == b"data":
            frames = body
        pos += 8 + size + (size & 1)
    if fmt == 3 and bits == 32:
        x = struct.unpack(f"<{len(frames) // 4}f", frames)
    elif bits == 16:
        x = [v / 32768.0 for v in struct.unpack(f"<{len(frames) // 2}h", frames)]
    else:
        raise SystemExit(f"unsupported wav format {fmt}/{bits}")
    return x, ch, sr


x, ch, sr = read(sys.argv[1])
win = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0


def stats(s):
    if not s:
        return "empty"
    pk = max(abs(v) for v in s)
    rms = math.sqrt(sum(v * v for v in s) / len(s))
    db = lambda v: 20 * math.log10(v) if v > 0 else -999.0
    return f"peak {db(pk):7.1f} dBFS  rms {db(rms):7.1f} dBFS"


print(f"{sys.argv[1]}: {len(x) // ch / sr:.2f} s, {ch} ch: {stats(x)}")
if win > 0:
    n = int(win * sr) * ch
    for i in range(0, len(x), n):
        print(f"  {i // ch / sr:7.2f} s: {stats(x[i:i + n])}")
