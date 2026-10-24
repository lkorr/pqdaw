"""Writes the T3.11 piano-roll fixture projects (tests/views/fixtures/pianoroll-*/project.pqd).

Each project has one MIDI track with one clip; the view scenes of the same names open that clip in the clip view
(src/views/src/view_scenes.cpp) and the goldens in tests/views/golden/ are rendered from them.

    python tools/fixtures/make_pianoroll_fixtures.py

Deterministic: rerunning produces identical files. Ticks are written at PPQ 27,720 (schema 1); T0.5-fix1's
migration (PPQ 221,760, schema 2) must migrate these fixtures with the others (or PPQ below changes with it).
"""

import json
import os

PPQ = 27720
WHOLE = 4 * PPQ
ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "views", "fixtures")


def note(nid, start, length, pitch, velocity=100.0, chance=None, disabled=False):
    n = {"id": nid, "start": start, "length": length, "pitch": pitch, "velocity": float(velocity)}
    if chance is not None:
        n["chance"] = float(chance)
    if disabled:
        n["disabled"] = True
    return n


def deg(d, e=0):
    return {"degree": d, "equave": e}


def project(tuning_name, tuning_def, clip_name, colour, notes, length=2 * WHOLE, loop=None):
    loop = loop or {"enabled": True, "start": 0, "end": length}
    return {
        "schemaVersion": 1,
        "timeline": {
            "tempo": [{"tick": 0, "bpm": 120.0, "curve": "step"}],
            "signatures": [{"bar": 0, "numerator": 4, "denominator": 4}],
        },
        "loop": {"start": 0, "end": 4 * WHOLE, "enabled": False},
        "locators": [],
        "tunings": [{"id": 1, "name": tuning_name, "colour": "#5E9BD6", "def": tuning_def}],
        "tracks": [{
            "id": 3, "kind": "midi", "name": "1 MIDI", "colour": colour, "active": True, "tuning": "inherit",
            "mixer": {"volumeDb": 0.0, "pan": 0.0, "mute": False, "solo": False, "arm": False, "sends": [],
                      "output": "default"},
            "lanes": [],
            "clips": [{
                "id": 4, "name": clip_name, "colour": colour, "start": 0, "length": length, "contentStart": 0,
                "loop": loop, "signature": {"numerator": 4, "denominator": 4}, "tuning": "inherit", "active": True,
                "notes": notes,
            }],
        }],
        "returns": [],
        "master": {
            "id": 2, "kind": "master", "name": "Master", "colour": "#808080", "active": True, "tuning": "inherit",
            "mixer": {"volumeDb": 0.0, "pan": 0.0, "mute": False, "solo": False, "arm": False, "sends": [],
                      "output": "default"},
        },
    }


def edo(n):
    return {"kind": "edo", "n": n, "equave": "2/1", "referenceHz": 261.626}


