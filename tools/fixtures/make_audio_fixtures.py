"""Generates the audio decoding fixtures in tests/media/fixtures/ (T5.1).

    python -I tools/fixtures/make_audio_fixtures.py

Encoder: the `soundfile` Python package (its wheel bundles libsndfile 1.2.x, which encodes FLAC with libFLAC, MP3
with LAME and OGG Vorbis with libvorbis). The machine had no ffmpeg / flac / lame / oggenc on PATH (checked
2026-10-31), so the fixtures were generated in an isolated venv:

    python -I -m venv <dir>  &&  <dir>/Scripts/python -I -m pip install soundfile numpy
    <dir>/Scripts/python -I tools/fixtures/make_audio_fixtures.py

The exact versions used are recorded in tests/media/fixtures/manifest.json with each file's SHA-256.

One known signal (stereo, 48 kHz), computed in float64:
  L = 0.5 * sin(sweep phase, 40 Hz -> 12 kHz, logarithmic over the whole signal) + pulses at 1000 + 1500 k
  R = 0.5 * sin(sweep phase, 12 kHz -> 40 Hz)                                    + pulses at 1750 + 1500 k
  sweep phase(t) = 2 pi f0 T / ln(f1/f0) * (exp(t/T * ln(f1/f0)) - 1),  t = n / 48000, T = N / 48000
  pulse(m) = 0.3 * sinc(m/3) * 0.5 * (1 + cos(pi m / 16)) for |m| <= 15 (a Hann-windowed 8 kHz band-limited impulse:
  lossy encoders low-pass near 16 kHz, so a one-sample impulse would cap any SNR near 27 dB)
tests/media/test_decode.cpp computes the same formula (knownSignal) for the lossy comparisons.

Lossless fixtures (N = 6000 frames) store integer PCM quantised here; the reference values are written as raw
little-endian files next to them, so the C++ test compares bit for bit without recomputing the rounding:
  ref_pcm16.raw  int16, round(x * 2^15) clipped      (WAV 16, RF64 16, AIFF 16, FLAC 16)
  ref_pcm24.raw  int32 holding round(x * 2^23)        (WAV 24, WAV extensible 24, AIFF 24, FLAC 24)
  ref_pcm32.raw  int32, round(x * 2^31) clipped       (WAV 32-bit int)
  ref_f32.raw    float32(x)                           (WAV 32-bit float)
Lossy fixtures (N = 48000 frames, 1 s): MP3 CBR 320 kbit/s, MP3 VBR (LAME V0), OGG Vorbis (quality 10, the
highest). libvorbis's output stays near 39.5 dB SNR on this signal at every quality (even a pure 1 kHz sine only
reaches 37 dB: its noise normalisation), so the Vorbis fixture also has a reference decode by libvorbis itself:
  ogg_vorbis_libvorbis_f32.raw  float32, the first 6000 frames as libsndfile (libvorbis) decodes them
which the test compares stb_vorbis against (two decoders of one stream agree to float rounding).
meta_smpl_acid.wav: the 16-bit signal plus a `smpl` chunk (root key 62 + 50 cents, one loop 1000..4999) and an
`acid` chunk (root note 62, 4 beats, 120.5 BPM), for the probe's metadata fields.
"""

import hashlib
import json
import math
import os
import struct
import sys

import numpy as np
import soundfile as sf

RATE = 48000
N_LOSSLESS = 6000
N_LOSSY = 48000

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'tests', 'media', 'fixtures'))


def sweep_phase(n, total, f0, f1):
    t = n / RATE
    T = total / RATE
    k = math.log(f1 / f0)
    return 2.0 * math.pi * f0 * T / k * (np.exp(t / T * k) - 1.0)


def pulse(m):
    m = np.asarray(m, dtype=np.float64)
    x = np.pi * m / 3.0
    s = np.where(m == 0, 1.0, np.sin(x) / np.where(m == 0, 1.0, x))
    return 0.3 * s * 0.5 * (1.0 + np.cos(np.pi * m / 16.0))


def known_signal(total):
    n = np.arange(total, dtype=np.float64)
    left = 0.5 * np.sin(sweep_phase(n, total, 40.0, 12000.0))
    right = 0.5 * np.sin(sweep_phase(n, total, 12000.0, 40.0))
    taps = np.arange(-15, 16)
    for start, ch in ((1000, left), (1750, right)):
        for p in range(start, total, 1500):
            idx = p + taps
            ok = (idx >= 0) & (idx < total)
            ch[idx[ok]] += pulse(taps[ok])
    return np.stack([left, right], axis=1)  # frames x 2


def quantise(x, bits):
    scale = float(1 << (bits - 1))
    q = np.round(x * scale)
    return np.clip(q, -scale, scale - 1).astype(np.int64)


def chunk(tag, payload):
    pad = b'\0' if len(payload) & 1 else b''
    return tag + struct.pack('<I', len(payload)) + payload + pad


