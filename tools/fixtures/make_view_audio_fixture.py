"""T5.6: the `audio` views fixture (tests/views/fixtures/audio): a project with audio tracks and the two small WAV files
its clips play, for the `arrangement-audio` and `clip-view-audio` goldens and the arrangement audio tests.

    python tools/fixtures/make_view_audio_fixture.py

Writes tests/views/fixtures/audio/project.pqd (schema 7) and samples/beat.wav (stereo 16-bit 44.1 kHz, 2 s: four
kicks and hats) and samples/vox.wav (mono 16-bit 48 kHz, 1.5 s: three vowel-like tones). A fourth sample entry points at
samples/missing.wav, which is not written (the "missing" overlay). Deterministic; standard library only.

Clips (120 BPM, a 4/4 bar = 2 s = 887040 ticks):
  Drums  beat   bar 1        whole file, fade-in 1/32 note, fade-out 1/8 (curve -0.5)
         beat   bar 2        adjacent, crossfade 1/8, gain -6 dB
         beat   bar 3.5      1 s from the start (at +7 st: 1.5 s of the file), reversed
  Vox    vox    bar 1.5      Re-Pitch at 100 BPM, loop over its first 2 beats, +6 dB
  Keys   (MIDI) bar 1        a note clip (mixed MIDI + audio rows)
  Lost   missing bar 2       a sample that is not on disk
"""

import json
import math
import os
import struct
import wave

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "views", "fixtures", "audio")

WHOLE = 887040          # ticks per 4/4 bar (kTicksPerWhole)
PPQ = WHOLE // 4        # ticks per quarter
SEC = 2 * PPQ           # ticks per second at 120 BPM


def lcg(seed):
    x = seed
    while True:
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        yield (x >> 8) / 8388608.0 - 1.0


def write_wav(path, channels, rate, frames):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(rate)
        data = bytearray()
        for i in range(frames):
            for ch in channels:
                v = max(-1.0, min(1.0, ch[i]))
                data += struct.pack("<h", int(round(v * 32767)))
        w.writeframes(bytes(data))


def beat():
    rate, n = 44100, 88200
    noise = lcg(7)
    left, right = [0.0] * n, [0.0] * n
    for i in range(n):
        t = i / rate
        beat_t = t % 0.5
        kick = math.exp(-beat_t * 14) * math.sin(2 * math.pi * (50 + 90 * math.exp(-beat_t * 30)) * beat_t) * 0.85
        hat_t = (t + 0.25) % 0.5
        hat = math.exp(-hat_t * 60) * next(noise) * 0.35
        left[i] = kick + hat
        right[i] = kick * 0.9 - hat * 0.8
    return rate, [left, right], n


def vox():
    rate, n = 48000, 72000
    out = [0.0] * n
    phase = 0.0
    for i in range(n):
        t = i / rate
        syl = t % 0.5
        env = math.sin(math.pi * min(1.0, syl / 0.45)) ** 2 if syl < 0.45 else 0.0
        f = [220.0, 247.0, 196.0][int(t / 0.5) % 3] * (1 + 0.01 * math.sin(2 * math.pi * 5.5 * t))
        phase += 2 * math.pi * f / rate
        out[i] = env * 0.6 * (math.sin(phase) + 0.5 * math.sin(2 * phase) + 0.25 * math.sin(3 * phase))
    return rate, [out], n


