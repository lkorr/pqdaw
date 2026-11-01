"""Coefficients of the frame-table interpolator (src/dsp/src/frame_table_interp.h, TMM.1).

Run: python tools/fit/frame_interp_coeffs.py   (needs numpy + scipy)

A 6-point, 5th-order piecewise-polynomial interpolator for data that is 4x oversampled (content at most 1/8 cycle
per sample). Its impulse response h(t), t in [-3, 3], is symmetric; on |t| in [d, d + 1) (d = 0, 1, 2) it is
P_d(z) = sum_n v[d][n] z^n with z = |t| - (d + 1/2). Reading at fraction x in [0, 1) between samples y0 and y1,
with z = x - 1/2, e_d = y_{-d} + y_{d+1} and o_d = y_{-d} - y_{d+1}:
    out = sum_n c_n z^n,   c_n = sum_d v[d][n] * (e_d if n even else o_d).

Design: a linear program (minimax). Minimise max |H(f)| over every image band [m - 1/8, m + 1/8], m = 1 .. 24,
subject to H(0) = 1, |H(f) - 1| <= 0.01 on the passband [0, 1/8] (the table builder divides every harmonic by
H(h / len), so passband droop costs nothing) and h continuous (C0, h(3) = 0). H is the continuous Fourier
transform of h. Continuity makes |H| fall as 1/f^2, so the bands above m = 24 are lower still (checked up to
m = 400 below). Rounded to float32, the worst image is -130.8 dB below the passband (C1 continuity: -121 dB;
3x oversampling with C0: -115 dB). The plan's target was >= 105 dB.
"""
import numpy as np
from scipy.optimize import linprog

g0, w0 = np.polynomial.legendre.leggauss(24)
SUB = 256  # composite Gauss-Legendre: exact enough for f up to ~400
gx = np.concatenate([(g0 + 1) / SUB - 1 + 2 * k / SUB for k in range(SUB)])
gw = np.concatenate([w0 / SUB] * SUB)
Z = gx * 0.5
W = gw * 0.5
NU = 1 / 8


def h_row(f):
    """H(f) as a linear function of the 18 coefficients v[d][n] (index d*6 + n)."""
    row = np.zeros(18)
    for d in range(3):
        c = np.cos(2 * np.pi * f * (Z + d + 0.5))
        for n in range(6):
            row[d * 6 + n] = 2 * np.sum(W * Z ** n * c)
    return row


def value_row(d, z):
    row = np.zeros(18)
    for n in range(6):
        row[d * 6 + n] = z ** n
    return row


def design(delta=0.01, bands=24, grid=24):
    a_ub, b_ub = [], []
    for m in range(1, bands + 1):
        for f in np.linspace(m - NU, m + NU, grid):
            r = h_row(f)
            a_ub += [np.r_[r, -1], np.r_[-r, -1]]
            b_ub += [0, 0]
    for f in np.linspace(0, NU, 2 * grid):
        r = h_row(f)
        a_ub += [np.r_[r, 0], np.r_[-r, 0]]
        b_ub += [1 + delta, -(1 - delta)]
    a_eq = [np.r_[h_row(0.0), 0]]
    b_eq = [1]
    a_eq.append(np.r_[value_row(0, 0.5) - value_row(1, -0.5), 0])  # continuous at |t| = 1
    a_eq.append(np.r_[value_row(1, 0.5) - value_row(2, -0.5), 0])  # at |t| = 2
    a_eq.append(np.r_[value_row(2, 0.5), 0])                      # h(3) = 0
    b_eq += [0, 0, 0]
    c = np.zeros(19)
    c[-1] = 1
    res = linprog(c, A_ub=np.array(a_ub), b_ub=np.array(b_ub), A_eq=np.array(a_eq), b_eq=np.array(b_eq),
                  bounds=[(None, None)] * 19, method="highs")
    assert res.status == 0, res.message
    return res.x[:18]


def main():
    v = design().astype(np.float32).astype(np.float64)
    passband = [h_row(f) @ v for f in np.linspace(0, NU, 200)]
    worst, at = 0.0, 0.0
    for m in range(1, 401):
        for f in np.linspace(m - NU, m + NU, 41):
            a = abs(h_row(f) @ v)
            if a > worst:
                worst, at = a, f
    print(f"// passband H in [{min(passband):.6f}, {max(passband):.6f}]; worst image {20 * np.log10(worst / min(passband)):.1f} dB"
          f" (f = {at:.4f})")
    print("inline constexpr float kFrameInterp[3][6] = {")
    for d in range(3):
        print("    {" + ", ".join(f"{float(np.float32(x))!r}f" for x in v[d * 6:(d + 1) * 6]) + "},")
    print("};")


if __name__ == "__main__":
    main()