def write_smpl_acid_wav(path, pcm16):
    """A 16-bit WAV with smpl + acid chunks, written by hand (libsndfile has no acid writer)."""
    frames = pcm16.shape[0]
    fmt = struct.pack('<HHIIHH', 1, 2, RATE, RATE * 4, 4, 16)
    # smpl: manufacturer, product, sample period (ns), MIDI unity note, pitch fraction (fraction of a semitone
    # as an unsigned 32-bit fraction), SMPTE format, SMPTE offset, loop count, sampler data; then the loops:
    # cue id, type (0 = forward), start, end (inclusive), fraction, play count (0 = infinite).
    period = int(round(1e9 / RATE))
    smpl = struct.pack('<9I', 0, 0, period, 62, 0x80000000, 0, 0, 1, 0)
    smpl += struct.pack('<6I', 0, 0, 1000, 4999, 0, 0)
    # acid: flags (0x02 = root note set, 0x01 one-shot off), root note, unknown 16, unknown float, beats,
    # meter denominator, meter numerator, tempo (float BPM).
    acid = struct.pack('<IHHfIHHf', 0x02, 62, 0x8000, 0.0, 4, 4, 4, 120.5)
    data = pcm16.astype('<i2').tobytes()
    body = b'WAVE' + chunk(b'fmt ', fmt) + chunk(b'smpl', smpl) + chunk(b'acid', acid) + chunk(b'data', data)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)
    assert frames == N_LOSSLESS


def main():
    os.makedirs(OUT, exist_ok=True)
    x = known_signal(N_LOSSLESS)
    q16 = quantise(x, 16)
    q24 = quantise(x, 24)
    q32 = quantise(x, 32)
    f32 = x.astype(np.float32)

    q16.astype('<i2').tofile(os.path.join(OUT, 'ref_pcm16.raw'))
    q24.astype('<i4').tofile(os.path.join(OUT, 'ref_pcm24.raw'))
    q32.astype('<i4').tofile(os.path.join(OUT, 'ref_pcm32.raw'))
    f32.astype('<f4').tofile(os.path.join(OUT, 'ref_f32.raw'))

    i16 = q16.astype(np.int16)
    i24 = (q24 << 8).astype(np.int32)  # libsndfile's int path keeps the top 24 bits for PCM_24
    i32 = q32.astype(np.int32)

    def w(name, data, fmt, subtype, **kw):
        sf.write(os.path.join(OUT, name), data, RATE, format=fmt, subtype=subtype, **kw)

    w('wav_pcm16.wav', i16, 'WAV', 'PCM_16')
    w('wav_pcm24.wav', i24, 'WAV', 'PCM_24')
    w('wav_pcm32.wav', i32, 'WAV', 'PCM_32')
    w('wav_f32.wav', f32, 'WAV', 'FLOAT')
    w('wavex_pcm24.wav', i24, 'WAVEX', 'PCM_24')
    w('rf64_pcm16.rf64', i16, 'RF64', 'PCM_16')
    w('aiff_pcm16.aiff', i16, 'AIFF', 'PCM_16')
    w('aiff_pcm24.aiff', i24, 'AIFF', 'PCM_24')
    w('flac_16.flac', i16, 'FLAC', 'PCM_16')
    w('flac_24.flac', i24, 'FLAC', 'PCM_24')
    write_smpl_acid_wav(os.path.join(OUT, 'meta_smpl_acid.wav'), q16)

    y = known_signal(N_LOSSY).astype(np.float32)
    w('mp3_cbr320.mp3', y, 'MP3', 'MPEG_LAYER_III', bitrate_mode='CONSTANT', compression_level=0.0)
    w('mp3_vbr.mp3', y, 'MP3', 'MPEG_LAYER_III', bitrate_mode='VARIABLE', compression_level=0.0)
    w('ogg_vorbis.ogg', y, 'OGG', 'VORBIS', compression_level=0.0)
    ref_ogg, _ = sf.read(os.path.join(OUT, 'ogg_vorbis.ogg'), dtype='float32', frames=N_LOSSLESS)
    ref_ogg.astype('<f4').tofile(os.path.join(OUT, 'ogg_vorbis_libvorbis_f32.raw'))

    files = {}
    for name in sorted(os.listdir(OUT)):
        if name == 'manifest.json':
            continue
        p = os.path.join(OUT, name)
        size = os.path.getsize(p)
        if size > 64 * 1024:
            sys.exit(f'{name}: {size} bytes > 64 KiB')
        with open(p, 'rb') as f:
            files[name] = {'bytes': size, 'sha256': hashlib.sha256(f.read()).hexdigest()}
    manifest = {
        'generator': 'tools/fixtures/make_audio_fixtures.py',
        'encoder': f'python-soundfile {sf.__version__}, libsndfile {sf.__libsndfile_version__} '
                   f'(bundled libFLAC, LAME, libvorbis), numpy {np.__version__}',
        'rate': RATE, 'framesLossless': N_LOSSLESS, 'framesLossy': N_LOSSY,
        'files': files,
    }
    with open(os.path.join(OUT, 'manifest.json'), 'w', newline='\n') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')
    for name, info in files.items():
        print(f'{name:22s} {info["bytes"]:6d}')


if __name__ == '__main__':
    main()