def content_hash(path):
    """doc::fileIdentity / media::contentHash: FNV-1a 64 over the size (8 bytes LE) and three 64 KiB windows."""
    data = open(path, "rb").read()
    size = len(data)
    W = 64 * 1024
    h = 1469598103934665603
    def fnv(h, b):
        for c in b:
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        return h
    h = fnv(h, struct.pack("<Q", size))
    if size <= W:
        h = fnv(h, data)
    else:
        for off in (0, (size - W) // 2, size - W):
            h = fnv(h, data[off:off + W])
    return "%016x" % h, size


MIXER = {"volumeDb": 0.0, "pan": 0.0, "mute": False, "solo": False, "arm": False, "sends": [], "output": "default"}


def audio_clip(cid, name, colour, start, sample, **kw):
    c = {"kind": "audio", "id": cid, "name": name, "colour": colour, "start": start, "length": kw.get("length", SEC),
         "sample": sample, "warp": kw.get("warp", "off"), "contentStartFrame": kw.get("csf", 0.0),
         "seconds": kw.get("seconds", 1.0), "contentStart": kw.get("contentStart", 0),
         "loop": kw.get("loop", {"enabled": False, "start": 0, "end": WHOLE}), "sourceBpm": kw.get("bpm", 120.0),
         "gainDb": kw.get("gain", 0.0), "transpose": kw.get("transpose", {"cents": 0.0}), "detuneCents": 0.0,
         "reverse": kw.get("reverse", False), "fadeIn": kw.get("fadeIn", {"length": 0, "curve": 0.0}),
         "fadeOut": kw.get("fadeOut", {"length": 0, "curve": 0.0}), "crossfadeIn": kw.get("xfade", 0),
         "tuning": "inherit", "active": True}
    return c


def track(tid, kind, name, colour, clips, devices=None):
    return {"id": tid, "kind": kind, "name": name, "colour": colour, "active": True, "tuning": "inherit",
            "mixer": dict(MIXER), "lanes": [], "devices": devices or [], "clips": clips}


def main():
    rate_b, ch_b, n_b = beat()
    rate_v, ch_v, n_v = vox()
    write_wav(os.path.join(ROOT, "samples", "beat.wav"), ch_b, rate_b, n_b)
    write_wav(os.path.join(ROOT, "samples", "vox.wav"), ch_v, rate_v, n_v)
    hb, sb = content_hash(os.path.join(ROOT, "samples", "beat.wav"))
    hv, sv = content_hash(os.path.join(ROOT, "samples", "vox.wav"))
    orange, blue, green, grey = "#E8A33D", "#5E9BD6", "#7BC47F", "#B0B0B0"
    drums = track(10, "audio", "Drums", orange, [
        audio_clip(20, "beat", orange, 0, 3, seconds=2.0, length=2 * SEC,
                   fadeIn={"length": WHOLE // 32, "curve": 0.0}, fadeOut={"length": WHOLE // 8, "curve": -0.5}),
        audio_clip(21, "beat", orange, WHOLE, 3, seconds=2.0, length=2 * SEC, gain=-6.0, xfade=WHOLE // 8),
        audio_clip(22, "beat rev", orange, WHOLE * 5 // 2, 3, seconds=1.0, length=SEC, csf=0.0, reverse=True,
                   transpose={"cents": 700.0}),
    ])
    # Re-Pitch at 100 BPM: content tick c plays frame c * 48000 * 60 / (100 * PPQ); the loop is the first 2 beats.
    vox_t = track(11, "audio", "Vox", blue, [
        audio_clip(23, "vox", blue, WHOLE // 2, 4, warp="rePitch", bpm=100.0, length=2 * WHOLE, gain=6.0,
                   loop={"enabled": True, "start": 0, "end": 2 * PPQ}),
    ])
    notes = [{"id": 40 + i, "start": i * PPQ // 2, "length": PPQ // 2, "pitch": {"degree": d, "equave": 0}, "velocity": 100.0}
             for i, d in enumerate([0, 4, 7, 4, 2, 5, 9, 5])]
    keys = track(12, "midi", "Keys", green, [
        {"id": 30, "name": "chords", "colour": green, "start": 0, "length": WHOLE, "contentStart": 0,
         "loop": {"enabled": False, "start": 0, "end": WHOLE}, "signature": {"numerator": 4, "denominator": 4},
         "tuning": "inherit", "active": True, "notes": notes}], [{"id": 13, "patch": "BasicSynth", "kind": "instrument"}])
    lost = track(14, "audio", "Lost", grey, [audio_clip(24, "missing", grey, WHOLE, 5, seconds=1.0, length=SEC)])
    project = {
        "schemaVersion": 7,
        "timeline": {"tempo": [{"tick": 0, "bpm": 120.0, "curve": "step"}],
                     "signatures": [{"bar": 0, "numerator": 4, "denominator": 4}]},
        "loop": {"start": 0, "end": 4 * WHOLE, "enabled": False},
        "locators": [],
        "tunings": [{"id": 1, "name": "12-EDO", "colour": "#5E9BD6",
                     "def": {"kind": "edo", "n": 12, "equave": "2/1", "referenceHz": 261.626}}],
        "tracks": [drums, vox_t, keys, lost],
        "returns": [],
        "master": {"id": 2, "kind": "master", "name": "Master", "colour": "#808080", "active": True, "tuning": "inherit",
                   "mixer": dict(MIXER), "lanes": [], "devices": []},
        "samples": [
            {"id": 3, "path": "samples/beat.wav", "name": "beat", "hash": hb, "size": sb, "frames": n_b, "rate": float(rate_b), "channels": 2},
            {"id": 4, "path": "samples/vox.wav", "name": "vox", "hash": hv, "size": sv, "frames": n_v, "rate": float(rate_v), "channels": 1},
            {"id": 5, "path": "samples/missing.wav", "name": "missing", "hash": "0000000000000001", "size": 1000, "frames": 48000, "rate": 48000.0, "channels": 1},
        ],
        "view": {"tracks": [{"id": 10, "folded": False, "height": 72}, {"id": 11, "folded": False, "height": 60}], "lanes": []},
    }
    with open(os.path.join(ROOT, "project.pqd"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(project, f, indent=2, ensure_ascii=False)
        f.write("\n")


if __name__ == "__main__":
    main()