def twelve():
    # C – Am – F – G (two beats each) over a bass line, a melody, a 5/4 (off-grid) and a −30 ¢ blue note, one muted.
    q, e, s = PPQ, PPQ // 2, PPQ // 4
    ns, nid = [], 100
    chords = [(0, [0, 4, 7]), (2 * q, [-3, 0, 4]), (4 * q, [-7, -3, 0]), (6 * q, [-5, -1, 2])]
    for t, ds in chords:
        for i, d in enumerate(ds):
            ns.append(note(nid, t, 2 * q - s, deg(d % 12, d // 12), 92 - 10 * i)); nid += 1
        ns.append(note(nid, t, q + e, deg((ds[0] - 12) % 12, (ds[0] - 12) // 12), 112)); nid += 1
    melody = [(0, 12, 120), (q, 14, 84), (q + e, 16, 70), (2 * q, 19, 127), (3 * q, 16, 60), (4 * q + e, 17, 96),
              (5 * q, 21, 76), (6 * q, 19, 110), (7 * q, 23, 52)]
    for t, d, v in melody:
        ns.append(note(nid, t, e if d % 2 else q - s, deg(d % 12, d // 12), v)); nid += 1
    ns.append(note(nid, 3 * q + e, e, {"ratio": "5/4"}, 100)); nid += 1          # 386.3 ¢: 13.7 ¢ below E
    ns.append(note(nid, 5 * q + e, q, {"cents": 1270.0}, 90)); nid += 1         # a blue note between D#4 and E4
    ns.append(note(nid, 7 * q + e, e, deg(11, 0), 80, disabled=True)); nid += 1
    ns.sort(key=lambda n: (n["start"], n["id"]))
    return project("12-EDO", edo(12), "Chords", "#E68632", ns)


def thirtyone():
    # 31edo: a 4:5:6:7 chord on 0 (0, 10, 18, 25\31), a neutral-third melody, a 7/4 and an 11/8 as ratios.
    q, e = PPQ, PPQ // 2
    ns, nid = [], 200
    for i, d in enumerate([0, 10, 18, 25]):
        ns.append(note(nid, 0, 2 * q, deg(d), 100 - 8 * i)); nid += 1
    for i, d in enumerate([-13, 5, 13, 23]):  # a subminor-flavoured chord a fourth up
        ns.append(note(nid, 2 * q, 2 * q, deg(d % 31, d // 31), 96 - 8 * i)); nid += 1
    mel = [(0, 31, 110), (e, 36, 74), (q, 40, 96), (q + e, 41, 66), (2 * q, 44, 120), (3 * q, 49, 80),
           (4 * q, 48, 104), (5 * q, 44, 90), (6 * q, 40, 70), (7 * q, 31, 127)]
    for t, d, v in mel:
        ns.append(note(nid, t, e, deg(d % 31, d // 31), v)); nid += 1
    for i, d in enumerate([-31, -13, 0]):
        ns.append(note(nid, 4 * q, 3 * q, deg(d % 31, d // 31), 88 - 6 * i)); nid += 1
    ns.append(note(nid, 6 * q, q, {"ratio": "7/4"}, 112)); nid += 1   # 968.8 ¢ vs 25\31 = 967.7 ¢
    ns.append(note(nid, 7 * q, q, {"ratio": "11/8"}, 100)); nid += 1  # 551.3 ¢: between 14\31 and 15\31
    ns.append(note(nid, 5 * q + e, e, {"cents": 1500.0}, 84)); nid += 1
    ns.sort(key=lambda n: (n["start"], n["id"]))
    return project("31-EDO", edo(31), "Septimal", "#46A5E8", ns)


def velocity():
    # A two-bar sixteenth run with a velocity ramp, accents and a few chance < 1 notes.
    s = PPQ // 4
    ns, nid = [], 300
    scale = [0, 2, 4, 5, 7, 9, 11, 12]
    for i in range(32):
        v = 8 + (119 * i) // 31
        if i % 4 == 0:
            v = min(127, v + 30)
        d = scale[(i * 3) % 8] + (12 if (i // 8) % 2 else 0)
        ns.append(note(nid, i * s, s, deg(d % 12, d // 12), v, chance=0.5 if i % 7 == 3 else None)); nid += 1
    ns.sort(key=lambda n: (n["start"], n["id"]))
    return project("12-EDO", edo(12), "Velocity Ramp", "#16B293", ns)


def mos5l2s(generator):
    # 5L 2s, Ionian (rotation 4 of the generator chain's Lydian), in its region 4\7 … 3\5 of the octave.
    return {"kind": "mos", "generator": generator, "n": 7, "period": 1200.0, "periodsPerEquave": 1, "rotation": 4,
            "modmos": [], "genSequence": [], "octaveStretch": 0.0,
            "region": {"perPeriodSize": 7, "lowCents": 1200.0 * 4 / 7, "highCents": 720.0},
            "referenceHz": 261.626}


def scale_tune():
    # A diatonic tune over two octaves: chords (I, vi, IV, V in scale degrees), a bass line, a melody, plus a 7/4
    # (off the scale) and a Cents note on 13\31 (503.2 ¢, a 31edo line between degrees 3 and 4).
    q, e = PPQ, PPQ // 2
    ns, nid = [], 400
    chords = [(0, [0, 2, 4]), (2 * q, [-2, 0, 2]), (4 * q, [-4, -2, 0]), (6 * q, [-3, -1, 1])]
    for t, ds in chords:
        for i, d in enumerate(ds):
            ns.append(note(nid, t, 2 * q - e // 2, deg(d % 7, d // 7), 96 - 10 * i)); nid += 1
        b = ds[0] - 7
        ns.append(note(nid, t, q + e, deg(b % 7, b // 7), 112)); nid += 1
    melody = [(0, 7, 120), (q, 8, 84), (q + e, 9, 70), (2 * q, 11, 127), (3 * q, 9, 60), (4 * q + e, 10, 96),
              (5 * q, 12, 76), (6 * q, 11, 110), (7 * q, 13, 70)]
    for t, d, v in melody:
        ns.append(note(nid, t, e if d % 2 else q - e // 2, deg(d % 7, d // 7), v)); nid += 1
    ns.append(note(nid, 3 * q + e, e, {"ratio": "7/4"}, 100)); nid += 1
    ns.append(note(nid, 5 * q + e, q, {"cents": 1200.0 * 13 / 31}, 90)); nid += 1
    ns.sort(key=lambda n: (n["start"], n["id"]))
    return ns


def mos_project(generator, clip_name):
    return project("5L 2s", mos5l2s(generator), clip_name, "#16B293", scale_tune())


def scala_project():
    just = {"kind": "scale", "description": "Just major (Ptolemy's intense diatonic)",
            "pitches": ["9/8", "5/4", "4/3", "3/2", "5/3", "15/8", "2/1"], "referenceHz": 261.626}
    return project("Just major", just, "Just Tune", "#8499F0", scale_tune())


def write(name, proj):
    d = os.path.join(ROOT, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "project.pqd"), "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(proj, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    write("pianoroll-12edo", twelve())
    write("pianoroll-31edo", thirtyone())
    write("pianoroll-velocity", velocity())
    # T3.12: 5L 2s at 18\31 (the -prop / -equi / -over-31edo scenes), near collapse (717 ¢: s = 9 ¢), a Scala scale.
    write("pianoroll-5L2s", mos_project(1200.0 * 18 / 31, "Meantone Tune"))
    write("pianoroll-5L2s-near-collapse", mos_project(717.0, "Near Collapse"))
    write("pianoroll-scala", scala_project())
