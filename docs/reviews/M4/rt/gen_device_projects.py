"""M4 RT review: synthetic schema-6 projects with stock device chains, racks, macros, LFO devices and wires, to time
`pqdaw --project <dir> --render out.wav --seconds S` (the playback path: ProjectLink -> Engine -> ProjectProgram).

usage: python -I gen_device_projects.py <out_root> [tracks]
writes <out_root>/<name>/project.pqd for every configuration in CONFIGS (default 30 tracks).

Notes modes: none (no clips: a silent project), short (a 3-note chord for one beat at 0, then silence: tails, sleep),
held (the chord held for the whole render).
Chains (per MIDI track, after the instrument):
  basic      BasicSynth only (the M3 baseline)
  chain      BasicSynth -> Utility -> EQ Eight (defaults) -> Saturator (defaults)
  chainhq    BasicSynth -> Utility (bass mono) -> EQ Eight (M/S, HQ) -> Saturator (HQ 4x)
  rack       BasicSynth -> Audio Effect Rack [A: EQ Eight -> Saturator | B: Utility]
  rackmute   the same, chain B muted
  macro      rack + macro 1 mapped to EQ Eight freq4 and Saturator drive
  lfo        BasicSynth -> LFO -> EQ Eight (LFO not wired)
  wire       BasicSynth -> LFO -> EQ Eight, wire lfo -> EQ Eight freq4 (same track)
  op         Operator (defaults) -> Utility -> EQ Eight -> Saturator
"""
import json, os, sys

PPQ = 27720  # pq::kPpq (core/time.h); 120 BPM -> one beat = 0.5 s

ids = [100000]


def nid():
    ids[0] += 1
    return ids[0]


def dev(patch, kind="audioEffect", params=None, settings=None, active=True, **extra):
    d = {"id": nid(), "patch": patch, "kind": kind}
    if not active:
        d["active"] = False
    if params:
        d["params"] = params
    if settings:
        d["settings"] = settings
    d.update(extra)
    return d


def chain_devices(kind, wires):
    if kind == "basic":
        return [dev("BasicSynth", "instrument")]
    if kind == "chain":
        return [dev("BasicSynth", "instrument"), dev("utility"), dev("eq_eight"), dev("saturator")]
    if kind == "chainhq":
        return [dev("BasicSynth", "instrument"), dev("utility", settings={"bassMono": 1}),
                dev("eq_eight", settings={"mode": 2, "hq": 2}), dev("saturator", settings={"hq": 4})]
    if kind == "op":
        return [dev("operator", "instrument"), dev("utility"), dev("eq_eight"), dev("saturator")]
    if kind in ("rack", "rackmute", "macro"):
        eq, sat, ut = dev("eq_eight"), dev("saturator"), dev("utility")
        chA = {"id": nid(), "name": "A", "devices": [eq, sat]}
        chB = {"id": nid(), "name": "B", "devices": [ut]}
        if kind == "rackmute":
            chB["active"] = False
        rack = {"chains": [chA, chB]}
        params = None
        if kind == "macro":
            rack["mappings"] = [
                {"id": nid(), "macro": 1, "device": eq["id"], "param": "freq4", "min": 0.2, "max": 0.8},
                {"id": nid(), "macro": 1, "device": sat["id"], "param": "drive", "min": 0.4, "max": 0.6}]
            params = {"macro1": 64.0}
        return [dev("BasicSynth", "instrument"), dev("AudioEffectRack", params=params, rack=rack)]
    if kind in ("lfo", "wire"):
        lfo, eq = dev("lfo_device", params={"rate": 0.5}), dev("eq_eight")
        if kind == "wire":
            wires.append({"id": nid(), "from": lfo["id"], "port": "lfo", "to": eq["id"], "param": "freq4", "amount": 0.3})
        return [dev("BasicSynth", "instrument"), lfo, eq]
    raise ValueError(kind)


def clip(notes_mode, seconds):
    if notes_mode == "none":
        return []
    length = PPQ if notes_mode == "short" else PPQ * 2 * int(seconds + 2)
    return [{"id": nid(), "name": "c", "colour": "#808080", "start": 0, "length": length, "contentStart": 0,
             "loop": {"enabled": False, "start": 0, "end": length}, "signature": {"numerator": 4, "denominator": 4},
             "tuning": "inherit", "active": True,
             "notes": [{"id": nid(), "start": 0, "length": length, "pitch": {"degree": d, "equave": 0}, "velocity": 90.0}
                       for d in (0, 10, 18)]}]


def mixer(vol=-12.0, output="default"):
    return {"volumeDb": vol, "pan": 0.0, "mute": False, "solo": False, "arm": False, "sends": [], "output": output}


def project(tracks, kind, notes_mode, seconds):
    wires = []
    trs = []
    for i in range(tracks):
        trs.append({"id": 1000 + i, "kind": "midi", "name": f"T{i}", "colour": "#808080", "active": True,
                    "tuning": {"main": 8}, "mixer": mixer(), "lanes": [], "devices": chain_devices(kind, wires),
                    "clips": clip(notes_mode, seconds)})
    p = {"schemaVersion": 6,
         "timeline": {"tempo": [{"tick": 0, "bpm": 120.0, "curve": "step"}],
                      "signatures": [{"bar": 0, "numerator": 4, "denominator": 4}]},
         "loop": {"start": 0, "end": PPQ * 16, "enabled": False}, "locators": [],
         "tunings": [{"id": 8, "name": "31-EDO", "colour": "#22AA44",
                      "def": {"kind": "edo", "n": 31, "equave": "2/1", "referenceHz": 261.626}}],
         "tracks": trs, "returns": [],
         "master": {"id": 2, "kind": "master", "name": "Master", "colour": "#808080", "active": True,
                    "tuning": "inherit", "mixer": mixer(0.0), "lanes": [], "devices": []}}
    if wires:
        p["wires"] = wires
    return p


CONFIGS = [(k, n) for k in ("basic", "chain", "chainhq", "rack", "rackmute", "macro", "lfo", "wire", "op")
           for n in ("none", "short", "held")]

if __name__ == "__main__":
    root = sys.argv[1]
    tracks = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    for kind, notes in CONFIGS:
        d = os.path.join(root, f"t{tracks}_{kind}_{notes}")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "project.pqd"), "w") as f:
            json.dump(project(tracks, kind, notes, 60), f, indent=1)
        print(d)
