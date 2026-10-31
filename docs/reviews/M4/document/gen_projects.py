"""M4 review (document): writes small hand-made schema-6 projects for `pqdaw --render` probes.

Usage: python gen_projects.py <out_dir>
Each project is <out_dir>/<name>/project.pqd. Renders and their analysis: analyse.py.

Families:
  map_*      macro-mapping law: a rack-wrapped Utility whose gain is driven by macro 1 vs the same gain set directly
  rackid_*   a one-chain Audio Effect Rack (chain at 0 dB, centre) vs the bare device: should be transparent
  adv_*      adversarial / malformed files: each should be refused cleanly (or load) without a crash
"""
import copy
import json
import os
import sys

PPQ = 221760
BAR = 4 * PPQ


def note(i, start, length, degree, vel=100.0):
    return {"id": i, "start": start, "length": length, "pitch": {"degree": degree, "equave": 0}, "velocity": vel}


def mixer(vol=0.0):
    return {"volumeDb": vol, "pan": 0.0, "mute": False, "solo": False, "arm": False, "sends": [], "output": "default"}


def midi_track(tid, devices, clip_id, note_ids):
    notes = [note(note_ids[0], 0, 2 * BAR, 9), note(note_ids[1], 0, 2 * BAR, 16), note(note_ids[2], 0, 2 * BAR, 21)]
    return {
        "id": tid, "kind": "midi", "name": "M", "colour": "#FF00A0", "active": True, "tuning": "inherit",
        "mixer": mixer(), "devices": devices,
        "clips": [{
            "id": clip_id, "name": "A", "colour": "#808080", "start": 0, "length": 2 * BAR, "contentStart": 0,
            "loop": {"enabled": False, "start": 0, "end": 2 * BAR}, "signature": {"numerator": 4, "denominator": 4},
            "tuning": "inherit", "active": True, "notes": notes,
        }],
    }


def project(tracks):
    return {
        "schemaVersion": 6,
        "timeline": {"tempo": [{"tick": 0, "bpm": 120.0, "curve": "step"}],
                     "signatures": [{"bar": 0, "numerator": 4, "denominator": 4}]},
        "loop": {"start": 0, "end": 4 * BAR, "enabled": False},
        "locators": [],
        "tunings": [{"id": 1, "name": "12-EDO", "colour": "#5E9BD6",
                     "def": {"kind": "edo", "n": 12, "equave": "2/1", "referenceHz": 261.626}}],
        "tracks": tracks,
        "returns": [],
        "master": {"id": 2, "kind": "master", "name": "Master", "colour": "#808080", "active": True, "tuning": "inherit",
                   "mixer": mixer(), "devices": []},
    }


SYNTH = {"id": 10, "patch": "BasicSynth", "kind": "instrument"}


def utility(did, params=None):
    d = {"id": did, "patch": "utility", "kind": "audioEffect"}
    if params:
        d["params"] = params
    return d


def rack(did, chains, mappings=None, params=None, kind="audioEffect"):
    d = {"id": did, "patch": "AudioEffectRack" if kind == "audioEffect" else "InstrumentRack", "kind": kind,
         "rack": {"chains": chains}}
    if mappings:
        d["rack"]["mappings"] = mappings
    if params:
        d["params"] = params
    return d


def chain(cid, devices, **kw):
    c = {"id": cid, "name": "Chain", "devices": devices}
    c.update(kw)
    return c


def write(out, name, proj):
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "project.pqd"), "w", encoding="utf-8") as f:
        f.write(json.dumps(proj, indent=1, ensure_ascii=False) if not isinstance(proj, str) else proj)


def gain_db_to_value(db):
    return 10.0 ** (db / 20.0)


