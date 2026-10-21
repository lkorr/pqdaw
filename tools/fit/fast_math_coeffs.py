"""Minimax coefficient fits for src/core/include/pq/core/fast_math.h.

Run: python tools/fit/fast_math_coeffs.py   (needs numpy only)

Linear Remez exchange over a general basis with a weight function, and a rational Remez (iterated
linearisation) for tanh. Coefficients are then rounded to float32 and the error of a float32
evaluation is re-measured here (numpy float32, FMA not emulated, so the C++ sweep tests are the
authority; this script only chooses coefficients).
"""
import numpy as np


def remez(f, basis, w, a, b, iters=40, grid=200001):
    """Minimise max |w(x) (f(x) - sum c_k basis_k(x))| on [a, b]."""
    n = len(basis)
    k = np.arange(n + 2)
    ref = (a + b) / 2 - (b - a) / 2 * np.cos(np.pi * k / (n + 1))
    xs = np.linspace(a, b, grid)
    fx = f(xs)
    wx = w(xs)
    B = np.stack([p(xs) for p in basis], axis=1)
    best = None
    for _ in range(iters):
        A = np.zeros((n + 2, n + 1))
        for j, p in enumerate(basis):
            A[:, j] = p(ref)
        A[:, n] = (-1.0) ** k / w(ref)
        sol = np.linalg.lstsq(A, f(ref), rcond=None)[0]
        c = sol[:n]
        err = wx * (fx - B @ c)
        emax = np.max(np.abs(err))
        if best is None or emax < best[1]:
            best = (c, emax)
        # New reference: one extremum per sign run, then keep n+2 alternating points.
        sgn = np.sign(err)
        sgn[sgn == 0] = 1
        idx = []
        start = 0
        for i in range(1, grid + 1):
            if i == grid or sgn[i] != sgn[start]:
                seg = np.arange(start, i)
                idx.append(seg[np.argmax(np.abs(err[seg]))])
                start = i
        idx = list(idx)
        while len(idx) > n + 2:
            # drop the smaller end point (keeps alternation)
            if abs(err[idx[0]]) < abs(err[idx[-1]]):
                idx.pop(0)
            else:
                idx.pop()
        if len(idx) < n + 2:
            break
        ref = xs[np.array(idx)]
    return best


def eval_basis_f32(c, basis_pows, x):
    """Horner in float32 of sum c_k x^k for k in basis_pows (contiguous)."""
    c = np.float32(c)
    acc = np.float32(c[-1]) * np.ones_like(x, dtype=np.float32)
    for ck in c[-2::-1]:
        acc = acc * x + ck
    return acc


def show(name, c, emax):
    print(f"{name}: minimax weighted err {emax:.3e}")
    for i, v in enumerate(c):
        print(f"  c{i} = {np.float32(v)!r:>18}  ({v:.17g})")


def fit_exp2(deg):
    # 2^x = 1 + x * P(x), x in [-0.5, 0.5], relative error
    basis = [lambda x, i=i: x ** (i + 1) for i in range(deg)]
    c, e = remez(lambda x: 2.0 ** x - 1.0, basis, lambda x: 1.0 / 2.0 ** x, -0.5, 0.5)
    show(f"exp2 deg {deg} (2^x = 1 + x*P(x), rel)", c, e)
    return c


def fit_log2(deg):
    # log2(1+t) = t * P(t), t in [sqrt(.5)-1, sqrt(2)-1], relative error
    lo, hi = np.sqrt(0.5) - 1, np.sqrt(2.0) - 1
    def g(t):
        out = np.empty_like(t)
        nz = np.abs(t) > 1e-12
        out[nz] = np.log2(1 + t[nz]) / t[nz]
        out[~nz] = 1 / np.log(2)
        return out
    basis = [lambda t, i=i: t ** i for i in range(deg)]
    c, e = remez(g, basis, lambda t: 1.0 / g(t), lo, hi)
    show(f"log2 deg {deg} (log2(1+t) = t*P(t), rel)", c, e)
    return c


def fit_sin(deg):
    # sin(r) = r + r^3 * P(r^2), r in [-(pi/2 + 0.01), pi/2 + 0.01] (slack for reduction rounding at large |x|);
    # fit in u = r^2 with relative weight. The leading coefficient is fixed at 1 so sin(r) == r for tiny r.
    hi = (np.pi / 2 + 0.01) ** 2
    def g(u):  # (sin(r) - r) / r^3
        r = np.sqrt(u)
        out = np.full_like(u, -1.0 / 6.0)
        nz = r > 1e-3
        out[nz] = (np.sin(r[nz]) - r[nz]) / r[nz] ** 3
        small = ~nz
        out[small] = -1 / 6 + u[small] / 120 - u[small] ** 2 / 5040
        return out
    def w(u):  # relative to sin(r) = r (1 + u g)  ->  weight on the g error is u / (1 + u g)
        return np.maximum(u, 1e-300) / (1 + u * g(u))
    basis = [lambda u, i=i: u ** i for i in range(deg)]
    c, e = remez(g, basis, w, 1e-6, hi)
    show(f"sin: r + r^3*P(r^2), P deg {deg - 1} (rel)", c, e)
    return c


