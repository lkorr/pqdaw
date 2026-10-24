"""Probe 9: the T3.17 playback fixture rendered end to end: per-note presence at the model's onset/pitch."""
import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

d = os.path.join(WT, "tests", "app", "fixtures", "playback")
p = json.load(open(os.path.join(d, "project.pqd"), encoding="utf-8"))
x, log, rc = render(d, 5, name="fixture_playback")
print(log)
tempo = p["timeline"]["tempo"]
tun = {t["id"]: t["def"] for t in p["tunings"]}


def hz_of(defn, pitch):
    if defn["kind"] == "edo":
        return edo_hz(defn["referenceHz"], defn["n"], pitch["degree"], pitch["equave"])
    lyd = [0, 210, 420, 630, 705, 915, 1125]  # 5L2s g=705 brightest mode
    return defn["referenceHz"] * 2 ** ((lyd[pitch["degree"]] + 1200 * pitch["equave"]) / 1200)


def walk(ts):
    for t in ts:
        yield t
        yield from walk(t.get("children", []))


for t in walk(p["tracks"]):
    if t["kind"] != "midi":
        continue
    defn = tun[t["tuning"]["main"]] if isinstance(t["tuning"], dict) else tun[p["tunings"][0]["id"]]
    for c in t["clips"]:
        for n in c["notes"]:
            s = ticks_to_samples(tempo, c["start"] + n["start"])
            f = hz_of(defn, n["pitch"])
            seg = x[s + 2400:s + 4800, 0] + x[s + 2400:s + 4800, 1]
            tt = np.arange(seg.size) / SR
            w = np.hanning(seg.size)
            a = abs(np.dot(seg * w, np.exp(-2j * np.pi * f * tt))) * 2 / w.sum()
            print(f"track {t['name']:6s} note {n['id']} at {s/SR:.3f}s {f:8.3f} Hz: amplitude {a:.5f}"
                  f"{'  (muted track)' if t['mixer']['mute'] else ''}")