def main():
    out = sys.argv[1]
    nid = [100, 101, 102]
    # --- mapping law: macro 1 at 63.5 (position 0.5, straight curve) on Utility gain [-70 dB .. 35 dB] (log travel in
    # linear gain = linear in dB) must give -17.5 dB; macro 127 * 0.25 → -43.75 dB; curve 0.5 at macro 63.5 → shape(0.5)
    for name, macro, curve, mn, mx in [("map_half", 63.5, 0.0, 0.0, 1.0), ("map_quarter", 31.75, 0.0, 0.0, 1.0),
                                       ("map_inverted", 31.75, 0.0, 1.0, 0.0), ("map_curve", 63.5, 0.5, 0.0, 1.0)]:
        m = [{"id": 30, "macro": 1, "device": 20, "param": "gain", "min": mn, "max": mx, "curve": curve}]
        r = rack(21, [chain(22, [utility(20)])], m, params={"macro1": macro})
        write(out, name, project([midi_track(3, [SYNTH, r], 4, nid)]))
    for name, db in [("direct_m17p5", -17.5), ("direct_m43p75", -43.75), ("direct_p8p75", 8.75)]:
        write(out, name, project([midi_track(3, [SYNTH, utility(20, {"gain": gain_db_to_value(db)})], 4, nid)]))
    # rack transparency: one chain at 0 dB centre around a Utility at -6 dB vs the bare Utility at -6 dB
    write(out, "rackid_rack", project([midi_track(3, [SYNTH, rack(21, [chain(22, [utility(20, {"gain": 0.5})])])], 4, nid)]))
    write(out, "rackid_bare", project([midi_track(3, [SYNTH, utility(20, {"gain": 0.5})], 4, nid)]))
    # two chains, the second muted (inactive): must equal one chain
    write(out, "rackid_muted2", project([midi_track(3, [SYNTH, rack(21, [chain(22, [utility(20, {"gain": 0.5})]),
                                                                       chain(23, [utility(24)], active=False)])], 4, nid)]))
    # chain pan hard left (balance): right channel must be silent, left = centre level (balance law gl = min(1, 1 - pan))
    write(out, "rack_panL", project([midi_track(3, [SYNTH, rack(21, [chain(22, [utility(20, {"gain": 0.5})], pan=-1.0)])], 4, nid)]))

    # --- adversarial files
    base = project([midi_track(3, [SYNTH, rack(21, [chain(22, [utility(20)])])], 4, nid)])
    p = copy.deepcopy(base)  # a mapping onto a device outside its rack
    p["tracks"][0]["devices"][1]["rack"]["mappings"] = [{"id": 30, "macro": 1, "device": 10, "param": "cutoff", "min": 0, "max": 1}]
    write(out, "adv_map_outside", p)
    p = copy.deepcopy(base)  # a mapping onto a missing device
    p["tracks"][0]["devices"][1]["rack"]["mappings"] = [{"id": 30, "macro": 1, "device": 999, "param": "gain", "min": 0, "max": 1}]
    write(out, "adv_map_missing", p)
    p = copy.deepcopy(base)  # a mapping onto a param the device does not have (allowed: not lowered)
    p["tracks"][0]["devices"][1]["rack"]["mappings"] = [{"id": 30, "macro": 1, "device": 20, "param": "nosuch", "min": 0, "max": 1}]
    p["tracks"][0]["devices"][1]["params"] = {"macro1": 100}
    write(out, "adv_map_noparam", p)
    p = copy.deepcopy(base)  # a wire whose id is a device's id (NodeRole::Wire spans the other role spaces)
    p["wires"] = [{"id": 20, "from": 10, "port": "x", "to": 20, "param": "gain", "amount": 0.5}]
    write(out, "adv_wire_dupid", p)
    p = copy.deepcopy(base)  # a wire to a param that does not exist (Invalid, kept)
    p["wires"] = [{"id": 40, "from": 20, "port": "mid", "to": 20, "param": "nosuch", "amount": 0.5}]
    write(out, "adv_wire_noparam", p)
    p = copy.deepcopy(base)  # an instrument inside an Audio Effect Rack chain
    p["tracks"][0]["devices"][1]["rack"]["chains"][0]["devices"] = [{"id": 20, "patch": "BasicSynth", "kind": "instrument"}]
    write(out, "adv_inst_in_fxrack", p)
    p = copy.deepcopy(base)  # a rack with a "source"
    p["tracks"][0]["devices"][1]["source"] = ["patch AudioEffectRack {}"]
    write(out, "adv_rack_source", p)
    p = copy.deepcopy(base)  # a rack patch name without a rack payload
    p["tracks"][0]["devices"][1] = {"id": 21, "patch": "AudioEffectRack", "kind": "audioEffect"}
    write(out, "adv_rack_nopayload", p)
    p = copy.deepcopy(base)  # key zone on an Audio Effect Rack
    p["tracks"][0]["devices"][1]["rack"]["chains"][0]["key"] = [100, 100, 1000, 1000]
    write(out, "adv_keyzone_fxrack", p)
    p = copy.deepcopy(base)  # a zone out of order
    p["tracks"][0]["devices"][1]["rack"]["chains"][0]["select"] = [64, 10, 100, 127]
    write(out, "adv_zone_order", p)
    p = copy.deepcopy(base)  # macro 17
    p["tracks"][0]["devices"][1]["params"] = {"macro17": 3}
    write(out, "adv_macro17", p)
    p = copy.deepcopy(base)  # a chain id equal to a track id
    p["tracks"][0]["devices"][1]["rack"]["chains"][0]["id"] = 3
    write(out, "adv_chain_dupid", p)
    p = copy.deepcopy(base)  # a variation with 15 values
    p["tracks"][0]["devices"][1]["rack"]["variations"] = [{"name": "v", "values": [0] * 15}]
    write(out, "adv_variation15", p)
    p = copy.deepcopy(base)  # schema 4 file that already carries a rack (a field schema 4 does not have)
    p["schemaVersion"] = 4
    write(out, "adv_schema4_with_rack", p)
    # deep rack nesting: N Audio Effect Racks inside each other (no depth limit in the reader or the lowering)
    # (built as text: Python's json encoder recurses too)
    for depth in (64, 2000, 20000):
        inner = json.dumps(utility(20))
        for k in range(depth):
            i = 1000 + 2 * k
            inner = ('{"id":%d,"patch":"AudioEffectRack","kind":"audioEffect","rack":{"chains":[{"id":%d,"name":"C",'
                     '"devices":[%s]}]}}' % (i, i + 1, inner))
        proj = project([midi_track(3, [SYNTH, "@@INNER@@"], 4, nid)])
        text = json.dumps(proj, indent=1).replace('"@@INNER@@"', inner)
        write(out, "adv_nest_%d" % depth, text)


if __name__ == "__main__":
    main()
