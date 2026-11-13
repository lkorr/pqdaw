"""NZ.1: the noise module's colour (tilt) filter, designed offline (docs/plans/NZ.md "Colour").

The filter runs on the SEQUENCE of drawn values (once per step), so it is a fixed function of the colour c in dB/oct
in step-normalised frequency; the spec is ±0.5 dB of the target slope c·log2(f) over 20 Hz … 20 kHz at 48 kHz
(4.17e-4 … 0.4167 cycles per step), output power normalised to the input's (unit power gain).

Structure: four first-order sections H(z) = Π (1 − z_k z⁻¹) / (1 − p_k z⁻¹) with FIXED poles p_k (independent of the
colour: chosen by a joint minimax over colours ±1.5 … ±6 dB/oct) and per-colour zeros z_k(c) (a minimax per colour on a
0.25 dB/oct grid, continued from c = 0 where z = p: white is the identity). At run time the zeros and the gain are
interpolated linearly between grid rows (pq/dsp/noise_step.h). This script prints the C++ table and checks:
  - the slope error of every interpolated design (481 colours) on 1200 log-spaced frequencies,
  - the gain error of the interpolated gain against the exact unit-power gain of the interpolated zeros.

Run: python tools/fit/noise_colour.py > table.txt   (numpy + scipy; ~2 min)
"""
import numpy as np
from scipy.optimize import minimize

FS = 48000.0
F = np.geomspace(20, 20000, 160) / FS
LF = np.log2(F)
STEP = 0.25
DB_PER_OCT = 20 * np.log10(2)  # 6.0206: slope beta = colour / 6.0206 in amplitude


def db(p, z, f=F):
    w = np.exp(-2j * np.pi * f)
    h = np.ones_like(w)
    for pk, zk in zip(p, z):
        h = h * (1 - zk * w) / (1 - pk * w)
    return 20 * np.log10(np.abs(h))


def proto(n, flo, fhi, beta):  # analog pinking prototype (matched z): only the optimiser's start
    fk = np.geomspace(flo, fhi, n)
    r = (fhi / flo) ** (1.0 / (n - 1))
    return np.exp(-2 * np.pi * fk / FS), np.exp(-2 * np.pi * fk * r ** (-beta) / FS)


def minimax(fun_err, x0, nvar):
    """minimize t s.t. |err(x) − c| ≤ t (epigraph form): variables x (nvar), c, t."""
    def cons(v):
        e = fun_err(v[:nvar]) - v[nvar]
        return np.concatenate([v[-1] - e, v[-1] + e])
    e0 = fun_err(x0)
    v0 = np.concatenate([x0, [(e0.max() + e0.min()) / 2, (e0.max() - e0.min()) / 2 + 0.1]])
    r = minimize(lambda v: v[-1], v0, method='SLSQP', constraints=[{'type': 'ineq', 'fun': cons}],
                 options={'maxiter': 1500, 'ftol': 1e-13})
    return r.x[:nvar], r.x[-1]


def joint_poles(n=4, flo=5.0, fhi=12000.0):
    betas = np.array([-1, -0.75, -0.5, -0.25, 0.25, 0.5, 0.75, 1.0])
    k = len(betas)
    p0 = proto(n, flo, fhi, 0)[0]
    x0 = np.concatenate([-np.log(1 - p0)] + [np.arctanh(np.clip(proto(n, flo, fhi, b)[1], -0.999999, 0.999999))
                                              for b in betas] + [np.zeros(k), [3.0]])

    def unpack(x):
        return 1 - np.exp(-x[:n]), np.tanh(x[n:n + n * k]).reshape(k, n), x[n + n * k:n + n * k + k], x[-1]

    def cons(x):
        p, z, c, t = unpack(x)
        out = []
        for i, b in enumerate(betas):
            e = db(p, z[i]) - b * DB_PER_OCT * LF - c[i]
            out += [t - e, t + e]
        return np.concatenate(out)
    r = minimize(lambda x: x[-1], x0, method='SLSQP', constraints=[{'type': 'ineq', 'fun': cons}],
                 options={'maxiter': 1500, 'ftol': 1e-12})
    return unpack(r.x)[0]


def power_gain(p, z):
    """Σ h[n]² of Π (1 − z_k w)/(1 − p_k w) by partial fractions (distinct poles)."""
    n = len(p)
    c0 = np.prod(z / p)
    r = np.array([np.prod(1 - z / p[k]) / np.prod([1 - p[m] / p[k] for m in range(n) if m != k]) for k in range(n)])
    return c0 * c0 + 2 * c0 * r.sum() + sum(r[j] * r[k] / (1 - p[j] * p[k]) for j in range(n) for k in range(n))


def main():
    p = joint_poles()
    colours = np.arange(-6, 6 + 1e-9, STEP)
    betas = colours / DB_PER_OCT
    i0 = len(colours) // 2
    rows = {i0: np.array(p)}
    for rng in (range(i0 + 1, len(colours)), range(i0 - 1, -1, -1)):
        prev = np.array(p)
        for i in rng:
            b = betas[i]
            x, _ = minimax(lambda x: db(p, np.tanh(x)) - b * DB_PER_OCT * LF,
                           np.arctanh(np.clip(prev, -0.9999999, 0.9999999)), len(p))
            rows[i] = np.tanh(x)
            prev = rows[i]
    Z = np.array([rows[i] for i in range(len(colours))]).astype(np.float32).astype(np.float64)
    pf = np.array(p, dtype=np.float32).astype(np.float64)
    G = np.array([1.0 / np.sqrt(power_gain(pf, Z[i])) for i in range(len(colours))])
    G[i0] = 1.0
    # checks on the float32 coefficients the kernel uses
    ff = np.geomspace(20, 20000, 1200) / FS
    worst_slope = worst_gain = 0.0
    for c in np.linspace(-6, 6, 481):
        x = (c + 6) / STEP
        i = min(int(np.floor(x)), len(colours) - 2)
        fr = x - i
        z = Z[i] * (1 - fr) + Z[i + 1] * fr
        g = G[i] * (1 - fr) + G[i + 1] * fr
        if abs(c) > 1e-9:
            e = db(pf, z, ff) - c * np.log2(ff)
            worst_slope = max(worst_slope, (e.max() - e.min()) / 2)
            worst_gain = max(worst_gain, abs(20 * np.log10(g * np.sqrt(power_gain(pf, z)))))
    print(f'// Measured (tools/fit/noise_colour.py): slope error ≤ ±{worst_slope:.3f} dB over 20 Hz … 20 kHz at 48 kHz for')
    print(f'// every colour (481 interpolated designs); interpolated gain within {worst_gain:.3f} dB of unit power.')
    print('inline constexpr float kNoiseColourPoles[4] = {' + ', '.join(f'{v:.9g}f' for v in pf) + '};')
    print(f'inline constexpr uint32_t kNoiseColourRows = {len(colours)};  // colour −6 … +6 dB/oct in {STEP} steps')
    print('// per row: zeros z0 … z3, then the unit-power gain')
    print('inline constexpr float kNoiseColourTable[kNoiseColourRows][5] = {')
    for i, c in enumerate(colours):
        vals = ', '.join(f'{v:.9g}f' for v in list(Z[i]) + [G[i]])
        print(f'    {{{vals}}},  // {c:+.2f}')
    print('};')


if __name__ == '__main__':
    main()