def fit_tan(deg):
    # tan(x) = x * P(x^2) / (pi^2/4 - x^2), x in [0, pi/2); fit P in u = x^2, relative
    hi = (np.pi / 2) ** 2
    def g(u):
        x = np.sqrt(u)
        out = np.empty_like(u)
        small = x < 1e-6
        mid = (~small) & (x < np.pi / 2 - 1e-7)
        out[small] = np.pi ** 2 / 4
        out[mid] = np.tan(x[mid]) * (np.pi ** 2 / 4 - u[mid]) / x[mid]
        out[~(small | mid)] = 2.0  # limit at the pole: tan(x)(pi/2-x) -> 1, (pi/2+x)/x -> 2
        return out
    basis = [lambda u, i=i: u ** i for i in range(deg)]
    c, e = remez(g, basis, lambda u: 1.0 / g(u), 0.0, hi * (1 - 1e-12))
    show(f"tan deg {deg} in u (tan = x*P(x^2)/(pi^2/4-x^2), rel)", c, e)
    return c


def rational_remez(f, w, a, b, np_, nq, numf, denf, iters=60, grid=200001):
    """Minimise max |w (f - N/D)|, N = sum p_i numf_i, D = 1 + sum q_j denf_j (j>=1)."""
    n = np_ + nq + 1  # unknowns incl E
    k = np.arange(n + 1)
    ref = (a + b) / 2 - (b - a) / 2 * np.cos(np.pi * k / n)
    xs = np.linspace(a, b, grid)
    fx, wx = f(xs), w(xs)
    best = None
    for _ in range(iters):
        Dprev = np.ones_like(ref)
        E = 0.0
        for _ in range(30):
            A = np.zeros((n + 1, n))
            for i in range(np_):
                A[:, i] = numf(i, ref)
            for j in range(nq):
                A[:, np_ + j] = -(f(ref) + (-1.0) ** k * E / w(ref)) * denf(j + 1, ref)
            A[:, n - 1] = -((-1.0) ** k) * Dprev / w(ref)
            rhs = f(ref)
            sol = np.linalg.lstsq(A, rhs, rcond=None)[0]
            p, q, E = sol[:np_], sol[np_:np_ + nq], sol[n - 1]
            Dprev = 1 + sum(q[j] * denf(j + 1, ref) for j in range(nq))
        N = sum(p[i] * numf(i, xs) for i in range(np_))
        D = 1 + sum(q[j] * denf(j + 1, xs) for j in range(nq))
        err = wx * (fx - N / D)
        emax = np.max(np.abs(err))
        if best is None or emax < best[2]:
            best = (p, q, emax)
        sgn = np.sign(err)
        sgn[sgn == 0] = 1
        idx = []
        start = 0
        for i in range(1, grid + 1):
            if i == grid or sgn[i] != sgn[start]:
                seg = np.arange(start, i)
                idx.append(seg[np.argmax(np.abs(err[seg]))])
                start = i
        while len(idx) > n + 1:
            if abs(err[idx[0]]) < abs(err[idx[-1]]):
                idx.pop(0)
            else:
                idx.pop()
        if len(idx) < n + 1:
            break
        ref = xs[np.array(idx)]
    return best


def fit_tanh(np_, nq, clamp):
    # tanh(x) = x * N(x^2) / D(x^2), x in [0, clamp], weighted relative to tanh (=> abs near saturation)
    def f(x):
        return np.tanh(x)
    def w(x):
        return 1.0 / np.maximum(np.tanh(x), 1e-30)
    p, q, e = rational_remez(f, w, 1e-6, clamp, np_, nq,
                             lambda i, x: x ** (2 * i + 1), lambda j, x: x ** (2 * j))
    print(f"tanh [{2*np_-1}/{2*nq}] clamp {clamp}: rel err {e:.3e}")
    for i, v in enumerate(p):
        print(f"  p{i} (x^{2*i+1}) = {np.float32(v)!r:>18}  ({v:.17g})")
    for j, v in enumerate(q):
        print(f"  q{j+1} (x^{2*j+2}) = {np.float32(v)!r:>18}  ({v:.17g})")
    return p, q


if __name__ == "__main__":
    for d in (4, 5):
        fit_exp2(d)
    for d in (6, 7, 8):
        fit_log2(d)
    for d in (3, 4):
        fit_sin(d)
    for d in (4, 5):
        fit_tan(d)
    for (a, b, c) in ((4, 4, 9.0), (5, 4, 9.0)):
        fit_tanh(a, b, c)
