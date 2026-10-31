"""M4 review, DSP sub-review: helpers to render a stock device through `pqc render` and analyse it.

A probe is a tiny wrapper patch that imports a stock device by absolute path, feeds it a test signal generated
inside the patch (root `in`s read silence), and outputs the device's output. Renders and patches go to the
scratch directory (env PQ_PROBE_SCRATCH), never into the repo.
"""
import os
import subprocess
import hashlib
import numpy as np
from scipy.io import wavfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
PQC = os.environ.get("PQ_PQC", os.path.join(REPO, "build", "release", "tools", "pqc", "pqc.exe"))
SCRATCH = os.environ.get(
    "PQ_PROBE_SCRATCH",
    r"C:\Users\Luke\AppData\Local\Temp\claude\C--Users-Luke-Desktop-programming-pqdaw"
    r"\dc880915-a517-4e42-b333-63d5928f1e7f\scratchpad\dsp",
)
STDLIB = os.path.join(REPO, "stdlib").replace("\\", "/")


def render(body, imports, seconds=0.5, rate=48000, notes=None, voices=None, backend="jit", extra=None, out_type="audio stereo"):
    """Render `patch probe { out y : <out_type>; <body> }`. Returns (rate, float64 array [n] or [n, ch])."""
    imp = "\n".join('import "%s/%s"' % (STDLIB, i) for i in imports)
    if notes is None:
        src = "%s\npatch probe {\n  out y : %s\n%s\n}\n" % (imp, out_type, body)
    else:
        src = "%s\npatch probe {\n  out y : %s\n%s\n}\n" % (imp, out_type, body)
    key = hashlib.sha1((src + repr((seconds, rate, notes, voices, backend, extra))).encode()).hexdigest()[:16]
    os.makedirs(SCRATCH, exist_ok=True)
    pqp = os.path.join(SCRATCH, "p_%s.pqp" % key)
    wav = os.path.join(SCRATCH, "p_%s.wav" % key)
    if not os.path.exists(wav):
        with open(pqp, "w", encoding="utf-8") as f:
            f.write(src)
        cmd = [PQC, "render", pqp, "--patch", "probe", "--seconds", str(seconds), "--rate", str(rate),
               "--backend", backend, "-o", wav]
        if notes is not None:
            cmd += ["--notes", notes]
        if voices is not None:
            cmd += ["--voices", str(voices)]
        if extra:
            cmd += extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("pqc failed (%d):\n%s\n%s\n--- patch ---\n%s" % (r.returncode, r.stdout, r.stderr, src))
    sr, data = wavfile.read(wav)
    return sr, data.astype(np.float64)


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-30))


def freqresp(h, freqs, sr):
    """DTFT of impulse response h at freqs (Hz)."""
    # Memory-light (the review machine is shared): trim the tail, then evaluate a few frequencies at a time.
    h = np.asarray(h, dtype=np.float64)
    nz = np.nonzero(np.abs(h) > 1e-12 * max(np.max(np.abs(h)), 1e-30))[0]
    h = h[: (nz[-1] + 1 if len(nz) else 1)]
    n = np.arange(len(h), dtype=np.float64)
    w = 2 * np.pi * np.atleast_1d(np.asarray(freqs, dtype=np.float64)) / sr
    out = np.empty(len(w), dtype=np.complex128)
    step = max(1, int(2e6 // max(len(h), 1)))  # <= ~32 MB per chunk
    for i in range(0, len(w), step):
        out[i:i + step] = np.exp(-1j * np.outer(w[i:i + step], n)) @ h
    return out


def tone_amp(x, f, sr):
    """Amplitude of the sinusoid at f in x (least squares on a Hann-free exact fit over the whole buffer)."""
    n = np.arange(len(x))
    c = np.cos(2 * np.pi * f * n / sr)
    s = np.sin(2 * np.pi * f * n / sr)
    A = np.vstack([c, s, np.ones_like(c)]).T
    coef, *_ = np.linalg.lstsq(A, x, rcond=None)
    return np.hypot(coef[0], coef[1])


def peak_freq(x, sr, fmin=1.0, fmax=None, zp=8):
    """Peak frequency with zero-padded FFT and parabolic interpolation."""
    n = len(x)
    w = np.hanning(n)
    X = np.abs(np.fft.rfft(x * w, n * zp))
    f = np.fft.rfftfreq(n * zp, 1 / sr)
    lo = np.searchsorted(f, fmin)
    hi = len(f) if fmax is None else np.searchsorted(f, fmax)
    i = lo + int(np.argmax(X[lo:hi]))
    a, b, c = np.log(X[i - 1] + 1e-30), np.log(X[i] + 1e-30), np.log(X[i + 1] + 1e-30)
    p = 0.5 * (a - c) / (a - 2 * b + c)
    return f[i] + p * (f[1] - f[0])


# ---- independent analog prototypes (normalised: cutoff/centre at s = j) -------------------------------------------
def proto(kind, s, Q=0.7071067811865476, dB=0.0):
    A = 10 ** (dB / 40)
    if kind == "lp":
        return 1 / (s * s + s / Q + 1)
    if kind == "hp":
        return s * s / (s * s + s / Q + 1)
    if kind == "bp":  # unity peak
        return (s / Q) / (s * s + s / Q + 1)
    if kind == "notch":
        return (s * s + 1) / (s * s + s / Q + 1)
    if kind == "bell":  # RBJ peaking EQ
        return (s * s + s * A / Q + 1) / (s * s + s / (A * Q) + 1)
    if kind == "lowshelf":  # RBJ low shelf
        sq = np.sqrt(A)
        return A * (s * s + sq / Q * s + A) / (A * s * s + sq / Q * s + 1)
    if kind == "highshelf":
        sq = np.sqrt(A)
        return A * (A * s * s + sq / Q * s + 1) / (s * s + sq / Q * s + A)
    if kind in ("hp8", "lp8"):  # Butterworth 8th order: section Qs 1/(2 sin((2i-1)pi/16))
        h = np.ones_like(s)
        for i in range(1, 5):
            qi = 1 / (2 * np.sin((2 * i - 1) * np.pi / 16))
            h = h * proto("hp" if kind == "hp8" else "lp", s, qi)
        return h
    raise ValueError(kind)


def bilinear_resp(kind, f, f0, sr, **kw):
    """The bilinear-transformed (prewarped at f0) digital response at f."""
    s = 1j * np.tan(np.pi * np.asarray(f) / sr) / np.tan(np.pi * f0 / sr)
    return proto(kind, s, **kw)


def analog_resp(kind, f, f0, **kw):
    s = 1j * np.asarray(f) / f0
    return proto(kind, s, **kw)
