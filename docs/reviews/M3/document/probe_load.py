"""Probe 6: loader robustness through --render: hand-mutated project.pqd files must give clean errors, never a crash
or hang. Also schema-1 -> 2 migration with tricky values."""
import sys, os, json, time, copy
sys.path.insert(0, os.path.dirname(__file__))
from pqlib import *

Q = PPQ


def good():
    p = base_project()
    p["tracks"].append(midi([clip(0, 4 * Q, [note(0, Q)])], tid=50))
    p["tracks"][0]["clips"][0]["id"] = 60
    p["tracks"][0]["clips"][0]["notes"][0]["id"] = 70
    return p


def case(name, text, seconds=0.1, binary=False, timeout=30):
    d = write_text(text, name, binary)
    t0 = time.time()
    x, log, rc = render(d, seconds, timeout=timeout)
    dt = time.time() - t0
    crash = rc not in (0, 1)
    msg = log.splitlines()[-1] if log else ""
    print(f"{'CRASH' if crash else ('ok   ' if rc == 0 else 'error')} {name:38s} rc={rc} {dt:5.2f}s  {msg[:150]}")


def mutate(name, fn, **kw):
    p = good()
    r = fn(p)
    case(name, json.dumps(r if r is not None else p, indent=1), **kw)


base = json.dumps(good(), indent=1)
case("baseline", base)
case("truncated_half", base[: len(base) // 2])
case("truncated_1byte", base[:-1])
case("empty", "")
case("bom", "﻿" + base)
case("crlf", base.replace("\n", "\r\n"))
case("bad_utf8_in_name", base.replace('"name": "m"', '"name": "\xff\xfe"').encode("latin-1"), binary=True)
case("overlong_utf8", base.encode().replace(b'"name": "m"', b'"name": "\xc0\xaf"'), binary=True)
case("lone_surrogate_escape", base.replace('"name": "m"', '"name": "\\ud800"'))
case("nul_in_string", base.replace('"name": "m"', '"name": "a\\u0000b"'))
case("nan_literal", base.replace('"volumeDb": 0.0', '"volumeDb": NaN', 1))
case("inf_number", base.replace('"volumeDb": 0.0', '"volumeDb": 1e999', 1))
case("huge_int_tick", base.replace('"start": 0, "length": 221760', '"start": 0, "length": 99999999999999999999999'))
mutate("tick_kMaxTicks+1", lambda p: p["tracks"][0]["clips"][0].update(start=2 ** 52 + 1))
mutate("tick_kMaxTicks_exact_clip", lambda p: p["tracks"][0]["clips"][0].update(start=2 ** 52, length=2 ** 52),
       seconds=0.1)
mutate("note_start_int64_max", lambda p: p["tracks"][0]["clips"][0]["notes"][0].update(start=2 ** 63 - 1))
mutate("note_start_neg_kMax", lambda p: p["tracks"][0]["clips"][0]["notes"][0].update(start=-(2 ** 52)))
mutate("duplicate_track_id", lambda p: p["tracks"].append(midi([], tid=50)))
mutate("duplicate_note_id", lambda p: p["tracks"][0]["clips"][0]["notes"].append(dict(p["tracks"][0]["clips"][0]["notes"][0], start=10)))
mutate("note_id_equals_clip_id", lambda p: p["tracks"][0]["clips"][0]["notes"][0].update(id=60))
mutate("track_id_equals_note_id", lambda p: p["tracks"].append(midi([], tid=70)))
mutate("missing_tuning_ref", lambda p: p["tracks"][0].update(tuning={"main": 999}))
mutate("clip_missing_tuning_ref", lambda p: p["tracks"][0]["clips"][0].update(tuning={"main": 999}))
mutate("send_to_missing_return", lambda p: p["tracks"][0]["mixer"].update(sends=[{"target": 999, "levelDb": 0.0, "preFader": False}]))
mutate("output_to_missing_group", lambda p: p["tracks"][0]["mixer"].update(output={"track": 999}))
mutate("volume_7dB", lambda p: p["tracks"][0]["mixer"].update(volumeDb=7.0))
mutate("pan_nan_string", lambda p: p["tracks"][0]["mixer"].update(pan="NaN"))
mutate("bpm_0", lambda p: p["timeline"]["tempo"][0].update(bpm=0.0))
mutate("bpm_1000_ramp_to_1", lambda p: p["timeline"].update(tempo=[{"tick": 0, "bpm": 1000.0, "curve": "linear"}, {"tick": 1, "bpm": 1.0, "curve": "step"}]))
mutate("velocity_0", lambda p: p["tracks"][0]["clips"][0]["notes"][0].update(velocity=0.0))
mutate("velocity_200", lambda p: p["tracks"][0]["clips"][0]["notes"][0].update(velocity=200.0))
mutate("chance_2", lambda p: p["tracks"][0]["clips"][0]["notes"][0].update(chance=2.0))
mutate("edo_n_0", lambda p: p["tunings"][0]["def"].update(n=0))
mutate("edo_n_1e6", lambda p: p["tunings"][0]["def"].update(n=1000000))
mutate("ref_hz_negative", lambda p: p["tunings"][0]["def"].update(referenceHz=-1.0))
mutate("loop_len_1_tick_clip_huge", lambda p: p["tracks"][0]["clips"][0].update(
    length=2 ** 40, loop={"enabled": True, "start": 0, "end": 1}, notes=[dict(p["tracks"][0]["clips"][0]["notes"][0], length=1)]),
    seconds=2.0)


def nest(depth):
    p = good()
    t = p["tracks"].pop()
    for _ in range(depth):
        t = group([t])
    p["tracks"].append(t)
    return p


case("nested_groups_32", json.dumps(nest(31)))
case("nested_groups_33", json.dumps(nest(33)))
case("deep_array_100k", '{"schemaVersion": 2, "x": ' + "[" * 100000 + "]" * 100000 + "}")
case("deep_array_100k_v1_first_key_later", '{"x": ' + "[" * 100000 + "]" * 100000 + ', "schemaVersion": 1}')
case("schema_99", base.replace('"schemaVersion": 2', '"schemaVersion": 99'))
case("schema_0", base.replace('"schemaVersion": 2', '"schemaVersion": 0'))
case("schema_-1", base.replace('"schemaVersion": 2', '"schemaVersion": -1'))

# ---- schema 1 migration ---------------------------------------------------------------------------------------
def v1(p):
    p = copy.deepcopy(p)
    p["schemaVersion"] = 1
    return p


lim = (2 ** 63 - 1) // 8
mutate("v1_tick_overflow_x8", lambda p: v1(p) if False else (lambda q: (q["tracks"][0]["clips"][0].update(start=lim + 1), q)[1])(v1(p)))
mutate("v1_tick_beyond_kMax_after_x8", lambda p: (lambda q: (q["tracks"][0]["clips"][0].update(start=2 ** 52 // 8 + 1), q)[1])(v1(p)))
mutate("v1_negative_ticks", lambda p: (lambda q: (q["tracks"][0]["clips"][0].update(start=-27720, contentStart=-5), q)[1])(v1(p)))
mutate("v1_duplicate_key", None if False else (lambda p: None), )
case("v1_duplicate_key_text", json.dumps(v1(good()), indent=1).replace('"volumeDb": 0.0', '"volumeDb": 0.0, "volumeDb": -3.0', 1))
case("v2_duplicate_key_text", base.replace('"volumeDb": 0.0', '"volumeDb": 0.0, "volumeDb": -3.0', 1))
case("v1_float_tick", json.dumps(v1(good())).replace('"start": 0, "length": 221760', '"start": 0.5, "length": 221760'))
case("v1_float_velocity_precision", json.dumps(v1(good())).replace('"velocity": 100.0', '"velocity": 0.1'))
case("huge_int_clip_length", base.replace('"length": 887040', '"length": 99999999999999999999999', 1))
case("huge_neg_int", base.replace('"length": 887040', '"length": -99999999999999999999999', 1))
# no --seconds: the render length comes from the last clip end (+2 s)
_p = good()
_p["tracks"][0]["clips"][0].update(start=2 ** 52 - 887040)
case("far_clip_default_seconds", json.dumps(_p), seconds=None, timeout=20)
case("v1_real_fixture", open(os.path.join(WT, "tests", "document", "fixtures", "schema1", "project.pqd"), encoding="utf-8").read())
