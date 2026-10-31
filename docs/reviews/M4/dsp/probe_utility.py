"""Utility: default transparency, width / balance / gain laws, bass mono crossover vs an independent LR4 model."""
import numpy as np
from pqprobe import render, freqresp, bilinear_resp, db


def ut(sig_l, sig_r, secs=0.3, sr=48000, **kw):
    a = ", ".join("%s: %s" % (k, v) for k, v in kw.items())
    body = "  l = %s\n  r = %s\n  y = utility((l, r)%s)" % (sig_l, sig_r, (", " + a) if a else "")
    _, y = render(body, ["devices/utility.pqp"], seconds=secs, rate=sr)
    return y


if __name__ == "__main__":
    sl, sr_ = "0.5 * sine(phasor(997Hz))", "0.3 * sine(phasor(3001Hz))"
    _, ref = render("  y = (0.5 * sine(phasor(997Hz)), 0.3 * sine(phasor(3001Hz)))", [], seconds=0.3)
    y = ut(sl, sr_)
    print("default: max |out - in| = %.3g (after the 2 ms fade-in: %.3g)" % (np.max(np.abs(y - ref)), np.max(np.abs(y[960:] - ref[960:]))))
    y = ut(sl, sr_, width=0.0)
    print("width 0: max |L - R| = %.3g" % np.max(np.abs(y[960:, 0] - y[960:, 1])))
    y = ut(sl, sr_, gain="35dB")
    print("gain +35 dB: L ratio %.2f dB" % db(np.max(np.abs(y[960:, 0])) / 0.5))
    imp = "(gate_to_real(pulse(0.1)) - gate_to_real(pulse(0)))"  # one impulse at 0.1 s (after the 2 ms fade-in)
    for sr in (44100, 48000, 192000):
        for fx in (50, 120, 500):
            # side-only impulse (L = imp, R = -imp) and mid-only (L = R = imp)
            ys = ut(imp, "-" + imp, secs=0.19, sr=sr, bassMono=1, bassFreq="%dHz" % fx)
            ym = ut(imp, imp, secs=0.19, sr=sr, bassMono=1, bassFreq="%dHz" % fx)
            f = np.geomspace(20, 20000, 120)
            n0 = int(0.1 * sr); ys = ys[n0:]; ym = ym[n0:]
            Hs = freqresp(ys[:, 0], f, sr)          # side in -> L out (= S' )
            Hm = freqresp(ym[:, 0], f, sr)          # mid in -> L out (= M' allpass)
            lr4hp = bilinear_resp("hp", f, fx, sr, Q=0.7071067811865476) ** 2
            m = db(lr4hp) > -60
            print("sr %6d fx %3d: side = LR4 HP err %.4f dB (at fx %+.2f dB);  mid |H| range %.4f..%.4f dB"
                  % (sr, fx, np.max(np.abs(db(Hs)[m] - db(lr4hp)[m])), db(freqresp(ys[:, 0], [fx], sr))[0], db(Hm).min(), db(Hm).max()))
